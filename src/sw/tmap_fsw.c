#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

#include "../util/tmap_alloc.h"
#include "../util/tmap_progress.h"
#include "../util/tmap_definitions.h"
#include "../seq/tmap_sff.h"
#include "../io/tmap_file.h"
#include "tmap_sw.h"
#include "tmap_fsw.h"

/* TODO/IDEAS
   - do we need a transition probability added for going from one flow to the next?  For example, if we have a few flows that are empty, there is a certain probability of observing this... or this considered in your flow probabilities.  Basically, longer base alignments should be better, but using more flows is good/bad?
   - TODO: optimize memory usage by pre-allocating the cells/scores
   */

#define TMAP_FSW_ADD_FSCORE(s, f) (s).match_score -= f, (s).ins_score -= f, (s).del_score -= f

int32_t tmap_fsw_sm_short[] = {
    11*100, -19*100, -19*100, -19*100, -13*100,
    -19*100, 11*100, -19*100, -19*100, -13*100,
    -19*100, -19*100, 11*100, -19*100, -13*100,
    -19*100, -19*100, -19*100, 11*100, -13*100,
    -13*100, -13*100, -13*100, -13*100, -13*100
};

static inline void
tmap_fsw_set_match(tmap_fsw_dpcell_t **dpcell, tmap_fsw_dpscore_t **dpscore, 
                   int32_t i, int32_t j, 
                   int64_t score, int32_t sub_path) 
{ 
  if(dpscore[i-1][j-1].match_score >= dpscore[i-1][j-1].ins_score) {
      if(dpscore[i-1][j-1].match_score >= dpscore[i-1][j-1].del_score) {
          if(1 == sub_path) dpcell[i][j].match_from = TMAP_FSW_FROM_M | 4; 
          else dpcell[i][j].match_from = 4 + dpcell[i-1][j-1].match_from;
          dpcell[i][j].match_bc = 1 + dpcell[i-1][j-1].match_bc;
          dpscore[i][j].match_score = dpscore[i-1][j-1].match_score + score;
      } 
      else { 
          if(1 == sub_path) dpcell[i][j].match_from = TMAP_FSW_FROM_D | 4;
          else dpcell[i][j].match_from = 4 + dpcell[i-1][j-1].del_from; 
          dpcell[i][j].match_bc = 1 + dpcell[i-1][j-1].del_bc;
          dpscore[i][j].match_score = dpscore[i-1][j-1].del_score + score;
      }
  } else {
      if(dpscore[i-1][j-1].ins_score >= dpscore[i-1][j-1].del_score) {
          if(1 == sub_path) dpcell[i][j].match_from = TMAP_FSW_FROM_I | 4;
          else dpcell[i][j].match_from = 4 + dpcell[i-1][j-1].ins_from; 
          dpcell[i][j].match_bc = 1 + dpcell[i-1][j-1].ins_bc;
          dpscore[i][j].match_score = dpscore[i-1][j-1].ins_score + score;
      } 
      else {
          if(1 == sub_path) dpcell[i][j].match_from = TMAP_FSW_FROM_D | 4;
          else dpcell[i][j].match_from = 4 + dpcell[i-1][j-1].del_from; 
          dpcell[i][j].match_bc = 1 + dpcell[i-1][j-1].del_bc;
          dpscore[i][j].match_score = dpscore[i-1][j-1].del_score + score;
      }
  }
}

static inline void 
tmap_fsw_set_ins(tmap_fsw_dpcell_t **dpcell, tmap_fsw_dpscore_t **dpscore, 
                 int32_t i, int32_t j, 
                 int32_t gap_open, int32_t gap_ext, int32_t sub_path) 
{
  if(dpscore[i-1][j].match_score - gap_open > dpscore[i-1][j].ins_score) {
      if(1 == sub_path) dpcell[i][j].ins_from = TMAP_FSW_FROM_M;
      else dpcell[i][j].ins_from = dpcell[i-1][j].match_from;
      dpcell[i][j].ins_bc = 1 + dpcell[i-1][j].match_bc;
      dpscore[i][j].ins_score = dpscore[i-1][j].match_score - gap_open - gap_ext;
  } else {
      if(1 == sub_path) dpcell[i][j].ins_from = TMAP_FSW_FROM_I;
      else dpcell[i][j].ins_from = dpcell[i-1][j].ins_from;
      dpcell[i][j].ins_bc = 1 + dpcell[i-1][j].ins_bc;
      dpscore[i][j].ins_score = dpscore[i-1][j].ins_score - gap_ext;
  }
}

static inline void 
tmap_fsw_set_end_ins(tmap_fsw_dpcell_t **dpcell, tmap_fsw_dpscore_t **dpscore, 
                     int32_t i, int32_t j, 
                     int32_t gap_open, int32_t gap_ext, int32_t gap_end, int32_t sub_path) 
{
  if(gap_end >= 0) {
      tmap_fsw_set_ins(dpcell, dpscore, i, j, gap_open, gap_end, sub_path);
  }
  else {
      tmap_fsw_set_ins(dpcell, dpscore, i, j, gap_open, gap_ext, sub_path);
  }
}

static inline void 
tmap_fsw_set_del(tmap_fsw_dpcell_t **dpcell, tmap_fsw_dpscore_t **dpscore, 
                 int32_t i, int32_t j, 
                 int32_t gap_open, int32_t gap_ext, int32_t sub_path) 
{
  if(dpscore[i][j-1].match_score - gap_open > dpscore[i][j-1].del_score) {
      if(1 == sub_path) dpcell[i][j].del_from = TMAP_FSW_FROM_M | 4;
      else dpcell[i][j].del_from = 4 + dpcell[i][j-1].match_from;
      dpcell[i][j].del_bc = dpcell[i][j-1].match_bc;
      dpscore[i][j].del_score = dpscore[i][j-1].match_score - gap_open - gap_ext;
  } else {
      if(1 == sub_path) dpcell[i][j].del_from = TMAP_FSW_FROM_D | 4;
      else dpcell[i][j].del_from = 4 + dpcell[i][j-1].del_from;
      dpcell[i][j].del_bc = dpcell[i][j-1].del_bc;
      dpscore[i][j].del_score = dpscore[i][j-1].del_score - gap_ext;
  }
}

static inline void 
tmap_fsw_set_end_del(tmap_fsw_dpcell_t **dpcell, tmap_fsw_dpscore_t **dpscore, 
                     int32_t i, int32_t j, 
                     int32_t gap_open, int32_t gap_ext, int32_t gap_end, int32_t sub_path) 
{
  if(gap_end >= 0) {
      tmap_fsw_set_del(dpcell, dpscore, i, j, gap_open, gap_end, sub_path);
  }
  else {
      tmap_fsw_set_del(dpcell, dpscore, i, j, gap_open, gap_ext, sub_path);
  }
}

inline void
tmap_fsw_sub_core(uint8_t *seq, int32_t len,
                  uint8_t flow_base, uint8_t base_call, uint16_t flow_signal,
                  const tmap_fsw_param_t *ap,
                  tmap_fsw_dpcell_t **sub_dpcell,
                  tmap_fsw_dpscore_t **sub_dpscore, 
                  tmap_fsw_dpcell_t *dpcell_last,
                  tmap_fsw_dpscore_t *dpscore_last,
                  tmap_fsw_dpcell_t *dpcell_curr,
                  tmap_fsw_dpscore_t *dpscore_curr,
                  tmap_fsw_path_t *path, int32_t *path_len, int32_t best_ctype,
                  uint8_t key_bases,
                  int32_t type)
{
  register int32_t i, j;
  int32_t low_offset, high_offset, flow_score;
  int32_t gap_open, gap_ext, gap_end, bw;
  int32_t *mat, *score_matrix, N_MATRIX_ROW;
  uint8_t offset;
  int32_t num_bases; 
  int32_t sub_path;

  gap_open = ap->gap_open;
  gap_ext = ap->gap_ext;
  gap_end = ap->gap_end;
  bw = ap->band_width;
  score_matrix = ap->matrix;
  N_MATRIX_ROW = ap->row;
  offset = ap->offset;

  // get the homopolymer bounds
  low_offset = (base_call < offset) ? 0 : base_call - offset;
  high_offset = base_call + offset;

  //fprintf(stderr, "base_call=%d low_offset=%d high_offset=%d\n", base_call, low_offset, high_offset);

  // scoring matrix will always be the same
  mat = score_matrix + flow_base * N_MATRIX_ROW;

  sub_path = (NULL == path) ? 0 : 1; // only if we wish to recover the path  

  // copy previous row
  for(j=0;j<=len;j++) {
      sub_dpscore[0][j] = dpscore_last[j];
      TMAP_FSW_INIT_CELL(sub_dpcell[0][j]); // empty
      sub_dpcell[0][j].match_from = TMAP_FSW_FROM_M;
      sub_dpcell[0][j].ins_from = TMAP_FSW_FROM_I;
      sub_dpcell[0][j].del_from = TMAP_FSW_FROM_D;
      sub_dpcell[0][j].match_bc = sub_dpcell[0][j].ins_bc = sub_dpcell[0][j].del_bc = 0;
  }

  // fill in sub_dpcell and sub_dpscore
  for(i=1;i<=high_offset;i++) { // for each row in the sub-alignment
      // initialize the first column
      TMAP_FSW_SET_SCORE_INF(sub_dpscore[i][0]); 
      TMAP_FSW_INIT_CELL(sub_dpcell[i][0]);
      tmap_fsw_set_end_ins(sub_dpcell, sub_dpscore, i, 0, gap_open, gap_ext, gap_end, sub_path);
      // fill in the rest of the columns
      for(j=1;j<=len;j++) { // for each col
          // HERE
          //if(1 == i) fprintf(stderr, "j=%d flow_base=%d seq[j-1]=%d mat[seq[j-1]]=%d\n", j, flow_base, seq[j-1], mat[seq[j-1]]); 
          tmap_fsw_set_match(sub_dpcell, sub_dpscore, i, j, mat[seq[j-1]], sub_path);
          tmap_fsw_set_ins(sub_dpcell, sub_dpscore, i, j, gap_open, gap_ext, sub_path);
          tmap_fsw_set_del(sub_dpcell, sub_dpscore, i, j, gap_open, gap_ext, sub_path);
      }
  }

  /*
     fprintf(stderr, "BEFORE\n");
     for(i=low_offset;i<=high_offset;i++) { // for each row in the sub-alignment
     for(j=0;j<=len;j++) { // for each col
     fprintf(stderr, "(%d,%d M[%d,%d,%d,%d] I[%d,%d,%d,%d] D[%d,%d,%d,%d])\n",
     i, j,
     sub_dpcell[i][j].match_bc, sub_dpcell[i][j].match_from & 0x3, sub_dpcell[i][j].match_from >> 2, (int)sub_dpscore[i][j].match_score,
     sub_dpcell[i][j].ins_bc, sub_dpcell[i][j].ins_from & 0x3, sub_dpcell[i][j].ins_from >> 2, (int)sub_dpscore[i][j].ins_score,
     sub_dpcell[i][j].del_bc, sub_dpcell[i][j].del_from & 0x3, sub_dpcell[i][j].del_from >> 2, (int)sub_dpscore[i][j].del_score);
     }
     }
     */

  // add flow scores
  for(i=low_offset;i<=high_offset;i++) { // for each possible base call within +-offset
      // get flow score for "(i-low_offset)" bases
      num_bases = i + key_bases; // key bases will shift this
      flow_score = (flow_signal < 100*num_bases) ? (100*num_bases - flow_signal) : (flow_signal - 100*num_bases);
      flow_score *= ap->fscore / 100;
      // factor out extra match scores for over calls.  Is this correct?
      //if(base_call < i) flow_score += mat[flow_base] * (i - base_call);
      //fprintf(stderr, "flow_score=%d i=%d\n", flow_score, i);
      for(j=0;j<=len;j++) { // for each col
          TMAP_FSW_ADD_FSCORE(sub_dpscore[i][j], flow_score);
      }
  }


  if(NULL != dpcell_curr && NULL != dpscore_curr) {
      /*
         fprintf(stderr, "AFTER\n");
         for(i=low_offset;i<=high_offset;i++) { // for each row in the sub-alignment
         for(j=0;j<=len;j++) { // for each col
         fprintf(stderr, "(%d,%d M[%d,%d,%d,%d] I[%d,%d,%d,%d] D[%d,%d,%d,%d])\n",
         i, j,
         sub_dpcell[i][j].match_bc, sub_dpcell[i][j].match_from & 0x3, sub_dpcell[i][j].match_from >> 2, (int)sub_dpscore[i][j].match_score,
         sub_dpcell[i][j].ins_bc, sub_dpcell[i][j].ins_from & 0x3, sub_dpcell[i][j].ins_from >> 2, (int)sub_dpscore[i][j].ins_score,
         sub_dpcell[i][j].del_bc, sub_dpcell[i][j].del_from & 0x3, sub_dpcell[i][j].del_from >> 2, (int)sub_dpscore[i][j].del_score);
         }
         }
         */
      // set the best cell to be [base_call][0,len]
      // NOTE: set this to the original base call to get consistency between
      // calling homopolymer over/under calls
      for(j=0;j<=len;j++) { // for each col
          dpcell_curr[j] = sub_dpcell[base_call][j];
          dpscore_curr[j] = sub_dpscore[base_call][j];
          if(TMAP_SW_TYPE_LOCAL == type) { // local alignment
              if(dpscore_curr[j].match_score < 0) {
                  dpcell_curr[j].match_from = TMAP_FSW_FROM_S;
                  dpcell_curr[j].match_bc = 0;
                  dpscore_curr[j].match_score = 0;
              }
              if(dpscore_curr[j].ins_score < 0) {
                  dpcell_curr[j].ins_from = TMAP_FSW_FROM_S;
                  dpcell_curr[j].ins_bc = 0;
                  dpscore_curr[j].ins_score = 0;
              }
              if(dpscore_curr[j].del_score < 0) {
                  dpcell_curr[j].del_from = TMAP_FSW_FROM_S;
                  dpcell_curr[j].del_bc = 0;
                  dpscore_curr[j].del_score = 0;
              }
          }
      }

      // get the best cells within [low_offset+1,high_offset][0,len]
      for(i=low_offset;i<=high_offset;i++) {
          for(j=0;j<=len;j++) { // for each col
              // match
              if(dpscore_curr[j].match_score < sub_dpscore[i][j].match_score) {
                  dpcell_curr[j].match_from = sub_dpcell[i][j].match_from;
                  dpcell_curr[j].match_bc = sub_dpcell[i][j].match_bc;
                  dpscore_curr[j].match_score = sub_dpscore[i][j].match_score;
              }
              // ins
              if(dpscore_curr[j].ins_score < sub_dpscore[i][j].ins_score) {
                  dpcell_curr[j].ins_from = sub_dpcell[i][j].ins_from;
                  dpcell_curr[j].ins_bc = sub_dpcell[i][j].ins_bc;
                  dpscore_curr[j].ins_score = sub_dpscore[i][j].ins_score;
              }
              // del
              if(dpscore_curr[j].del_score < sub_dpscore[i][j].del_score) {
                  dpcell_curr[j].del_from = sub_dpcell[i][j].del_from;
                  dpcell_curr[j].del_bc = sub_dpcell[i][j].del_bc;
                  dpscore_curr[j].del_score = sub_dpscore[i][j].del_score;
              }
          }
      }
      /*
         fprintf(stderr, "STORED\n");
         for(j=0;j<=len;j++) { // for each col
         fprintf(stderr, "(%d M[%d,%d,%d,%d] I[%d,%d,%d,%d] D[%d,%d,%d,%d])\n",
         j,
         dpcell_curr[j].match_bc, dpcell_curr[j].match_from & 0x3, dpcell_curr[j].match_from >> 2, (int)dpscore_curr[j].match_score,
         dpcell_curr[j].ins_bc, dpcell_curr[j].ins_from & 0x3, dpcell_curr[j].ins_from >> 2, (int)dpscore_curr[j].ins_score,
         dpcell_curr[j].del_bc, dpcell_curr[j].del_from & 0x3, dpcell_curr[j].del_from >> 2, (int)dpscore_curr[j].del_score);
         }
         */
  }

  if(NULL != path) {
      int32_t ctype, ctype_next = 0;
      tmap_fsw_path_t *p;

      p = path;
      i = high_offset; j = len; 
      ctype = best_ctype;

      //fprintf(stderr, "  START sub_core i=%d j=%d ctype=%d\n", i, j, ctype);
      while(TMAP_FSW_FROM_S != ctype && 0 < i) {
          //fprintf(stderr, "  sub_core i=%d j=%d ctype=%d\n", i, j, ctype);
          if(i < 0 || j < 0) tmap_error("bug encountered", Exit, OutOfRange);

          switch(ctype) { 
            case TMAP_FSW_FROM_M: 
              ctype_next = sub_dpcell[i][j].match_from & 0x3;
              break;
            case TMAP_FSW_FROM_I: 
              ctype_next = sub_dpcell[i][j].ins_from & 0x3;
              break;
            case TMAP_FSW_FROM_D: 
              ctype_next = sub_dpcell[i][j].del_from & 0x3;
              break;
            default:
              tmap_error(NULL, Exit, OutOfRange);
          }
          p->ctype = ctype;
          p->i = i-1;
          p->j = j-1;
          p++;

          // move the row and column (as necessary)
          switch(ctype) {
            case TMAP_FSW_FROM_M: 
              --i; --j; break;
            case TMAP_FSW_FROM_I: 
              --i; break;
            case TMAP_FSW_FROM_D: 
              --j; break;
            default:
              tmap_error(NULL, Exit, OutOfRange);
          }

          // move to the next cell type
          ctype = ctype_next;
          //fprintf(stderr, "  sub_core i=%d j=%d ctype_next=%d\n", i, j, ctype_next);
      }

      (*path_len) = p - path;
  }
  else if(NULL != path_len) {
      (*path_len) = 0;
  }
}

static void
tmap_fsw_get_path(uint8_t *seq, uint8_t *flow, uint8_t *base_calls, uint16_t *flowgram,
                  int32_t key_index, int32_t key_bases,
                  tmap_fsw_dpcell_t **dpcell, tmap_fsw_dpscore_t **dpscore,
                  tmap_fsw_dpcell_t **sub_dpcell,
                  tmap_fsw_dpscore_t **sub_dpscore, 
                  const tmap_fsw_param_t *ap,
                  int32_t best_i, int32_t best_j, uint8_t best_ctype, int32_t type,
                  tmap_fsw_path_t *path, int32_t *path_len)
{
  register int32_t i, j;
  int32_t k, l;
  int32_t base_call = 0, col_offset = 0, base_call_diff = 0;
  uint8_t ctype, ctype_next = 0;
  tmap_fsw_path_t *p;
  tmap_fsw_path_t *sub_path = NULL;
  int32_t sub_path_len = 0, sub_path_mem = 0;

  /*
     fprintf(stderr, "PRINTING\n");
     for(i=0;i<=best_i;i++) {
     for(j=0;j<=best_j;j++) {
     fprintf(stderr, "(%d,%d M[%d,%d,%d,%d] I[%d,%d,%d,%d] D[%d,%d,%d,%d])\n",
     i, j,
     dpcell[i][j].match_bc, dpcell[i][j].match_from & 0x3, dpcell[i][j].match_from >> 2, (int)dpscore[i][j].match_score,
     dpcell[i][j].ins_bc, dpcell[i][j].ins_from & 0x3, dpcell[i][j].ins_from >> 2, (int)dpscore[i][j].ins_score,
     dpcell[i][j].del_bc, dpcell[i][j].del_from & 0x3, dpcell[i][j].del_from >> 2, (int)dpscore[i][j].del_score);
     }
     }
     */

  // get best scoring end cell
  i = best_i; j = best_j; p = path;
  ctype = best_ctype;

  /*
     fprintf(stderr, "GETTING PATH\n");
     fprintf(stderr, "best_i=%d best_j=%d ctype=%d\n",
     best_i, best_j, ctype);
     */

  while(TMAP_FSW_FROM_S != ctype && (0 < i || 0 < j)) {
      base_call = 0;
      col_offset = 0;
      base_call_diff = 0;

      // local
      if(i <= 0
         && (TMAP_SW_TYPE_LOCAL == type || TMAP_SW_TYPE_FITTING == type)) {
          break;
      }

      //fprintf(stderr, "CORE i=%d j=%d ctype=%d\n", i, j, ctype);
      switch(ctype) { 
        case TMAP_FSW_FROM_M: 
          if(i < 0 || j < 0) tmap_error("bug encountered", Exit, OutOfRange);
          base_call = dpcell[i][j].match_bc;
          col_offset = dpcell[i][j].match_from >> 2;
          ctype_next = dpcell[i][j].match_from & 0x3;
          break;
        case TMAP_FSW_FROM_I: 
          if(i < 0 || j < 0) tmap_error("bug encountered", Exit, OutOfRange);
          base_call = dpcell[i][j].ins_bc;
          col_offset = dpcell[i][j].ins_from >> 2;
          ctype_next = dpcell[i][j].ins_from & 0x3;
          break;
        case TMAP_FSW_FROM_D: 
          if(i < 0 || j < 0) tmap_error("bug encountered", Exit, OutOfRange);
          base_call = dpcell[i][j].del_bc;
          col_offset = dpcell[i][j].del_from >> 2;
          ctype_next = dpcell[i][j].del_from & 0x3;
          break;
        default:
          tmap_error(NULL, Exit, OutOfRange);
      }

      /*
         fprintf(stderr, "base_call=%d col_offset=%d ctype_next=%d\n",
         base_call, col_offset, ctype_next);
         fprintf(stderr, "base_calls[%d]=%d\n", i-1, (0 < i) ? base_calls[i-1] : 0);
         */

      if(j <= 0) {
          // starts with an insertion?
          // TODO
          for(k=0;k<base_call;k++) {
              p->i = i-1;
              p->j = j-1;
              p->ctype = TMAP_FSW_FROM_I;
              p++;
          }
          i--;
      }
      else if(i <= 0) {
          // starts with a deletion?
          p->i = i-1;
          p->j = j-1;
          p->ctype = TMAP_FSW_FROM_D;
          p++;
          j--;
      }
      else if(0 == base_call) {
          if(0 < i && 0 < base_calls[i-1]) {
              for(k=0;k<base_calls[i-1];k++) {
                  p->j = j - 1;
                  p->i = i - 1;
                  p->ctype = TMAP_FSW_FROM_HP_MINUS;
                  p++;
              }
          }
          i--;
          j -= col_offset;
      }
      else {
          while(sub_path_mem < j + ap->offset + 1) { // more memory please
              sub_path_mem = (0 == sub_path_mem) ? 4 : (sub_path_mem << 1);
              sub_path = tmap_realloc(sub_path, sizeof(tmap_fsw_path_t) * sub_path_mem, "sub_path"); 
          } 

          tmap_fsw_param_t ap_tmp;
          ap_tmp = (*ap);
          ap_tmp.offset = 0; // no offset, since we will use the previous base call

          // solve the sub-problem and get the path
          if(j - col_offset < 0) tmap_error("bug encountered", Exit, OutOfRange);
          tmap_fsw_sub_core(seq, j,
                            flow[(i-1) & 3], base_call, flowgram[i-1], // Note: %4 is the same as &3
                            &ap_tmp,
                            sub_dpcell, sub_dpscore,
                            dpcell[i-1], dpscore[i-1],
                            NULL, NULL, // do not update
                            sub_path, &sub_path_len, ctype, // get the path
                            ((key_index+1) == i) ? key_bases : 0,
                            ctype);

          base_call_diff = base_call - base_calls[i-1];
          // if base_call_diff > 0, add insertions
          // if base_call_diff < 0, add deletions

          k = j - 1; // for base_call_diff < 0
          //fprintf(stderr, "sub_path_len=%d base_call_diff=%d\n", sub_path_len, base_call_diff);
          for(l=0;l<sub_path_len;l++) {
              (*p) = sub_path[l]; // store p->j and p->ctype
              p->i = i - 1; // same flow index
              k = p->j; // for base_call_diff < 0


              if(0 < base_call_diff // there are bases left that were inserted
                 && TMAP_FSW_FROM_M == sub_path[l].ctype) { // delete a "match"
                  // change the type to a deletion
                  p->ctype = TMAP_FSW_FROM_HP_PLUS;
                  base_call_diff--;
              }

              p++;
          }
          while(base_call_diff < 0) { // there are bases left that were deleted
              p->ctype = TMAP_FSW_FROM_HP_MINUS; // add an insertion
              p->i = i - 1;
              p++;
              base_call_diff++;
          }

          // move the row and column (as necessary)
          i--;
          j -= col_offset;
      }

      // move to the next cell type
      ctype = ctype_next;

      //fprintf(stderr, "HERE i=%d j=%d ctype=%d\n", i, j, ctype);
  }
  (*path_len) = p - path;

  free(sub_path);
}

/*
Notes: key_index is zero-base and should be -1, 0, or (num_flows-1)
*/
static int64_t
tmap_fsw_stdaln_aux(uint8_t *seq, int32_t len, 
                    tmap_fsw_flowseq_t *flowseq,
                    const tmap_fsw_param_t *ap,
                    tmap_fsw_path_t *path, int32_t *path_len, 
                    int32_t type, int32_t prev_score,
                    int32_t _thres, int32_t *_subo)
{
  register int32_t i, j;
  int32_t max_bc = 0, bw;

  // main cells 
  tmap_fsw_dpcell_t **dpcell;
  tmap_fsw_dpscore_t **dpscore;

  // for homopolymer re-calling 
  tmap_fsw_dpcell_t **sub_dpcell;
  tmap_fsw_dpscore_t **sub_dpscore;

  int32_t gap_open, gap_ext, gap_end;
  int32_t *score_matrix, N_MATRIX_ROW;
  uint8_t offset;

  int32_t best_i=-1, best_j=-1;
  uint8_t best_ctype=0;
  int64_t best_score = TMAP_SW_MINOR_INF;

  if(0 == flowseq->num_flows || 0 == len) {
      (*path_len) = 0;
      return 0;
  }

  gap_open = ap->gap_open;
  gap_ext = ap->gap_ext;
  gap_end = ap->gap_end;
  bw = ap->band_width;
  score_matrix = ap->matrix;
  N_MATRIX_ROW = ap->row;
  offset = ap->offset;

  // maximum length base call
  for(i=0;i<flowseq->num_flows;i++) {
      if(max_bc < flowseq->base_calls[i]) {
          max_bc = flowseq->base_calls[i];
      }
  }

  //fprintf(stderr, "max_bc=%d offset=%d len=%d\n", max_bc, offset, len);

  // allocate memory for the sub-cells
  sub_dpcell = tmap_malloc(sizeof(tmap_fsw_dpcell_t*) * (max_bc + offset + 1), "sub_dpcell");
  sub_dpscore = tmap_malloc(sizeof(tmap_fsw_dpscore_t*) * (max_bc + offset + 1), "sub_dpscore");
  for(i=0;i<=max_bc+offset;i++) {
      sub_dpcell[i] = tmap_malloc(sizeof(tmap_fsw_dpcell_t) * (len + 1), "sub_dpcell");
      sub_dpscore[i] = tmap_malloc(sizeof(tmap_fsw_dpscore_t) * (len + 1), "sub_dpscore");
  }

  // allocate memory for the main cells
  dpcell = tmap_malloc(sizeof(tmap_fsw_dpcell_t*) * (flowseq->num_flows + 1), "dpcell");
  dpscore = tmap_malloc(sizeof(tmap_fsw_dpscore_t*) * (flowseq->num_flows + 1), "dpscore");
  for(i=0;i<=flowseq->num_flows;i++) {
      dpcell[i] = tmap_malloc(sizeof(tmap_fsw_dpcell_t) * (len + 1), "dpcell");
      dpscore[i] = tmap_malloc(sizeof(tmap_fsw_dpscore_t) * (len + 1), "dpscore");
  }

  // set first row
  TMAP_FSW_SET_SCORE_INF(dpscore[0][0]); 
  TMAP_FSW_INIT_CELL(dpcell[0][0]);
  if(TMAP_SW_TYPE_EXTEND == type
     || TMAP_SW_TYPE_EXTEND_FITTING == type) {
      dpscore[0][0].match_score = prev_score;
  }
  else {
      dpscore[0][0].match_score = 0;
  }
  for(j=1;j<=len;j++) { // for each col
      TMAP_FSW_SET_SCORE_INF(dpscore[0][j]);
      TMAP_FSW_INIT_CELL(dpcell[0][j]);
      switch(type) {
        case TMAP_SW_TYPE_LOCAL:
        case TMAP_SW_TYPE_FITTING:
          // the alignment can start anywhere with seq 
          dpscore[0][j].match_score = 0; 
          break;
        case TMAP_SW_TYPE_GLOBAL:
        case TMAP_SW_TYPE_EXTEND:
        case TMAP_SW_TYPE_EXTEND_FITTING:
          tmap_fsw_set_end_del(dpcell, dpscore, 0, j, gap_open, gap_ext, gap_end, 0);
          break;
        default:
          tmap_error("alignment type not understood", Exit, OutOfRange);
      }
  }

  // core loop
  for(i=1;i<=flowseq->num_flows;i++) { // for each row
      // fill in the columns
      //fprintf(stderr, "i=%d num_flows=%d\n", i, flowseq->num_flows);
      tmap_fsw_sub_core(seq, len,
                        flowseq->flow[(i-1) & 3], 
                        flowseq->base_calls[i-1], 
                        flowseq->flowgram[i-1], // Note: %4 is the same as &3
                        ap,
                        sub_dpcell, sub_dpscore,
                        dpcell[i-1], dpscore[i-1],
                        dpcell[i], dpscore[i],
                        NULL, NULL, 0,
                        ((flowseq->key_index+1) == i) ? flowseq->key_bases : 0,
                        type);

      // Update best
      switch(type) {
        case TMAP_SW_TYPE_LOCAL:
        case TMAP_SW_TYPE_EXTEND:
          for(j=1;j<=len;j++) {
              if(best_score < dpscore[i][j].match_score) {
                  best_score = dpscore[i][j].match_score;
                  best_ctype = TMAP_FSW_FROM_M;
                  best_i = i; best_j = j;
              }
              if(best_score < dpscore[i][j].ins_score) {
                  best_score = dpscore[i][j].ins_score;
                  best_ctype = TMAP_FSW_FROM_I;
                  best_i = i; best_j = j;
              }
              if(best_score < dpscore[i][j].del_score) {
                  best_score = dpscore[i][j].del_score;
                  best_ctype = TMAP_FSW_FROM_D;
                  best_i = i; best_j = j;
              }
          }
          break;
        case TMAP_SW_TYPE_GLOBAL:
          if(i == flowseq->num_flows) {
              if(best_score < dpscore[i][len].match_score) {
                  best_score = dpscore[i][len].match_score;
                  best_ctype = TMAP_FSW_FROM_M;
                  best_i = i; best_j = len;
              }
              if(best_score < dpscore[i][len].ins_score) {
                  best_score = dpscore[i][len].ins_score;
                  best_ctype = TMAP_FSW_FROM_I;
                  best_i = i; best_j = len;
              }
              if(best_score < dpscore[i][len].del_score) {
                  best_score = dpscore[i][len].del_score;
                  best_ctype = TMAP_FSW_FROM_D;
                  best_i = i; best_j = len;
              }
          }
          break;
        case TMAP_SW_TYPE_EXTEND_FITTING:
          if(flowseq->num_flows == i) {
              for(j=1;j<=len;j++) {
                  if(best_score < dpscore[i][j].match_score) {
                      best_score = dpscore[i][j].match_score;
                      best_ctype = TMAP_FSW_FROM_M;
                      best_i = i; best_j = j;
                  }
                  if(best_score < dpscore[i][j].ins_score) {
                      best_score = dpscore[i][j].ins_score;
                      best_ctype = TMAP_FSW_FROM_I;
                      best_i = i; best_j = j;
                  }
                  if(best_score < dpscore[i][j].del_score) {
                      best_score = dpscore[i][j].del_score;
                      best_ctype = TMAP_FSW_FROM_D;
                      best_i = i; best_j = j;
                  }
              }
          }
          break;
      }
  }

  if(best_i < 0 || best_j < 0) { // was not updated
      (*path_len) = 0;
      return 0;
  }

  if(NULL == path) { 
      // do nothing
  }
  else if(NULL == path_len) {
      path[0].i = best_i; path[0].j = best_j;
  }
  else if(best_score <= _thres) {
      (*path_len) = 0;
  }
  else {
      // recover the path
      tmap_fsw_get_path(seq, flowseq->flow, flowseq->base_calls, flowseq->flowgram,
                        flowseq->key_index, flowseq->key_bases,
                        dpcell, dpscore, 
                        sub_dpcell, sub_dpscore, 
                        ap, 
                        best_i, best_j, best_ctype, type, 
                        path, path_len);
  }

  // free memory for the sub-cells
  for(i=0;i<=max_bc+offset;i++) {
      free(sub_dpcell[i]);
      free(sub_dpscore[i]);
  }
  free(sub_dpcell);
  free(sub_dpscore);

  // free memory for the main cells
  for(i=0;i<=flowseq->num_flows;i++) {
      free(dpcell[i]);
      free(dpscore[i]);
  }
  free(dpcell);
  free(dpscore);

  return best_score;
}

tmap_fsw_flowseq_t *
tmap_fsw_flowseq_init(uint8_t *flow, uint8_t *base_calls, uint16_t *flowgram,
                                          int32_t num_flows, int32_t key_index, int32_t key_bases)
{
  tmap_fsw_flowseq_t *flowseq;

  flowseq = tmap_calloc(1, sizeof(tmap_fsw_flowseq_t), "flowseq");

  flowseq->flow = flow;
  flowseq->base_calls = base_calls;
  flowseq->flowgram = flowgram;
  flowseq->num_flows = num_flows;
  flowseq->key_index = key_index;
  flowseq->key_bases = key_bases;

  return flowseq;
}

void
tmap_fsw_flowseq_destroy(tmap_fsw_flowseq_t *flowseq)
{
  free(flowseq);
}

int64_t
tmap_fsw_global_core(uint8_t *seq, int32_t len, 
                     tmap_fsw_flowseq_t *flowseq,
                     const tmap_fsw_param_t *ap,
                     tmap_fsw_path_t *path, int32_t *path_len)
{
  return tmap_fsw_stdaln_aux(seq, len, flowseq,
                             ap, path, path_len, TMAP_SW_TYPE_GLOBAL, 0,
                             INT32_MIN, NULL);
}

int64_t
tmap_fsw_local_core(uint8_t *seq, int32_t len, 
                    tmap_fsw_flowseq_t *flowseq,
                    const tmap_fsw_param_t *ap,
                    tmap_fsw_path_t *path, int32_t *path_len, int32_t _thres, int32_t *_subo)
{
  return tmap_fsw_stdaln_aux(seq, len, flowseq,
                             ap, path, path_len, TMAP_SW_TYPE_GLOBAL, 0,
                             _thres, _subo);
}

int64_t
tmap_fsw_extend_core(uint8_t *seq, int32_t len, 
                     tmap_fsw_flowseq_t *flowseq,
                     const tmap_fsw_param_t *ap,
                     tmap_fsw_path_t *path, int32_t *path_len, int32_t prev_score)
{
  return tmap_fsw_stdaln_aux(seq, len, flowseq,
                             ap, path, path_len, TMAP_SW_TYPE_EXTEND, prev_score,
                             0, NULL);
}

int64_t
tmap_fsw_extend_fitting_core(uint8_t *seq, int32_t len, 
                             tmap_fsw_flowseq_t *flowseq,
                             const tmap_fsw_param_t *ap,
                             tmap_fsw_path_t *path, int32_t *path_len, int32_t prev_score)
{
  return tmap_fsw_stdaln_aux(seq, len, flowseq,
                             ap, path, path_len, TMAP_SW_TYPE_EXTEND_FITTING, prev_score,
                             INT32_MIN, NULL);
}

int64_t
tmap_fsw_fitting_core(uint8_t *seq, int32_t len, 
                      tmap_fsw_flowseq_t *flowseq,
                      const tmap_fsw_param_t *ap,
                      tmap_fsw_path_t *path, int32_t *path_len)
{
  return tmap_fsw_stdaln_aux(seq, len, flowseq,
                             ap, path, path_len, TMAP_SW_TYPE_FITTING, 0,
                             INT32_MIN, NULL);
}

static inline
uint32_t tmap_fsw_path2cigar_get_type(uint32_t ctype)
{
  switch(ctype) {
    case TMAP_FSW_FROM_HP_PLUS: // deletion
      return TMAP_FSW_FROM_D;
      break;
    case TMAP_FSW_FROM_HP_MINUS: // insertion
      return TMAP_FSW_FROM_I;
      break;
    default:
      break;
  }
  return ctype;
}

uint32_t *
tmap_fsw_path2cigar(const tmap_fsw_path_t *path, int32_t path_len, int32_t *n_cigar, int32_t rm_hp)
{
  int32_t i, n;
  uint32_t *cigar;
  uint8_t dpscore_last_type, dpscore_cur_type;

  // Note: we could just use the function 'tmap_sw_path2cigar'

  if (path_len == 0 || path == 0) {
      *n_cigar = 0;
      return 0;
  }

  if(0 == rm_hp) {
      dpscore_last_type = path->ctype;
      for (i = n = 1; i < path_len; ++i) {
          if (dpscore_last_type != path[i].ctype) ++n;
          dpscore_last_type = path[i].ctype;
      }
      *n_cigar = n;
      cigar = tmap_malloc(*n_cigar * 4, "cigar");

      TMAP_SW_CIGAR_STORE(cigar[0], path[path_len-1].ctype, 1u);
      dpscore_last_type = path[path_len-1].ctype;
      for (i = path_len - 2, n = 0; i >= 0; --i) {
          if (path[i].ctype == dpscore_last_type) TMAP_SW_CIGAR_ADD_LENGTH(cigar[n], 1u);
          else {
              TMAP_SW_CIGAR_STORE(cigar[++n], path[i].ctype, 1u);
              dpscore_last_type = path[i].ctype;
          }
      }
  }
  else {
      // get the # of cigar operators
      dpscore_last_type = path->ctype;
      for (i = n = 1; i < path_len; ++i) {
          // get the current type
          dpscore_cur_type = tmap_fsw_path2cigar_get_type(path[i].ctype);
          if (dpscore_last_type != dpscore_cur_type) ++n;
          dpscore_last_type = dpscore_cur_type;
      }
      *n_cigar = n;
      cigar = tmap_malloc(*n_cigar * 4, "cigar");
          
      // get the last type
      dpscore_last_type = tmap_fsw_path2cigar_get_type(path[path_len-1].ctype);
      cigar[0] = 1u << 4 | dpscore_last_type;
      TMAP_SW_CIGAR_STORE(cigar[0], dpscore_last_type, 1u);
      for (i = path_len - 2, n = 0; i >= 0; --i) {
          // get the current type
          dpscore_cur_type = tmap_fsw_path2cigar_get_type(path[i].ctype);
          if (dpscore_cur_type == dpscore_last_type) TMAP_SW_CIGAR_ADD_LENGTH(cigar[n], 1u);
          else {
              TMAP_SW_CIGAR_STORE(cigar[++n], dpscore_cur_type, 1u);
              dpscore_last_type = dpscore_cur_type;
          }
      }
  }

  return cigar;
}

static int 
tmap_fsw_left_justify(char *ref, char *read, char *aln, int32_t len)
{
  char c;
  int32_t i, prev_del, prev_ins, start_ins, start_del, end_ins, end_del;
  int32_t justified = 0;

  prev_del = prev_ins = 0;
  start_del = start_ins = end_del = end_ins = -1;

  // reverse
  for(i=0;i<(len>>1);i++) {
      c = ref[i]; ref[i] = ref[len-i-1]; ref[len-i-1] = c;
      c = read[i]; read[i] = read[len-i-1]; read[len-i-1] = c;
      c = aln[i]; aln[i] = aln[len-i-1]; aln[len-i-1] = c;
  }

  for(i=0;i<len;) {
      if('-' == read[i]) { // deletion
          if(0 == prev_del) {
              start_del = i;
          }
          prev_del = 1;
          end_del = i;
          prev_ins = 0;
          start_ins = end_ins = -1;
          i++;
      }
      else if('-' == ref[i]) { // insert
          if(0 == prev_ins) {
              start_ins = i;
          }
          prev_ins = 1;
          end_ins = i;
          prev_del = 0;
          start_del = -1;
          end_del = -1;
          i++;
      }
      else {
          if(1 == prev_del) { // previous was an deletion
              start_del--;
              while(0 <= start_del && // bases remaining to examine 
                    read[start_del] != '-' && // hit another deletion 
                    ref[start_del] != '-' && // hit an insertion 
                    ref[start_del] == ref[end_del]) { // src ref base matches dest ref base 
                  // swap end_del and start_del for the read and aln
                  c = read[end_del]; read[end_del] = read[start_del]; read[start_del] = c;
                  c = aln[end_del]; aln[end_del] = aln[start_del]; aln[start_del] = c;
                  start_del--;
                  end_del--;
                  justified = 1;
              }
              end_del++; // we decremented when we exited the loop 
              i = end_del;
          }
          else if(1 == prev_ins) { // previous was an insertion
              start_ins--;
              while(0 <= start_ins && // bases remaining to examine
                    read[start_ins] != '-' && // hit another deletion 
                    ref[start_ins] != '-' && // hit an insertion 
                    read[start_ins] == read[end_ins]) { // src read base matches dest read base 
                  // swap end_ins and start_ins for the ref and aln
                  c = ref[end_ins]; ref[end_ins] = ref[start_ins]; ref[start_ins] = c;
                  c = aln[end_ins]; aln[end_ins] = aln[start_ins]; aln[start_ins] = c;
                  start_ins--;
                  end_ins--;
                  justified = 1;
              }
              end_ins++; // e decremented when we exited the loop 
              i = end_ins;
          }
          else {
              i++;
          }
          // reset
          prev_del = prev_ins = 0;
          start_del = start_ins = end_del = end_ins = -1;
      }
  }

  // reverse
  for(i=0;i<(len>>1);i++) {
      c = ref[i]; ref[i] = ref[len-i-1]; ref[len-i-1] = c;
      c = read[i]; read[i] = read[len-i-1]; read[len-i-1] = c;
      c = aln[i]; aln[i] = aln[len-i-1]; aln[len-i-1] = c;
  }

  return justified;
}

static int 
tmap_fsw_right_justify(char *ref, char *read, char *aln, int32_t len)
{
  int32_t i;
  int32_t justified = 0;
  // reverse
  for(i=0;i<(len>>1);i++) {
      char tmp;
      tmp = ref[i]; ref[i] = ref[len-i-1]; ref[len-i-1] = tmp;
      tmp = read[i]; read[i] = read[len-i-1]; read[len-i-1] = tmp;
      tmp = aln[i]; aln[i] = aln[len-i-1]; aln[len-i-1] = tmp;
  }
  // left-justify
  justified = tmap_fsw_left_justify(ref, read, aln, len);
  // reverse
  for(i=0;i<(len>>1);i++) {
      char tmp;
      tmp = ref[i]; ref[i] = ref[len-i-1]; ref[len-i-1] = tmp;
      tmp = read[i]; read[i] = read[len-i-1]; read[len-i-1] = tmp;
      tmp = aln[i]; aln[i] = aln[len-i-1]; aln[len-i-1] = tmp;
  }
  return justified;
}

void
tmap_fsw_get_aln(tmap_fsw_path_t *path, int32_t path_len,
                 uint8_t *flow, uint8_t *target, uint8_t strand,
                 char **ref, char **read, char **aln, int32_t j_type)
{
  int32_t i, j;

  (*ref) = tmap_malloc(sizeof(char) * (1 + path_len), "(*ref)");
  (*read) = tmap_malloc(sizeof(char) * (1 + path_len), "(*read)");
  (*aln) = tmap_malloc(sizeof(char) * (1 + path_len), "(*aln)");

  // ref
  for(i=path_len-1,j=0;0<=i;i--,j++) {
      if(TMAP_FSW_FROM_M == path[i].ctype 
         || TMAP_FSW_FROM_D == path[i].ctype
         || TMAP_FSW_FROM_HP_PLUS == path[i].ctype) {
          (*ref)[j] = "ACGTN"[target[path[i].j]];
      }
      else {
          (*ref)[j] = '-';
      }
  }
  (*ref)[path_len] = '\0';

  // alignment string
  for(i=path_len-1,j=0;0<=i;i--,j++) {
      switch(path[i].ctype) {
        case TMAP_FSW_FROM_M:
          if(flow[path[i].i & 3] == target[path[i].j]) {
              (*aln)[j] = '|';
          }
          else { 
              (*aln)[j] = ' ';
          }
          break;
        case TMAP_FSW_FROM_I:
          (*aln)[j] = '+'; break;
        case TMAP_FSW_FROM_D:
          (*aln)[j] = '-'; break;
        case TMAP_FSW_FROM_HP_PLUS: // overcall
          (*aln)[j] = 'h'; break;
        case TMAP_FSW_FROM_HP_MINUS: // undercall
          (*aln)[j] = 'H'; break;
        default:
          (*aln)[j] = ' '; break;
      }
  }
  (*aln)[path_len] = '\0';

  // read
  for(i=path_len-1,j=0;0<=i;i--,j++) {
      if(TMAP_FSW_FROM_M == path[i].ctype 
         || TMAP_FSW_FROM_I == path[i].ctype
         || TMAP_FSW_FROM_HP_MINUS == path[i].ctype) {
          (*read)[j] = "ACGTN"[flow[path[i].i & 3]];
      }
      else {
          (*read)[j] = '-';
      }
  }
  (*read)[path_len] = '\0';

  // the input is assumed to be in read-order
  switch(j_type) {
    case TMAP_FSW_NO_JUSTIFY:
      // do nothing
      break;
    case TMAP_FSW_JUSTIFY_LEFT_REF:
      if(0 == strand) { 
          tmap_fsw_left_justify((*ref), (*read), (*aln), path_len);
      }
      else {
          tmap_fsw_right_justify((*ref), (*read), (*aln), path_len);
      }
      break;
    case TMAP_FSW_JUSTIFY_LEFT_READ:
      tmap_fsw_left_justify((*ref), (*read), (*aln), path_len);
      break;
    default:
      break;
  }
}

void 
tmap_fsw_print_aln(tmap_file_t *fp, int64_t score, tmap_fsw_path_t *path, int32_t path_len,
                   uint8_t *flow, uint8_t *target, uint8_t strand, int32_t j_type)
{
  char *ref=NULL, *read=NULL, *aln=NULL;

  tmap_fsw_get_aln(path, path_len, flow, target, strand, &ref, &read, &aln, j_type);

  tmap_file_fprintf(fp, "%lld\t%s\t%s\t%s",
                    (long long int)score, read, aln, ref);

  free(ref);
  free(read);
  free(aln);
}

tmap_fsw_flowseq_t *
tmap_fsw_sff_to_flowseq(tmap_sff_t *sff)
{
  int32_t was_int;
  int32_t i, j, l;
  uint8_t *flow, *base_calls;
  uint16_t *flowgram;
  int32_t num_flows, key_index, key_bases;

  // convert bases to integers
  was_int = sff->is_int;
  if(0 == sff->is_int) {
      tmap_sff_to_int(sff);
  }

  // key_index/key_bases
  key_bases = 0;
  key_index = -1; // the last key base is not mixed with the first read base
  if(sff->gheader->key_length+1 <= sff->read->bases->l) {
      // get the number bases in the key that contribute to the first read flow
      for(i=sff->gheader->key_length-1;0<=i;i--) {
          uint8_t c = tmap_nt_char_to_int[(int)sff->gheader->key->s[i]];
          if(c == sff->read->bases->s[0]) {
              key_bases++;
          }
          else {
              break;
          }
      }
      if(0 < key_bases) {
          key_index = 0;
      }
  }

  // this does not have to be done for each read...
  for(i=l=0;i<sff->gheader->key_length;i++) { // get the flow index for the last key base
      l += sff->read->flow_index[i];
  }
  if(0 < key_bases) { // we must include the flow from the last key base(s)
      l--; // we want l to be zero-based
  }

  // get the flow length
  num_flows = sff->gheader->flow_length - l;

  // get the flowgram
  flowgram = tmap_malloc(num_flows * sizeof(uint16_t), "flowgram");
  for(i=0;i<num_flows;i++) {
      flowgram[i] = sff->read->flowgram[i+l];
  }

  // get the flow
  flow = tmap_malloc(sizeof(uint8_t) * 4, "flow");
  for(i=0;i<4;i++) {
      // offset it by the start flow
      flow[i] = tmap_nt_char_to_int[(int)sff->gheader->flow->s[(i + l) & 3]];
  }

  // get the number of bases called per flow
  base_calls = tmap_calloc(num_flows, sizeof(uint8_t), "base_calls");
  // i = 0-based index into sff->read->bases->s
  i = sff->gheader->key_length;
  // j = 0-based index into base_calls 
  j = 0;
  // go through the base calls
  while(i<sff->read->bases->l) {
      // skip to the correct flow
      while(j < num_flows 
            && flow[(j+l) & 3] != sff->read->bases->s[i]) {
          j++;
      }

      if(num_flows <= j) {
          fprintf(stderr, "num_flows=%d sff->read->bases->l=%d\n", 
                  num_flows, (int)sff->read->bases->l);
          fprintf(stderr, "i=%d j=%d\n", i, j);
          int k;
          for(k=0;k<sff->read->bases->l;k++) {
              fputc("ACGTN"[(int)sff->read->bases->s[k]], stderr);
          }
          fputc('\n', stderr);
          tmap_error("num_flows <= j", Exit, OutOfRange);
      }
      // get the number of bases called in this flow 
      while(i < sff->read->bases->l
            && flow[(j+l) & 3] == sff->read->bases->s[i]) {
          base_calls[j]++;
          i++;
      }
      if(0 == base_calls[j]) {
          tmap_error("bug encountered", Exit, OutOfRange);
      }
      j++; // next flow
  }
  while(j < num_flows) {
      base_calls[j] = 0;
      j++;
  }
  if(j != num_flows) {
      tmap_error("j != num_flows", Exit, OutOfRange);
  }

  if(0 == was_int) {
      tmap_sff_to_char(sff);
  }

  return tmap_fsw_flowseq_init(flow, base_calls, flowgram, num_flows, key_index, key_bases);
}

void
tmap_fsw_flowseq_reverse_compliment(tmap_fsw_flowseq_t *flowseq)
{
  int32_t i;

  // reverse compliment the flow order
  for(i=0;i<2;i++) {
      uint8_t c;
      c = flowseq->flow[i];
      flowseq->flow[i] = 3 - flowseq->flow[3-i];
      flowseq->flow[3-i] = 3 - c;
  }

  // reverse the base_calls and flowgram
  for(i=0;i<flowseq->num_flows>>1;i++) {
      uint8_t c;
      uint16_t f;
      // swap base_calls
      c = flowseq->base_calls[i];
      flowseq->base_calls[i] = flowseq->base_calls[flowseq->num_flows-i-1];
      flowseq->base_calls[flowseq->num_flows-i-1] = c;
      // swap flowgram
      f = flowseq->flowgram[i];
      flowseq->flowgram[i] = flowseq->flowgram[flowseq->num_flows-i-1];
      flowseq->flowgram[flowseq->num_flows-i-1] = f;
  }

  // switch where the key base flow aligns
  if(0 < flowseq->key_bases) {
      flowseq->key_index = flowseq->num_flows-1;
  }
}

typedef struct {
    char *flow;
    char *base_calls;
    char *flowgram;
    int32_t num_flows;
    char *target;
    int32_t target_length;
    tmap_fsw_param_t param;
    int32_t j_type;
} tmap_fsw_main_opt_t;

static tmap_fsw_main_opt_t *
tmap_fsw_main_opt_init()
{
  tmap_fsw_main_opt_t *opt = NULL;
  opt = tmap_calloc(1, sizeof(tmap_fsw_main_opt_t), "opt");

  opt->flow = tmap_strdup("TAGC");
  opt->base_calls = NULL;
  opt->flowgram = NULL;
  opt->target = NULL;
  opt->target_length = 0;
  opt->j_type = TMAP_FSW_NO_JUSTIFY;

  // param
  opt->param.gap_open = 13*100;
  opt->param.gap_ext = 2*100;
  opt->param.gap_end = 2*100;
  opt->param.matrix = tmap_fsw_sm_short; // 11*100 - match, -19*100 - mismatch
  opt->param.fscore = 26*100; // set this to score_match + gap_open + gap_ext
  opt->param.offset = 0;
  opt->param.row = 5;
  opt->param.band_width = 50;

  return opt;
}

static void
tmap_fsw_main_opt_destroy(tmap_fsw_main_opt_t *opt)
{
  free(opt->flow);
  free(opt->base_calls);
  free(opt->flowgram);
  free(opt->target);
  free(opt);
}

static uint16_t*
tmap_fsw_main_get_flowgram(char *flowgram, int32_t *num_flows)
{
  int32_t i, j, m;
  uint16_t *f = NULL;
  int32_t state = 0;
  int32_t v;

  // state  |  todo
  // 0      |  read in before decimal
  // 1      |  read in decimal or comma
  // 2      |  read in after decimal
  // 3      |  read in comma

  m = 4;
  f = tmap_calloc(m, sizeof(uint16_t), "f");

  i=j=0;
  while('\0' != flowgram[i]) {
      //fprintf(stderr, "state=%d flowgram[%d]=\"%s\" j=%d\n", state, i, flowgram+i, j);
      if('.' == flowgram[i]) {
          if(1 != state) {
              tmap_error("unexpected decimal point", Exit, OutOfRange);
          }
          i++; // skip over the decimal
          state = 2;
      }
      else if(',' == flowgram[i]) {
          if(1 != state && 3 != state) {
              tmap_error("unexpected comma", Exit, OutOfRange);
          }
          i++; // skip over the comma
          j++; // next flowgram
          state = 0;
      }
      else if(0 != isgraph(flowgram[i])) {
          if(0 != state && 2 != state) {
              tmap_error("unexpected digits", Exit, OutOfRange);
          }
          if(m <= j) { // more memory
              m <<= 1;
              f = tmap_realloc(f, m*sizeof(uint16_t), "f");
          }
          if(0 == state) {
              v = atoi(flowgram + i);
              f[j] = 100*v;
          }
          else { // state 2
              int32_t x = 10;
              // keep only the 2 left-most digits
              for(v=0;v<2 && '0'<=flowgram[i+v] && flowgram[i+v] <= '9';v++) {
                  f[j] += x * ((int)(flowgram[i+v]) - (int)'0');
                  x /= 10;
              }
          }
          while('\0' != flowgram[i] && '0' <= flowgram[i] && flowgram[i] <= '9') { // skip over
              i++;
          }
          state++;
      }
      else {
          tmap_error("could not parse the flowgram", Exit, OutOfRange);
      }
  }
  j++;
  if(1 != state && 3 != state) {
      tmap_error("ended with a comma or a decimal point", Exit, OutOfRange);
  } 

  (*num_flows) = j;
  f = tmap_realloc(f, (*num_flows)*sizeof(uint16_t), "f");
  return f;
}

uint8_t *
tmap_fsw_main_get_base_calls(char *optarg, int32_t num_flows, char *flow)
{
  int32_t i, j, k;
  uint8_t *base_calls = NULL;

  base_calls = tmap_calloc(num_flows, sizeof(uint8_t), "base_calls");

  for(i=j=0;i<strlen(optarg);i++) {
      k=0;
      while(toupper(flow[j & 3]) != toupper(optarg[i])) {
          j++; k++;
          if(3 < k) tmap_error("could not understand the base", Exit, OutOfRange);
      }
      base_calls[j]++;
  }
  j++;
  while(j < num_flows) {
      base_calls[j]=0;
      j++;
  }

  return base_calls;
}

static int32_t
usage(tmap_fsw_main_opt_t *opt)
{
  tmap_file_fprintf(tmap_file_stderr, "\n");
  tmap_file_fprintf(tmap_file_stderr, "Usage: %s fsw [options]", PACKAGE);
  tmap_file_fprintf(tmap_file_stderr, "\n");
  tmap_file_fprintf(tmap_file_stderr, "Options (required):\n");
  tmap_file_fprintf(tmap_file_stderr, "         -b STRING   base calls ([ACGT]+) [%s]\n",
                    opt->base_calls);
  tmap_file_fprintf(tmap_file_stderr, "         -f STRING   flowgram float values (comma separated) [%s]\n",
                    opt->flowgram);
  tmap_file_fprintf(tmap_file_stderr, "         -t STRING   target sequence ([ACGT]+) [%s]\n",
                    opt->target);
  tmap_file_fprintf(tmap_file_stderr, "\n");
  tmap_file_fprintf(tmap_file_stderr, "Options (optional):\n");
  tmap_file_fprintf(tmap_file_stderr, "         -k STRING   the flow order ([ACGT]{4}) [%s]\n",
                    opt->flow);
  tmap_file_fprintf(tmap_file_stderr, "         -F INT      flow penalty [%d]\n", opt->param.fscore);
  tmap_file_fprintf(tmap_file_stderr, "         -o INT      search for homopolymer errors +- offset [%d]\n", opt->param.offset);
  tmap_file_fprintf(tmap_file_stderr, "         -l INT      indel justification type: 0 - none, 1 - 5' strand of the reference, 2 - 5' strand of the read [%d]\n", opt->j_type);
  tmap_file_fprintf(tmap_file_stderr, "         -v          print verbose progress information\n");
  tmap_file_fprintf(tmap_file_stderr, "         -h          print this message\n");
  tmap_file_fprintf(tmap_file_stderr, "\n");

  return 1;
}

int tmap_fsw_main(int argc, char *argv[])
{
  int32_t i, c;
  tmap_fsw_path_t *path=NULL;
  int32_t path_len = 0;
  tmap_fsw_main_opt_t *opt;
  int32_t num_flows = 0;
  uint16_t *flowgram = NULL;
  uint8_t *base_calls = NULL;
  uint8_t flow[5];
  tmap_fsw_flowseq_t *flowseq = NULL;

  opt = tmap_fsw_main_opt_init();

  while((c = getopt(argc, argv, "b:f:t:k:F:o:l:vh")) >= 0) {
      switch(c) {
        case 'b':
          opt->base_calls = tmap_strdup(optarg); break;
        case 'f':
          opt->flowgram = tmap_strdup(optarg); break;
        case 't':
          opt->target = tmap_strdup(optarg); opt->target_length = strlen(opt->target); break;
        case 'k':
          free(opt->flow);
          opt->flow = tmap_strdup(optarg); break;
        case 'F':
          opt->param.fscore = 100*atoi(optarg); break;
        case 'o':
          opt->param.offset = atoi(optarg); break;
        case 'l':
          opt->j_type = atoi(optarg); break;
        case 'v':
          tmap_progress_set_verbosity(1); break;
        case 'h':
        default:
          return usage(opt);
      }
  }

  if(argc != optind || 1 == argc) {
      return usage(opt);
  }
  else { // check the command line options
      if(NULL == opt->base_calls) {
          tmap_error("option -b must be specified", Exit, CommandLineArgument);
      }
      if(NULL == opt->flowgram) {
          tmap_error("option -f must be specified", Exit, CommandLineArgument);
      }
      if(NULL == opt->target) {
          tmap_error("option -t must be specified", Exit, CommandLineArgument);
      }
      if(NULL == opt->flow) {
          tmap_error("option -k must be specified", Exit, CommandLineArgument);
      }
      tmap_error_cmd_check_int(opt->param.fscore, 0, INT32_MAX, "-F");
      tmap_error_cmd_check_int(opt->param.offset, 0, INT32_MAX, "-o");
      tmap_error_cmd_check_int(opt->param.offset, TMAP_FSW_NO_JUSTIFY, TMAP_FSW_NO_JUSTIFY, "-l");
  }

  // get the flowgram
  flowgram = tmap_fsw_main_get_flowgram(opt->flowgram, &num_flows); 

  // get the flow base calls
  base_calls = tmap_fsw_main_get_base_calls(opt->base_calls, num_flows, opt->flow);

  // convert the target to 2-bit format 
  for(i=0;i<opt->target_length;i++) {
      opt->target[i] = tmap_nt_char_to_int[(int)opt->target[i]];
  }

  for(i=0;i<4;i++) {
      flow[i] = tmap_nt_char_to_int[(int)opt->flow[i]];
  }

  path = tmap_malloc(sizeof(tmap_fsw_path_t)*TMAP_FSW_MAX_PATH_LENGTH(opt->target_length, num_flows, opt->param.offset), "path");

  flowseq = tmap_fsw_flowseq_init(flow, base_calls, flowgram, num_flows, -1, -1);

  // align
  int64_t score = tmap_fsw_global_core((uint8_t*)opt->target, opt->target_length,
                                       flowseq,
                                       &opt->param,
                                       path, &path_len);

  tmap_file_stdout = tmap_file_fdopen(fileno(stdout), "wb", TMAP_FILE_NO_COMPRESSION);

  // print
  tmap_fsw_print_aln(tmap_file_stdout, score, path, path_len,
                     flow, (uint8_t*)opt->target, 0, opt->j_type);
  tmap_file_fprintf(tmap_file_stdout, "\n");

  tmap_file_fclose(tmap_file_stdout);

  // destroy memory
  tmap_fsw_main_opt_destroy(opt);
  free(flowgram);
  free(base_calls);
  free(path);
  tmap_fsw_flowseq_destroy(flowseq);

  return 0;
}
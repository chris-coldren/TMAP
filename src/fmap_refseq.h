#ifndef FMAP_REFSEQ_H_
#define FMAP_REFSEQ_H_

#include <stdint.h>
#include "fmap_io.h"

// version id of the indexes
#define FMAP_REFSEQ_VERSION_ID 0
// seed for our random number generator
#define FMAP_REFSEQ_SEED 13
// refseq file extension
#define FMAP_REFSEQ_ANNO_FILE_EXTENSION ".fmap.refseq.anno"
#define FMAP_REFSEQ_PAC_FILE_EXTENSION ".fmap.refseq.pac"
// compression for the refseq file
#define FMAP_REFSEQ_COMPRESSION FMAP_FILE_NO_COMPRESSION
// buffer size for reading in from a FASTA file
#define FMAP_REFSEQ_BUFFER_SIZE 0x10000

/*! @macro
  @param  _len  the number of bases stored 
  @return       the number of bytes allocated
  */
#define fmap_refseq_seq_memory(_len) (((_len) + 1) >> 2)

/*! @macro
  @param  _i  the 0-based base position 
  @return     the 0-based byte index
  */
#define fmap_refseq_seq_byte_i(_i) (fmap_refseq_seq_memory(_i-1))

/*! @macro
  @param  _i  the 0-based base position 
  @return     the number of bits the base is shifted (returns a multiple of two)
  @dicsussion the reference is stored in a 2-bit format
  */
#define fmap_refseq_seq_byte_shift(_i) ((3 - ((_i) & 3)) << 1)

/*! @macro
  @param  _refseq  pointer to the structure holding the reference sequence
  @param  _i       the 0-based base position to store
  @param  _c       the base's 2-bit integer representation
  */
#define fmap_refseq_seq_store_i(_refseq, _i, _c) (_refseq->seq[fmap_refseq_seq_byte_i(_i)] |= _c << fmap_refseq_seq_byte_shift(_i))

/*! @macro
  @param  _refseq  pointer to the structure holding the reference sequence
  @param  _i       the 0-based base position to retrieve
  @return          the base's 2-bit integer representation
  */
#define fmap_refseq_seq_i(_refseq, _i) ((_refseq->seq[fmap_refseq_seq_byte_i(_i)] >> fmap_refseq_seq_byte_shift(_i)) & 3)

/*! @typedef
  @abstract  
  @field  name    the name of the contig
  @field  len     the length of the current contig 
  @field  offset  the offset from the start of the reference
  */
typedef struct {
    char *name;
    uint64_t len;
    uint64_t offset;
} fmap_anno_t;

/*! @typedef
  @abstract  
  @field  version_id    the version id of this file
  @field  seed          the random base generator seed
  @field  seq           the packed nucleotide sequence, with contigs concatenated
  @field  annos         the annotations about the contigs
  @field  len           the total length of the reference sequence
  */
typedef struct {
    uint64_t version_id;
    uint32_t seed;
    uint8_t *seq;
    fmap_anno_t *annos;
    uint32_t num_annos;
    uint64_t len;
} fmap_refseq_t;

/*! @function
  @abstract
  @field  fn_fasta     the file name of the fasta file
  @field  compression  the type of compression, if any to be used
  @return             a pointer to the initialized memory
  */
fmap_refseq_t *
fmap_refseq_read_fasta(const char *fn_fasta, int32_t compression);

/*! @function
  @abstract
  @field  refseq  pointer to the structure in which to store the data 
  @field  prefix   the prefix of the file to be written, usually the fasta file name 
  */
void
fmap_refseq_write(fmap_refseq_t *refseq, const char *prefix);

/*! @function
  @abstract
  @field  prefix   the prefix of the file to be read, usually the fasta file name 
  @return        a pointer to the initialized memory
  */
fmap_refseq_t *
fmap_refseq_read(const char *prefix);

/*! @function
  @abstract
  @field  refseq  pointer to the structure in which the data is stored
  */
void
fmap_refseq_destroy(fmap_refseq_t *refseq);

/*! @function
  @abstract
  @field  refseq  pointer to the structure in which the data is stored
  @return        the number of bytes used by this data structure
  */
size_t
fmap_refseq_size(fmap_refseq_t *refseq);
#endif

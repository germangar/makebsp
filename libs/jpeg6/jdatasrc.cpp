/*
 * jdatasrc.c
 *
 * Copyright (C) 1994, Thomas G. Lane.
 * This file is part of the Independent JPEG Group's software.
 * For conditions of distribution and use, see the accompanying README file.
 *
 * This file contains decompression data source routines for the case of
 * reading JPEG data from a file (or any stdio stream).  While these routines
 * are sufficient for most applications, some will want to use a different
 * source manager.
 * IMPORTANT: we assume that fread() will correctly transcribe an array of
 * JOCTETs from 8-bit-wide elements on external storage.  If char is wider
 * than 8 bits on your machine, you may need to do some tweaking.
 */


/* this is not a core library module, so it doesn't define JPEG_INTERNALS */
#include "jinclude.h"
#include "jpeglib.h"
#include "jerror.h"


/* Expanded data source object for stdio input */

typedef struct {
  struct jpeg_source_mgr pub; /* public fields */
} my_source_mgr;

typedef my_source_mgr *my_src_ptr;

/*
 * Initialize source --- called by jpeg_read_header
 * before any data is actually read.
 */

METHODDEF void init_source(j_decompress_ptr cinfo) {}

/*
 * Fill the input buffer --- called whenever buffer is emptied.
 */

METHODDEF boolean fill_input_buffer(j_decompress_ptr cinfo) {
  /* Insert a fake EOI marker */
  static const JOCTET fake_eoi[2] = {(JOCTET)0xFF, (JOCTET)JPEG_EOI};
  cinfo->src->next_input_byte = fake_eoi;
  cinfo->src->bytes_in_buffer = 2;
  return TRUE;
}

/*
 * Skip data --- used to skip over a potentially large amount of
 * uninteresting data (such as an APPn marker).
 */

METHODDEF void skip_input_data(j_decompress_ptr cinfo, long num_bytes) {
  if (num_bytes > 0) {
    if (num_bytes > (long)cinfo->src->bytes_in_buffer) {
      num_bytes = (long)cinfo->src->bytes_in_buffer;
    }
    cinfo->src->next_input_byte += (size_t)num_bytes;
    cinfo->src->bytes_in_buffer -= (size_t)num_bytes;
  }
}

/*
 * Terminate source --- called by jpeg_finish_decompress
 * after all data has been read. Often a no-op.
 */

METHODDEF void term_source(j_decompress_ptr cinfo) {
  /* no work necessary here */
}

/*
 * Prepare for input from a memory buffer.
 * The caller is responsible for the buffer during decompression.
 */

GLOBAL void jpeg_stdio_src(j_decompress_ptr cinfo, unsigned char *infile,
                           size_t len) {
  my_src_ptr src;

  if (cinfo->src == NULL) { /* first time for this JPEG object? */
    cinfo->src = (struct jpeg_source_mgr *)(*cinfo->mem->alloc_small)(
        (j_common_ptr)cinfo, JPOOL_PERMANENT, SIZEOF(my_source_mgr));
  }

  src = (my_src_ptr)cinfo->src;
  src->pub.init_source = init_source;
  src->pub.fill_input_buffer = fill_input_buffer;
  src->pub.skip_input_data = skip_input_data;
  src->pub.resync_to_restart = jpeg_resync_to_restart; /* use default method */
  src->pub.term_source = term_source;
  src->pub.next_input_byte = infile;
  src->pub.bytes_in_buffer = len;
}

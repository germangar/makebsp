#include "jpeglib.h"
#include <memory.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Custom error handler to avoid silent exits
 */
static void my_error_exit(j_common_ptr cinfo) {
  char buffer[JMSG_LENGTH_MAX];
  (*cinfo->err->format_message)(cinfo, buffer);
  printf("JPEG FATAL ERROR: %s\n", buffer);
  fflush(stdout);
  // We still have to exit unless we use setjmp/longjmp, 
  // but at least we see the message now.
  exit(1); 
}

static void my_output_message(j_common_ptr cinfo) {
  char buffer[JMSG_LENGTH_MAX];
  (*cinfo->err->format_message)(cinfo, buffer);
  printf("JPEG WARNING: %s\n", buffer);
  fflush(stdout);
}

GLOBAL void LoadJPGBuff(unsigned char *fbuffer, size_t len, unsigned char **pic,
                        int *width, int *height) {

  struct jpeg_decompress_struct cinfo;
  struct jpeg_error_mgr jerr;
  int row_stride;
  unsigned char *out;
  int nSize;

  cinfo.err = jpeg_std_error(&jerr);
  jerr.error_exit = my_error_exit;
  jerr.output_message = my_output_message;

  jpeg_create_decompress(&cinfo);


  jpeg_stdio_src(&cinfo, fbuffer, len);


  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {

    return;
  }


  (void)jpeg_start_decompress(&cinfo);


  row_stride = cinfo.output_width * cinfo.output_components;

  nSize = cinfo.output_width * cinfo.output_height * 4;
  out = reinterpret_cast<unsigned char *>(malloc(nSize + 1));
  if (!out) {

    return;
  }
  memset(out, 0, nSize + 1);


  *pic = out;
  *width = cinfo.output_width;
  *height = cinfo.output_height;

  /* Allocate a small row buffer for the decompressor */
  JSAMPARRAY row_buffer = (*cinfo.mem->alloc_sarray)(
      (j_common_ptr)&cinfo, JPOOL_IMAGE, row_stride, 1);


  while (cinfo.output_scanline < cinfo.output_height) {
    int current_scanline = cinfo.output_scanline;
    (void)jpeg_read_scanlines(&cinfo, row_buffer, 1);

    unsigned char *out_row = out + (current_scanline * cinfo.output_width * 4);
    unsigned char *in_row = row_buffer[0];

    if (cinfo.output_components == 3) {
      for (int i = 0; i < (int)cinfo.output_width; i++) {
        out_row[i * 4 + 0] = in_row[i * 3 + 0];
        out_row[i * 4 + 1] = in_row[i * 3 + 1];
        out_row[i * 4 + 2] = in_row[i * 3 + 2];
        out_row[i * 4 + 3] = 255;
      }
    } else if (cinfo.output_components == 1) {
      for (int i = 0; i < (int)cinfo.output_width; i++) {
        out_row[i * 4 + 0] = in_row[i];
        out_row[i * 4 + 1] = in_row[i];
        out_row[i * 4 + 2] = in_row[i];
        out_row[i * 4 + 3] = 255;
      }
    }
  }


  (void)jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);

}

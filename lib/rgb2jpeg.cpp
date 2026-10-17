#include "rgb2jpeg.h"

#include <jpeglib.h>
#include <stdexcept>
#include "madness.h"

namespace {

// libjpeg writes in blocks into a buffer the caller owns; this one hands the
// blocks straight to a Bytestream, the same way baselinify does.
struct bts_destination_mgr : jpeg_destination_mgr
{
  static const size_t BLOCK = 16*1024;

  bts_destination_mgr(Bytestream& out) : bts(out)
  {
    jpeg_destination_mgr::init_destination = init_destination;
    jpeg_destination_mgr::empty_output_buffer = empty_output_buffer;
    jpeg_destination_mgr::term_destination = term_destination;
    buffer = Array<uint8_t>(BLOCK);
    jpeg_destination_mgr::next_output_byte = (JOCTET*)buffer;
    jpeg_destination_mgr::free_in_buffer = BLOCK;
  }

  static void init_destination(j_compress_ptr) {}

  static boolean empty_output_buffer(j_compress_ptr cinfo)
  {
    bts_destination_mgr* self = (bts_destination_mgr*)cinfo->dest;
    self->bts.putBytes(self->buffer, BLOCK);
    self->next_output_byte = (JOCTET*)self->buffer;
    self->free_in_buffer = BLOCK;
    return TRUE;
  }

  static void term_destination(j_compress_ptr cinfo)
  {
    bts_destination_mgr* self = (bts_destination_mgr*)cinfo->dest;
    self->bts.putBytes(self->buffer, BLOCK - self->free_in_buffer);
  }

  Bytestream& bts;
  Array<uint8_t> buffer;
};

}

void rgb_to_jpeg(const uint8_t* samples, size_t width, size_t height,
                 size_t components, int quality, Bytestream& out)
{
  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr err;

  #if MADNESS
  #include "libfuncs_jpeg_encode"
  #endif

  cinfo.err = jpeg_std_error(&err);
  jpeg_create_compress(&cinfo);

  bts_destination_mgr dest(out);
  cinfo.dest = &dest;

  cinfo.image_width = width;
  cinfo.image_height = height;
  cinfo.input_components = components;
  cinfo.in_color_space = components == 1 ? JCS_GRAYSCALE : JCS_RGB;

  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);
  // Baseline, no progressive scan: a printer reads these strips one after the
  // other as they arrive and cannot wait for a second pass.
  cinfo.optimize_coding = FALSE;

  jpeg_start_compress(&cinfo, TRUE);

  const size_t stride = width*components;
  while(cinfo.next_scanline < cinfo.image_height)
  {
    JSAMPROW row = (JSAMPROW)(samples + cinfo.next_scanline*stride);
    jpeg_write_scanlines(&cinfo, &row, 1);
  }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
}

#include <poppler.h>
#include <poppler-document.h>

#include <cairo.h>
#include <cairo-ps.h>
#include <cairo-pdf.h>

#include <iostream>
#include <fstream>
#include <filesystem>
#include <math.h>
#include <unistd.h>
#include "madness.h"

#include <bytestream.h>
#include <array.h>
#include <pointer.h>
#include "ppm2pwg.h"
#include "pdf2printable.h"

#include <cstdio>
#include <jpeglib.h>
#include <vector>

#define R_RELATIVE_LUMINOSITY 0.299
#define G_RELATIVE_LUMINOSITY 0.587
#define B_RELATIVE_LUMINOSITY 0.114
#define RGB32_R(RGB) ((RGB>>16)&0xff)
#define RGB32_G(RGB) ((RGB>>8)&0xff)
#define RGB32_B(RGB) (RGB&0xff)
#define RGB32_GRAY(RGB) (((RGB32_R(RGB)*R_RELATIVE_LUMINOSITY) \
                        + (RGB32_G(RGB)*G_RELATIVE_LUMINOSITY) \
                        + (RGB32_B(RGB)*B_RELATIVE_LUMINOSITY)))

#define MAX3(A,B,C) std::max(std::max(A, B), C)
#define SIXTEENTHS(parts, value) (parts*(value/16))

#ifndef PDF_CREATOR
#define PDF_CREATOR "pdf2printable"
#endif

#define CHECK(call) if(!(call)) {return 1;}

void copy_raster_buffer(Bytestream& bmpBts, uint32_t* data, const PrintParameters& params);

void fixup_scale(double& xScale, double& yScale, double& xOffset, double& yOffset,
                 bool& rotate, double& wIn, double& hIn, const PrintParameters& params);

inline cairo_status_t bytestream_writer(void* bts, const unsigned char* data, unsigned int length)
{
  ((Bytestream*)bts)->putBytes(data, length);
  return CAIRO_STATUS_SUCCESS;
}

inline double round2(double d)
{
  return round(d*100)/100;
}

// Raw samples to a baseline JPEG, which is what the strips of a PCLm document
// are made of. The other direction, re-encoding an existing JPEG, is
// baselinify; this one is small enough to keep next to its only user.
namespace {

struct jpeg_bytestream_dest : jpeg_destination_mgr
{
  static const size_t BLOCK = 16*1024;

  jpeg_bytestream_dest(Bytestream& out) : bts(out), buffer(BLOCK)
  {
    init_destination = do_init;
    empty_output_buffer = do_empty;
    term_destination = do_term;
    next_output_byte = (JOCTET*)buffer;
    free_in_buffer = BLOCK;
  }

  static void do_init(j_compress_ptr) {}

  static boolean do_empty(j_compress_ptr cinfo)
  {
    jpeg_bytestream_dest* self = (jpeg_bytestream_dest*)cinfo->dest;
    self->bts.putBytes(self->buffer, BLOCK);
    self->next_output_byte = (JOCTET*)self->buffer;
    self->free_in_buffer = BLOCK;
    return TRUE;
  }

  static void do_term(j_compress_ptr cinfo)
  {
    jpeg_bytestream_dest* self = (jpeg_bytestream_dest*)cinfo->dest;
    self->bts.putBytes(self->buffer, BLOCK - self->free_in_buffer);
  }

  Bytestream& bts;
  Array<uint8_t> buffer;
};

void samples_to_jpeg(const uint8_t* samples, size_t width, size_t height,
                     size_t components, int quality, Bytestream& out)
{
  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr err;

  #if MADNESS
  LIB(jpeg, "libjpeg.so.62");
  FUNC(jpeg, struct jpeg_error_mgr*, jpeg_std_error, struct jpeg_error_mgr*);
  FUNC(jpeg, void, jpeg_CreateCompress, j_compress_ptr, int, size_t);
  FUNC(jpeg, void, jpeg_set_defaults, j_compress_ptr);
  FUNC(jpeg, void, jpeg_set_quality, j_compress_ptr, int, boolean);
  FUNC(jpeg, void, jpeg_start_compress, j_compress_ptr, boolean);
  FUNC(jpeg, JDIMENSION, jpeg_write_scanlines, j_compress_ptr, JSAMPARRAY, JDIMENSION);
  FUNC(jpeg, void, jpeg_finish_compress, j_compress_ptr);
  FUNC(jpeg, void, jpeg_destroy_compress, j_compress_ptr);
  #endif

  cinfo.err = jpeg_std_error(&err);
  jpeg_create_compress(&cinfo);

  jpeg_bytestream_dest dest(out);
  cinfo.dest = &dest;

  cinfo.image_width = width;
  cinfo.image_height = height;
  cinfo.input_components = components;
  cinfo.in_color_space = components == 1 ? JCS_GRAYSCALE : JCS_RGB;

  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);
  // Baseline: a printer reads these strips one after the other as they arrive
  // and cannot wait for a second pass.
  cinfo.optimize_coding = FALSE;

  jpeg_start_compress(&cinfo, TRUE);
  while(cinfo.next_scanline < cinfo.image_height)
  {
    JSAMPROW row = (JSAMPROW)(samples + cinfo.next_scanline*width*components);
    jpeg_write_scanlines(&cinfo, &row, 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
}

}

// A page of PCLm is a row of image strips, and a printer takes them one at a
// time: it never holds a whole page, which is exactly why an HP LaserJet with
// 384 MB runs out of memory on a magazine page as PDF and prints the same page
// as PCLm without complaint (measured; see the SeaPrint port's notes).
//
// The same trick works on this side of the wire. The page is rendered in
// bands of a few hundred rows instead of all at once -- an A4 page at 600 dpi
// is 4961x6614 pixels, 104 MB in colour, where a band is some 8 MB -- and each
// band is cut into strips, compressed, and handed to writeFun as it is made.
// Nothing but the band and one strip's JPEG is ever in memory, so a phone with
// 70 MB free can print what it could not even render before.
//
// The document is written front to back in one pass: every page names its
// strips before they arrive, the strips follow, and the cross-reference table
// comes last. That is what lets a printer start on page one while the rest is
// still being made.
static int pdf_to_pclm(std::string inFile, WriteFun writeFun, const PrintParameters& params,
                       ProgressFun progressFun, bool verbose)
{
  // How much memory one band may take. Smaller is slower (poppler draws the
  // page once per band), larger needs more room: 8 MB is about 400 rows of an
  // A4 page at 600 dpi, and 17 bands for the whole page.
  const size_t BAND_BYTES = 8*1024*1024;
  const int JPEG_QUALITY = 90;

  #if MADNESS
#if !GLIB_CHECK_VERSION(2, 36, 0)
  g_type_init();
#endif
  // The same table of function pointers as the other writer uses, of which
  // this one needs about half -- and ppm2pwg is built with -Werror.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
  #include "libfuncs"
#pragma GCC diagnostic pop
  #endif

  Pointer<PopplerDocument> doc(nullptr, g_object_unref);
  GError* error = nullptr;

  if(inFile == "-")
  {
    doc = poppler_document_new_from_fd(STDIN_FILENO, nullptr, &error);
  }
  else
  {
    inFile = std::filesystem::absolute(inFile);
    std::string url("file://");
    url.append(inFile);
    doc = poppler_document_new_from_file(url.c_str(), nullptr, &error);
  }

  if(doc == nullptr)
  {
    std::cerr << "Failed to open PDF: " << error->message << " (" << inFile << ")" << std::endl;
    g_error_free(error);
    return 1;
  }

  const size_t pages = poppler_document_get_n_pages(doc);
  const PageSequence seq = params.getPageSequence(pages);

  const size_t pageW = params.getPaperSizeWInPixels();
  const size_t pageH = params.getPaperSizeHInPixels();
  const size_t stripHeight = params.stripHeight ? params.stripHeight : 16;
  const size_t strips = (pageH+stripHeight-1)/stripHeight;
  const bool gray = params.getNumberOfColors() == 1;
  const size_t components = gray ? 1 : 3;

  // Bands are whole strips, and at least one.
  size_t bandRows = (BAND_BYTES/(pageW*4))/stripHeight*stripHeight;
  bandRows = std::max(stripHeight, std::min(bandRows, pageH));

  const double wPt = round2(pageW*72.0/params.hwResW);
  const double hPt = round2(pageH*72.0/params.hwResH);

  if(verbose)
  {
    std::cerr << "PCLm: " << pageW << "x" << pageH << " px, " << strips << " strips of "
              << stripHeight << " rows, bands of " << bandRows << " rows" << std::endl;
  }

  // Object numbers: 1 is the catalogue, 2 the page tree, then page by page.
  // Offsets are collected for the cross-reference table at the end.
  std::vector<size_t> offsets(3, 0);
  size_t written = 0;
  size_t nextObj = 3;

  auto put = [&](const std::string& text) -> bool
  {
    if(!writeFun((const unsigned char*)text.data(), text.size()))
    {
      return false;
    }
    written += text.size();
    return true;
  };
  auto putBytes = [&](const uint8_t* data, size_t length) -> bool
  {
    if(!writeFun(data, length))
    {
      return false;
    }
    written += length;
    return true;
  };
  auto startObj = [&](size_t obj) -> bool
  {
    if(offsets.size() <= obj)
    {
      offsets.resize(obj+1, 0);
    }
    offsets[obj] = written;
    return put(std::to_string(obj)+" 0 obj\n");
  };

  CHECK(put("%PDF-1.7\n%PCLm-1.0\n"));

  Pointer<cairo_surface_t> surface(cairo_image_surface_create(CAIRO_FORMAT_RGB24, pageW, bandRows),
                                   cairo_surface_destroy);
  Array<uint8_t> stripSamples(pageW*stripHeight*components);
  Bytestream jpegBts;
  std::vector<size_t> pageObjs;
  size_t outPageNo = 0;

  for(size_t pageNo : seq)
  {
    outPageNo++;
    const size_t pageObj = nextObj++;
    const size_t contentsObj = nextObj++;
    const size_t firstStripObj = nextObj;
    nextObj += strips;
    pageObjs.push_back(pageObj);

    // The page lists its strips up front: a printer reads forwards and wants
    // the names before the images turn up.
    std::string xobjects;
    for(size_t i = 0; i < strips; i++)
    {
      xobjects += "/Im"+std::to_string(i)+" "+std::to_string(firstStripObj+i)+" 0 R\n";
    }
    CHECK(startObj(pageObj));
    CHECK(put("<<\n/Type /Page\n/Parent 2 0 R\n/MediaBox [0 0 "+std::to_string(wPt)+" "
              +std::to_string(hPt)+"]\n/Resources <<\n/XObject <<\n"+xobjects+">>\n>>\n/Contents "
              +std::to_string(contentsObj)+" 0 R\n>>\nendobj\n"));

    // Where each strip goes. PDF counts from the bottom left, the strips come
    // from the top, so the first strip sits highest.
    std::string content;
    for(size_t i = 0; i < strips; i++)
    {
      const size_t rows = std::min(stripHeight, pageH-i*stripHeight);
      const double stripPt = round2(rows*72.0/params.hwResH);
      const double yPt = round2(hPt - (i*stripHeight)*72.0/params.hwResH - stripPt);
      content += "q "+std::to_string(wPt)+" 0 0 "+std::to_string(stripPt)+" 0 "
               + std::to_string(yPt)+" cm /Im"+std::to_string(i)+" Do Q\n";
    }
    CHECK(startObj(contentsObj));
    CHECK(put("<<\n/Length "+std::to_string(content.size())+"\n>>\nstream\n"));
    CHECK(put(content));
    CHECK(put("endstream\nendobj\n"));

    size_t stripNo = 0;
    for(size_t bandTop = 0; bandTop < pageH; bandTop += bandRows)
    {
      const size_t bandHeight = std::min(bandRows, pageH-bandTop);

      {
        Pointer<cairo_t> cairo(cairo_create(surface), cairo_destroy);

        if(!params.antiAlias)
        {
          cairo_set_antialias(cairo, CAIRO_ANTIALIAS_NONE);
          Pointer<cairo_font_options_t> fontOptions(cairo_font_options_create(),
                                                    cairo_font_options_destroy);
          cairo_get_font_options(cairo, fontOptions);
          cairo_font_options_set_antialias(fontOptions, CAIRO_ANTIALIAS_NONE);
          cairo_set_font_options(cairo, fontOptions);
        }

        cairo_save(cairo);
        cairo_set_source_rgb(cairo, 1, 1, 1);
        cairo_paint(cairo);
        cairo_restore(cairo);

        if(pageNo != INVALID_PAGE)
        {
          Pointer<PopplerPage> page(poppler_document_get_page(doc, pageNo-1), g_object_unref);
          double pageWidth, pageHeight;
          double xScale, yScale, xOffset, yOffset;
          bool rotate = false;

          poppler_page_get_size(page, &pageWidth, &pageHeight);
          fixup_scale(xScale, yScale, xOffset, yOffset, rotate, pageWidth, pageHeight, params);

          // The band is a window on the page: move the page up by the rows
          // that are already done, then place it as usual.
          cairo_translate(cairo, 0, -(double)bandTop);
          cairo_translate(cairo, xOffset, yOffset);
          cairo_scale(cairo, xScale, yScale);

          if(rotate)
          { // Rotate to portrait
            cairo_matrix_t matrix;
            cairo_matrix_init(&matrix, 0, -1, 1, 0, 0, pageHeight);
            cairo_transform(cairo, &matrix);
          }

          poppler_page_render_for_printing(page, cairo);
        }

        const cairo_status_t status = cairo_status(cairo);
        if(status)
        {
          std::cerr << "cairo error: " << cairo_status_to_string(status) << std::endl;
          return 1;
        }
      }

      cairo_surface_flush(surface);
      const uint32_t* band = (const uint32_t*)cairo_image_surface_get_data(surface);

      for(size_t top = 0; top < bandHeight; top += stripHeight, stripNo++)
      {
        const size_t rows = std::min(stripHeight, bandHeight-top);
        // Cairo keeps 32 bits per pixel; a JPEG wants them packed, and in
        // gray it wants one value instead of three.
        uint8_t* out = stripSamples;
        for(size_t row = 0; row < rows; row++)
        {
          const uint32_t* in = band + (top+row)*pageW;
          for(size_t col = 0; col < pageW; col++)
          {
            const uint32_t pixel = in[col];
            if(gray)
            {
              *out++ = (uint8_t)RGB32_GRAY(pixel);
            }
            else
            {
              *out++ = RGB32_R(pixel);
              *out++ = RGB32_G(pixel);
              *out++ = RGB32_B(pixel);
            }
          }
        }

        jpegBts.reset();
        samples_to_jpeg(stripSamples, pageW, rows, components, JPEG_QUALITY, jpegBts);

        CHECK(startObj(firstStripObj+stripNo));
        CHECK(put("<<\n/Type /XObject\n/Subtype /Image\n/Width "+std::to_string(pageW)
                  +"\n/Height "+std::to_string(rows)+"\n/ColorSpace "
                  +(gray ? "/DeviceGray" : "/DeviceRGB")
                  +"\n/BitsPerComponent 8\n/Filter /DCTDecode\n/Length "
                  +std::to_string(jpegBts.size())+"\n>>\nstream\n"));
        CHECK(putBytes(jpegBts.raw(), jpegBts.size()));
        CHECK(put("\nendstream\nendobj\n"));
      }
    }

    if(progressFun != nullptr)
    {
      progressFun(outPageNo, seq.size());
    }
  }

  std::string kids;
  for(size_t obj : pageObjs)
  {
    kids += std::to_string(obj)+" 0 R ";
  }
  CHECK(startObj(2));
  CHECK(put("<<\n/Type /Pages\n/Count "+std::to_string(pageObjs.size())+"\n/Kids ["+kids+"]\n>>\nendobj\n"));
  CHECK(startObj(1));
  CHECK(put("<<\n/Type /Catalog\n/Pages 2 0 R\n>>\nendobj\n"));

  const size_t xref = written;
  std::string table = "xref\n0 "+std::to_string(nextObj)+"\n0000000000 65535 f \n";
  for(size_t obj = 1; obj < nextObj; obj++)
  {
    char entry[21];
    snprintf(entry, sizeof(entry), "%010zu 00000 n \n", obj < offsets.size() ? offsets[obj] : 0);
    table += entry;
  }
  table += "trailer\n<<\n/Size "+std::to_string(nextObj)+"\n/Root 1 0 R\n>>\nstartxref\n"
         + std::to_string(xref)+"\n%%EOF\n";
  CHECK(put(table));

  return 0;
}

int pdf_to_printable(std::string inFile, WriteFun writeFun, const PrintParameters& params,
                     ProgressFun progressFun, bool verbose)
{
  if(params.format == PrintParameters::PCLm)
  { // A different shape of document altogether, written strip by strip.
    return pdf_to_pclm(inFile, writeFun, params, progressFun, verbose);
  }

  if(params.format == PrintParameters::URF && (params.hwResW != params.hwResH))
  { // URF must have a symmetric resolution
    std::cerr << "URF must have a symmetric resolution." << std::endl;
    return 1;
  }

  #if MADNESS
  #include "libfuncs"
  #endif

  bool raster = params.format == PrintParameters::PWG ||
                params.format == PrintParameters::URF;

  Pointer<cairo_surface_t> surface(nullptr, cairo_surface_destroy);
  Pointer<cairo_t> cairo(nullptr, cairo_destroy);
  cairo_status_t status;
  Bytestream bmpBts;
  Bytestream outBts;

  Pointer<PopplerDocument> doc(nullptr, g_object_unref);
  GError* error = nullptr;

  if(inFile == "-")
  {
    doc = poppler_document_new_from_fd(STDIN_FILENO, nullptr, &error);
  }
  else
  {
    inFile = std::filesystem::absolute(inFile);
    std::string url("file://");
    url.append(inFile);
    doc = poppler_document_new_from_file(url.c_str(), nullptr, &error);
  }

  if(doc == nullptr)
  {
    std::cerr << "Failed to open PDF: " << error->message << " (" << inFile << ")" << std::endl;
    g_error_free(error);
    return 1;
  }

  size_t pages = poppler_document_get_n_pages(doc);
  PageSequence seq = params.getPageSequence(pages);

  size_t outPageNo = 0;

  if(raster)
  {
    surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24,
                                         params.getPaperSizeWInPixels(),
                                         params.getPaperSizeHInPixels());
    Bytestream fileHdr;
    if(params.format == PrintParameters::URF)
    {
      fileHdr = make_urf_file_hdr(seq.size());
    }
    else
    {
      fileHdr = make_pwg_file_hdr();
    }
    CHECK(writeFun(fileHdr.raw(), fileHdr.size()));
  }
  else if(params.format == PrintParameters::PDF)
  {
    surface = cairo_pdf_surface_create_for_stream(bytestream_writer, &outBts,
                                                  params.getPaperSizeWInPoints(),
                                                  params.getPaperSizeHInPoints());
    cairo_pdf_surface_set_metadata(surface, CAIRO_PDF_METADATA_CREATOR, PDF_CREATOR);
  }
  else if(params.format == PrintParameters::Postscript)
  {
    surface = cairo_ps_surface_create_for_stream(bytestream_writer, &outBts,
                                                 params.getPaperSizeWInPoints(),
                                                 params.getPaperSizeHInPoints());
    cairo_ps_surface_restrict_to_level(surface, CAIRO_PS_LEVEL_2);
  }
  else
  {
    return 1;
  }

  for(size_t pageNo : seq)
  {
    outPageNo++;

    cairo = cairo_create(surface);

    if(raster)
    {
      if(!params.antiAlias)
      {
        cairo_set_antialias(cairo, CAIRO_ANTIALIAS_NONE);
        Pointer<cairo_font_options_t> fontOptions(cairo_font_options_create(),
                                                  cairo_font_options_destroy);
        cairo_get_font_options(cairo, fontOptions);
        cairo_font_options_set_antialias(fontOptions, CAIRO_ANTIALIAS_NONE);
        cairo_set_font_options(cairo, fontOptions);
      }
      cairo_save(cairo);
      cairo_set_source_rgb(cairo, 1, 1, 1);
      cairo_paint(cairo);
      cairo_restore(cairo);
    }

    if(pageNo != INVALID_PAGE)
    { // We are actually rendering a page and not just a blank...
      Pointer<PopplerPage> page(poppler_document_get_page(doc, pageNo-1), g_object_unref);
      double pageWidth, pageHeight;
      double xScale, yScale, xOffset, yOffset;
      bool rotate = false;

      poppler_page_get_size(page, &pageWidth, &pageHeight);
      fixup_scale(xScale, yScale, xOffset, yOffset, rotate, pageWidth, pageHeight, params);

      cairo_translate(cairo, xOffset, yOffset);
      cairo_scale(cairo, xScale, yScale);

      if(rotate)
      { // Rotate to portrait
        cairo_matrix_t matrix;
        cairo_matrix_init(&matrix, 0, -1, 1, 0, 0, pageHeight);
        cairo_transform(cairo, &matrix);
      }

      poppler_page_render_for_printing(page, cairo);
    }

    status = cairo_status(cairo);
    if(status)
    {
      std::cerr << "cairo error: " << cairo_status_to_string(status) << std::endl;
      return 1;
    }
    cairo_surface_show_page(surface);

    if(raster)
    {
      cairo_surface_flush(surface);
      uint32_t* data = (uint32_t*)cairo_image_surface_get_data(surface);
      copy_raster_buffer(bmpBts, data, params);
      bmp_to_pwg(bmpBts, outBts, outPageNo, params, verbose);
    }

    CHECK(writeFun(outBts.raw(), outBts.size()));
    outBts.reset();

    if(progressFun != nullptr)
    {
      progressFun(outPageNo, seq.size());
    }

  }

  cairo_surface_finish(surface);
  // PDF and PS will have written something now, write it out
  if(outBts.size() != 0)
  {
    CHECK(writeFun(outBts.raw(), outBts.size()));
  }

  return 0;
}

void copy_raster_buffer(Bytestream& bmpBts, uint32_t* data, const PrintParameters& params)
{
  size_t size = params.getPaperSizeInPixels();
  bool black = params.isBlack();

  if(bmpBts.size() != params.getPaperSizeInBytes())
  {
    bmpBts = Bytestream(params.getPaperSizeInBytes());
  }

  uint8_t* tmp = bmpBts.raw();

  if(params.colorMode == PrintParameters::Gray1 || params.colorMode == PrintParameters::Black1)
  {
    size_t paperSizeWInPixels = params.getPaperSizeWInPixels();
    size_t paperSizeWInBytes = params.getPaperSizeWInBytes();
    size_t paperSizeHInPixels = params.getPaperSizeHInPixels();
    int nextDebt, pixel, newpixel, debt;
    Array<int> debtArray(paperSizeWInPixels+2);
    memset(debtArray, 0, (paperSizeWInPixels+2)*sizeof(int));
    memset(tmp, black ? 0 : 0xff, bmpBts.size());
    for(size_t line=0; line < paperSizeHInPixels; line++)
    {
      nextDebt = 0; // Don't carry over forward debt from previous line
      for(size_t col=0; col < paperSizeWInPixels; col++)
      { // Do Floyd-Steinberg dithering to keep grayscales readable in 1-bit
        pixel = RGB32_GRAY(data[line*paperSizeWInPixels+col]) + nextDebt;
        newpixel = pixel < 128 ? 0 : 255;
        debt = pixel - newpixel;
        nextDebt = debtArray[col+2] + SIXTEENTHS(7, debt);
        debtArray[col] += SIXTEENTHS(3, debt);
        debtArray[col+1] += SIXTEENTHS(5, debt);
        debtArray[col+2] = SIXTEENTHS(1, debt);
        if(newpixel == 0)
        {
          if(black)
          {
            tmp[line*paperSizeWInBytes+col/8] |= (0x80 >> (col % 8));
          }
          else
          {
            tmp[line*paperSizeWInBytes+col/8] &= ~(0x80 >> (col % 8));
          }
        }
      }
    }
  }
  else if(params.colorMode == PrintParameters::Gray8 || params.colorMode == PrintParameters::Black8)
  {
    for(size_t i=0; i < size; i++)
    {
      tmp[i] = black ? (255 - RGB32_GRAY(data[i])) : RGB32_GRAY(data[i]);
    }
  }
  else if(params.colorMode == PrintParameters::sRGB24)
  {
    for(size_t i=0; i < size; i++)
    {
      tmp[i*3] = RGB32_R(data[i]);
      tmp[i*3+1] = RGB32_G(data[i]);
      tmp[i*3+2] = RGB32_B(data[i]);
    }
  }
  else if(params.colorMode == PrintParameters::CMYK32)
  {
    for(size_t i=0; i < size; i++)
    {
      uint32_t white = MAX3(RGB32_R(data[i]), RGB32_G(data[i]), RGB32_B(data[i]));
      tmp[i*4] = (white - RGB32_R(data[i]));
      tmp[i*4+1] = (white - RGB32_G(data[i]));
      tmp[i*4+2] = (white - RGB32_B(data[i]));
      tmp[i*4+3] = 255-white;
    }
  }
}


void fixup_scale(double& xScale, double& yScale, double& xOffset, double& yOffset,
                 bool& rotate, double& wIn, double& hIn, const PrintParameters& params)
{
  // If the page is landscape, contunue as if it is not, but remember to rotate
  if(wIn > hIn)
  {
    std::swap(wIn, hIn);
    rotate = true;
  }

  // Scale to fit as if we had a symmetric resolution
  // ...this makes determining fitment easier
  PrintParameters tmp = params;
  tmp.hwResW = std::min(params.hwResW, params.hwResH);
  tmp.hwResH = std::min(params.hwResW, params.hwResH);

  // PCLm is pixels as well, it just carries them as strips.
  bool raster = params.format == PrintParameters::PWG ||
                params.format == PrintParameters::URF ||
                params.format == PrintParameters::PCLm;

  size_t hOut = raster ? tmp.getPaperSizeHInPixels() : tmp.getPaperSizeHInPoints();
  size_t wOut = raster ? tmp.getPaperSizeWInPixels() : tmp.getPaperSizeWInPoints();
  double scale = round2(std::min(wOut/wIn, hOut/hIn));
  xOffset = roundf((wOut-(wIn*scale))/2);
  yOffset = roundf((hOut-(hIn*scale))/2);

  xScale = scale;
  yScale = scale;

  // Finally, if we have an asymmetric resolution
  // and are not working in absolute dimensions (points), compensate for it.
  if(raster)
  { // URF will/should not end up here, but still...
    if(params.hwResW > params.hwResH)
    {
      xScale *= (params.hwResW/params.hwResH);
      xOffset *= (params.hwResW/params.hwResH);
    }
    else if(params.hwResH > params.hwResW)
    {
      yScale *= (params.hwResH/params.hwResW);
      yOffset *= (params.hwResH/params.hwResW);
    }
  }
}

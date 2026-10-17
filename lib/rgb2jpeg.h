#ifndef RGB2JPEG_H
#define RGB2JPEG_H

#include "bytestream.h"

// Packs raw 8-bit samples into a baseline JPEG.
//
// This is the other direction from baselinify, which re-encodes an existing
// JPEG: here the pixels come from a rendered page and go out as the strips of
// a PCLm document, which is what a printer that cannot hold a whole page in
// memory asks for.
//
// components is 3 for packed RGB and 1 for gray; the rows are tightly packed
// (width*components bytes each).
void rgb_to_jpeg(const uint8_t* samples, size_t width, size_t height,
                 size_t components, int quality, Bytestream& out);

#endif // RGB2JPEG_H

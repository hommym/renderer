#ifndef PNG_H
#define PNG_H

// PNG decoder, written so the mesh loaders can sample a glTF baseColorTexture
// per vertex. Decodes to straight (non-premultiplied) 8-bit RGBA regardless of
// what the file stores, so callers never branch on colour type or bit depth.
//
// Covers every non-interlaced PNG: colour types 0/2/3/4/6, bit depths 1/2/4/8/16,
// PLTE, tRNS, and IDAT split across any number of chunks. Adam7 interlacing is
// rejected rather than silently mis-decoded; no image in the models this was
// written for uses it.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct PngImage {
    uint32_t width;
    uint32_t height;
    uint8_t* rgba;      // width*height*4 bytes, R,G,B,A per pixel, row-major, top row first
} PngImage;

// Decodes data[0..len) into *out. On success out->rgba is malloc'd and owned by
// the caller. On failure returns false, leaves *out zeroed, and writes a reason
// into err when err is non-NULL.
bool png_decode(const void* data, size_t len, PngImage* out, char* err, size_t err_len);

#endif

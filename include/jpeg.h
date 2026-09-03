#ifndef JPEG_H
#define JPEG_H

// Baseline JPEG decoder, written so the mesh loaders can hand a model's diffuse
// map to the renderer as an Object texture. glTF allows exactly two image types
// (image/png and image/jpeg) and OBJ material maps are overwhelmingly one of the
// two, so PNG plus baseline JPEG covers the formats a mesh can legally carry.
//
// Handles: SOF0 and SOF1 (baseline and extended sequential, 8-bit), 1 or 3
// components, any h/v sampling factors, restart intervals, and multiple
// quantisation and Huffman tables. Progressive (SOF2), arithmetic coding,
// 12-bit samples and CMYK are rejected with a reason rather than mis-decoded.
//
// max_dim exists because these files are big: a model's diffuse map is routinely
// 8192x8192, which is 268MB as 32-bit pixels but only 96MB as the YCbCr planes
// the decoder already has to build. Reducing during the final colour-convert
// pass means the oversized buffer is never allocated at all.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct JpegImage {
    uint32_t width;      // after reduction, not the width stored in the file
    uint32_t height;
    uint8_t* rgba;       // width*height*4, R,G,B,A per pixel, row-major, top row first
} JpegImage;

// Decodes data[0..len) into *out. When max_dim is non-zero and either stored
// dimension exceeds it, the image is box-filtered down by the smallest integer
// factor that fits; pass 0 for the full stored resolution.
//
// On success out->rgba is malloc'd and owned by the caller. On failure returns
// false, leaves *out zeroed, and writes a reason into err when err is non-NULL.
bool jpeg_decode(const void* data, size_t len, uint32_t max_dim,
                 JpegImage* out, char* err, size_t err_len);

// Frees the pixel buffer and zeroes the struct. Safe on NULL and on an already
// freed or zeroed image.
void jpeg_free(JpegImage* img);

// True when data[0..len) opens with the JPEG SOI marker.
bool jpeg_is_jpeg(const void* data, size_t len);

#endif

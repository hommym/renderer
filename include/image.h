#ifndef IMAGE_H
#define IMAGE_H

// One front door for the two image formats a mesh file can legally reference,
// so the loaders never branch on PNG vs JPEG. Produces exactly what
// Object.texture is: a row-major array of 0xAARRGGBB words with the top row
// first, which is the orientation glTF TEXCOORD_0 already assumes (v runs down).
//
// max_dim caps the longest side. Model diffuse maps run to 8192x8192, which is
// 268MB of pixels for a renderer whose window is a fraction of that; the cap
// trades detail nobody can see for memory the machine actually has.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct Image {
    uint32_t* pixels;   // width*height words, 0xAARRGGBB, row-major, top row first
    size_t    width;
    size_t    height;
} Image;

// Decodes data[0..len) after sniffing its magic bytes. Pass 0 for max_dim to
// keep the stored resolution. On failure returns false, leaves *out zeroed, and
// writes a reason into err when err is non-NULL.
bool image_decode(const void* data, size_t len, uint32_t max_dim,
                  Image* out, char* err, size_t err_len);

// Same, reading the whole file first. Used for OBJ map_Kd and for glTF images
// that point at a sibling file rather than a bufferView.
bool image_decode_file(const char* path, uint32_t max_dim,
                       Image* out, char* err, size_t err_len);

// A 1x1 image of one colour. Every Object handed to the renderer needs a
// texture -- the rasterizer samples unconditionally -- so a mesh with no image
// gets one of these rather than a NULL pointer.
bool image_solid(uint32_t argb, Image* out);

void image_free(Image* img);

#endif

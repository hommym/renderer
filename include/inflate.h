#ifndef INFLATE_H
#define INFLATE_H

// DEFLATE (RFC 1951) and its zlib wrapper (RFC 1950). Written for PNG's IDAT
// stream, which is the only consumer, so there is no compression side and no
// gzip framing.
//
// The destination is a caller-supplied fixed buffer rather than a growing one:
// PNG knows its exact decompressed size from IHDR before it starts, and a fixed
// capacity turns "how much will this expand to" from a guess into a bound. A
// stream that would write past dst_cap is an error, not a reallocation, which is
// also what stops a zip-bomb IDAT.

#include <stddef.h>
#include <stdbool.h>

// Consumes a zlib stream: 2-byte header, DEFLATE data, 4-byte adler32.
// The adler32 is verified. Returns false on any malformed input, on output
// exceeding dst_cap, or on a checksum mismatch, and writes a reason into err
// when err is non-NULL.
bool inflate_zlib(const void* src, size_t src_len,
                  void* dst, size_t dst_cap, size_t* out_len,
                  char* err, size_t err_len);

// Raw DEFLATE with no zlib header or trailer, for callers that have already
// stripped the framing.
bool inflate_raw(const void* src, size_t src_len,
                 void* dst, size_t dst_cap, size_t* out_len,
                 char* err, size_t err_len);

#endif

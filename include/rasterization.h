#ifndef RASTERIZATION
#define RASTERIZATION

#include <stdint.h>
#include <stdbool.h>

typedef struct Vectex Vectex;
typedef struct PixelCord PixelCord;
typedef struct Object Object;

// The rasterizer runs in two passes over a chunk of triangles, and render()
// drives both from its frame worker pool.
//
//   A. SETUP, parallel by TRIANGLE. Each thread takes a statically assigned,
//      contiguous range of the chunk, transforms/culls/clips/projects it, and
//      writes the survivors into its own slice of a shared setup array. It then
//      records each survivor in its own bin for every row BAND it touches.
//      Nothing is shared, so nothing is locked.
//
//   B. FILL, parallel by BAND. A band is a horizontal strip of the frame
//      buffer, and each band is claimed by exactly one thread -- so every pixel
//      has exactly one writer and the frame-buffer locks disappear entirely.
//
// Splitting it this way is also what makes a frame reproducible. Thread t's
// setup slice sits entirely below thread t+1's, so walking a band's bins in
// thread order visits triangles in increasing source order. Two fragments at
// exactly the same depth therefore resolve the same way on every run, on any
// number of cores.

// Snapshot the camera for the frame. Call after view_refresh() and before any
// worker starts; the passes only read it.
void rasterization_frame_begin(void);

// Size the setup array and the per-thread bins. Cheap and idempotent when
// nothing changed; call again after a resize (the band count follows the screen
// height). Returns false only on allocation failure.
bool rasterization_pool_init(uint32_t threads);
void rasterization_pool_release(void);

// Pass A, run by every worker: thread `tid` of `threads` handles its share of
// source triangles [lo,hi) of `obj`.
void rasterization_setup_pass(const Object* obj, uint64_t lo, uint64_t hi, uint32_t tid);

// Pass B, run by every worker: claim bands until they are gone.
void rasterization_fill_pass(const Object* obj);

// Empty this thread's bins for the next chunk. Between a fill barrier and a
// setup barrier.
void rasterization_chunk_reset(uint32_t tid);

// How many source triangles a chunk holds. The setup array is sized from it, so
// it bounds memory rather than tracking the model.
uint32_t rasterization_chunk_size(void);

// The frustum test used by the point-cloud path in render().
bool is_vectex_visible(Vectex point, PixelCord* pxcord_p);

#endif

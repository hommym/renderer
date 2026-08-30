# Renderer Issues

Open problems in the renderer. Each entry: what breaks, why it breaks, fix direction. Resolved items removed; historical narrative removed.

---

## 1. The frame buffer is raced by the rasterization threads

**What breaks.** The same scene renders differently every run. Some pixels end up holding a combination of values that no triangle ever produced.

**Why.** `rasterization.c:282-285` (span fill) and `rasterization.c:294-297` (edge pixel) both do an unsynchronised read-modify-write:

```c
PixelCord existing = frame_buffer[y][x];              // 1. read
if(!(existing.in_use && existing.z < fill_pixel.z))   // 2. decide
    frame_buffer[y][x] = fill_pixel;                  // 3. write
```

Work is partitioned **by triangle**, so two threads routinely land on the same pixel. Two failures follow:

- *Lost update.* Both threads read the same `existing`, both conclude they are in front, and the second write clobbers the first. The depth test was answered correctly about a state that no longer existed, so the farther fragment can win.
- *Torn write.* `sizeof(PixelCord) == 32` — four 64-bit words, not one atomic store. Two threads writing the same pixel interleave inside the copy.

Measured on 240 overlapping triangles with 8 workers: identical input, 8 runs, 8 different frame hashes at a constant 922,437 painted pixels. Between 78 and 943 pixels per run carried one triangle's colour beside another triangle's depth — a pairing that cannot exist in the input.

The cube hides this because 12 triangles barely contend for the same pixel; a real mesh contends at every overlap and silhouette.

**Fix path.** Partition so no two threads can address the same pixel, rather than locking. Interleave by screen row (thread `i` owns rows where `row % nthreads == i`) or by tile: every thread still walks every triangle, but only writes rows it owns, so the depth test stays a plain read-modify-write on private memory. Costs redundant clip/project per thread; the fill work — the expensive part — still divides. A mutex per pixel is impractical and one global lock removes the point of threading.

---

## 2. `rasterization_worker` falls off the end without returning

**What breaks.** Undefined behaviour on every worker exit.

**Why.** `rasterization.c:309` declares `void* rasterization_worker(void* args)` and the function body ends after the `while` loop with no `return`. Reaching the closing brace of a non-`void` function and having the caller use the value is UB. `pthread_join` is passed `NULL` for the result today, so nothing reads it and it does not misbehave in practice.

**Fix path.** `return NULL;` at the end. `args` is also unused (`-Wunused-parameter`) — cast it to void or use it, since the worker index is currently passed and discarded.

---

## 3. Zero-length VLA on a single-core machine

**What breaks.** `renderer.c:129` declares `pthread_t threads[num_core]` where `num_core` can be 0.

**Why.** `render_init` computes `num_core = num_core<0 ? 1 : num_core-1`. On a 1-core machine `get_number_of_cores()` returns 1, so `num_core` becomes 0. C requires a VLA bound to be greater than zero; 0 is a constraint violation regardless of whether the array is indexed.

UBSan confirms: `runtime error: variable length array bound evaluates to non-positive value 0`.

The surrounding logic is correct — both loops are guarded by `num_core > 1`, so no threads spawn and the main thread rasterizes everything. Verified: the single-threaded path produces a byte-identical frame to the 8-worker path. This is latent UB, not an active failure.

**Fix path.** Clamp the declaration: `pthread_t threads[num_core > 0 ? num_core : 1];`.

---

## 4. `Object` cannot distinguish a triangle list from an edge list

**What breaks.** `render()` sends any object with `len_of_connectors != 0` to the triangle rasterizer, which reads `connectors_sequence` three at a time. Nothing records how many indices per primitive the array actually holds.

**Why.** `model_data.h` ships two index arrays over the same vertices: `model_connectors` (pairs, 392,102 entries) and `model_tri_connectors` (triples, 2,534,133 entries). They differ only by which field an `Object` literal points at. 392,102 is not divisible by 3, so feeding the pair list to the raster path both draws nonsense *and* over-reads the end of the array — the worker loop's `p < len_of_connectors` bound is only safe because a triangle list's length is always a multiple of 3.

**Fix path.** Either add an explicit primitive-kind tag to `Object` (also removes the `len_of_connectors == 0` point-cloud dispatch trick), or make the worker bound independent of the assumption: `while(p + 2 < obj.len_of_connectors)`.

---

## 5. Camera rotation

**What breaks.** There is no way to rotate the camera. `move_camera(unit, Movement)` handles translation only.

**Why.** World points are consumed by the projection directly. There is no view matrix, no per-Object model matrix, no matrix pipeline at all. Rotating an Object today means mutating its `vertices` array in place — which literally moves the model in the world, so you cannot tell "Object rotated" apart from "camera rotated the other way relative to a stationary Object."

**Fix path.** Add a 4x4 transform matrix to `Object` (the model matrix) and one to `Camera` (the view matrix). Multiply each vertex by `view * model` before projection.

---

## 6. No back-face culling

**What breaks.** Every triangle of a closed mesh is rasterized, including the roughly half facing away from the camera. They are then discarded by the depth compare, so the output is correct but up to twice the fill work is wasted — and under issue 1, back faces are extra contention on the same pixels.

**Why.** There is no winding-order test. `clip_triangle_near` also does not preserve winding when it splits a triangle, so a culling test would need to establish orientation from the projected vertices rather than trusting index order.

**Fix path.** After projecting the three vertices, take the sign of the 2D cross product of two edges and skip triangles facing away. Decide a winding convention first, and make the clip respect it.

---

## 7. Depth and colour interpolation is not perspective-correct

**What breaks.** Depth arbitration and the per-pixel colour lerp both interpolate linearly in *screen* space. Under perspective projection that is close but wrong — foreshortening biases the true value toward the farther endpoint. Visible as a faint seam along the diagonal where the two triangles of a quad meet, on any face with a strong colour gradient.

Note this applies only to the raster-time lerp. `lerp_vectex` in the near clip interpolates in world space along a straight 3D edge, which is exact and unaffected.

**Fix path.** Interpolate `1/z` linearly in screen space and invert per pixel; interpolate `colour/z` the same way. The same machinery gives perspective-correct texture coordinates later.

---

## 8. `clear_frame_buffer(true)` leaks the old buffer

**What breaks.** Calling it with `keep_frame == true` leaks the previous frame buffer — one full `screen_width * screen_height * sizeof(PixelCord)` allocation per call (48 MB at 1500x1000).

**Why.** `renderer.c:158`:

```c
if(!keep_frame) free(frame);
if(screen_hieght!=0 && screen_width!=0) create_frame_buffer(screen_width,screen_hieght);
```

`create_frame_buffer` unconditionally overwrites `frame` with a fresh `calloc`, so skipping the `free` does not keep the old buffer reachable — it strands it. No caller passes `true` today (all three call sites pass `false`), but `ARCHITECTURE.md` documents the flag as meaning the old buffer "still exists and can be referenced".

**Fix path.** Decide what the flag means. If it is "reuse the existing allocation", return early instead of reallocating. If it is "hand ownership of the old buffer to the caller", it has to return the pointer. `create_frame_buffer` also never checks its `calloc` result; `render()` guards on `frame == NULL` but `update_win()` does not.

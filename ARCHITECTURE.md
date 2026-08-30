# Architecture

Design notes for the renderer. Companion to `issues.md` — that file tracks what's broken; this one tracks what exists and why.

---

## 1. Modules

| File | Responsibility |
| --- | --- |
| `src/renderer.c` | Renderer state (camera, frame buffer, object list), `render()` dispatch, thread pool lifecycle |
| `src/rasterization.c` | Near-plane clip, projection, scanline triangle fill. The worker body. |
| `src/wireframe.c` | `bresenhame_line_algo` — walks one segment, emits one `PixelCord` per step with interpolated z and colour |
| `src/projection.c` | `perspective_projection` — one axis of the perspective divide |
| `src/interpolation.c` | Scalar and per-channel colour lerps |
| `src/png.c` | PNG decoder: container, CRC32, unfiltering, all colour types and bit depths to RGBA8 |
| `src/inflate.c` | DEFLATE (RFC 1951) and its zlib wrapper (RFC 1950), for PNG's IDAT stream |
| `src/utils.c` | `sort_pixelcords_by_px` (insertion sort), `get_number_of_cores` |
| `src/win_i_o.c` | SDL window, event loop, frame-buffer → texture blit |

`renderer.c` owns the state; `rasterization.c` reaches it through `get_camera_pos()`, `get_frame_buffer()`, `get_current_object()` and the `screen_width` / `screen_hieght` externs.

---

## 2. Pipeline

```
Object[] ──▶ render() ──┬─▶ point-cloud path      (single-threaded)
                        └─▶ rasterization_worker  (thread pool)
                                │
                                ├─ clip_triangle_near   world space, against z = camera.z + 1
                                ├─ perspective_projection   world ──▶ pixel
                                ├─ bresenhame_line_algo x3   one strip per edge
                                ├─ scanline gather      cursor per edge, monotonic in py
                                └─ span fill + depth test ──▶ PixelCord grid (`frame`)
```

Dispatch is by connector count, not by an explicit tag:

```
for obj in objects:
    if obj.len_of_connectors == 0:  point-cloud path (one pixel per vertex)
    else:                           triangle raster path (3 indices per triangle)
```

**Wireframe mode is no longer supported.** `render()` has no mode flag and there is no dispatch to a line path; the renderer draws point clouds and filled triangles only. `bresenhame_line_algo` survives as the edge walker inside the triangle path, which is now its only caller.

---

## 3. API

- `render_init(objs, len, win_w, win_h)` — allocates the frame buffer, records the object array and screen size, derives the camera lens, and caches the worker count. Call once.
- `set_objects(objs, len)` — re-point the renderer at the caller's object array. See §9. Not safe to call concurrently with `render()`.
- `render()` — walks every `Object` and fills the frame buffer. Returns false only if the frame buffer is NULL.
- `clear_frame_buffer(keep_frame)` — allocates a fresh grid at the current screen size. `keep_frame == true` is currently broken (`issues.md` §7).
- `renderer_resize(win_w, win_h)` — updates screen size, re-derives the camera, reallocates the frame buffer.
- `move_camera(unit, direction)` — translates the camera by `unit` world units along one axis. Near value and matching far-plane extent shift together, so the view volume slides rigidly. Does *not* clear the frame buffer; the caller does that.
- `get_camera_pos()` — snapshot copy of the `Camera`. Mutating the result does nothing.
- `get_frame_buffer()` — raw pointer to the `PixelCord` grid.
- `get_current_object()` — the `Object` the worker pool is currently on. Only meaningful between thread spawn and join.

---

## 4. Threading model

Work is partitioned **by triangle**, using a shared atomic cursor rather than a precomputed split:

- `render_init` sets `num_core = get_number_of_cores() - 1` (one less, because the main thread also rasterizes).
- Per object, `render()` resets `triangle_tracker` to 0, spawns `num_core` workers, runs `rasterization_worker` on the main thread too, then joins.
- Each worker calls `atomic_fetch_add(&triangle_tracker, 3)` to claim the next triangle, so chunks are handed out as 0, 3, 6, … Threads that finish a cheap triangle immediately claim another — self-balancing, which matters because triangle cost varies by orders of magnitude with screen area.
- `object` and the frame buffer are published before `pthread_create` and read after `pthread_join`, so those are ordered. **The frame buffer itself is not synchronised** — this is the significant open bug, `issues.md` §1.

Only the triangle path is threaded. The point-cloud path runs on the calling thread.

---

## 5. Core types

**`Vectex`** — world-space position (`x`,`y`,`z`) plus a 32-bit colour. The input format.

**`PixelCord`** — screen-space position (`px`,`py`), the world-space depth `z` it came from, its colour, and two flags. 32 bytes. Serves three roles: a projected vertex, one step along a Bresenham strip, and a frame-buffer cell.

- `.in_use` — this cell has been written by `render()`. Doubles as the z-buffer occupancy bit: an unused cell always loses the depth test.
- `.is_visible` — passed the frustum / screen-bounds test. Cells failing it are never stored.

**`Object`** — `vertices[]` plus a flat `connectors_sequence[]` of indices into it, read three at a time. Colour is carried per vertex only; there is no per-object fallback. `len_of_connectors == 0` means point cloud.

**`Camera`** — axis-aligned frustum: position `(x,y,z)`, far-plane extents `(x_end,y_end,z_end)`, and lens fields `focal_l`, `v_fov`, `h_fov`. No rotation (`issues.md` §4).

---

## 6. Camera and projection

The camera is a **pinhole**. The lens sits at `camera.z`; magnification of an object at depth `Z` is `focal_l / Z`. The view volume is a cone whose apex is the camera itself — at `camera.z` the visible world width is exactly zero.

`v_fov` is the fixed input (60°); everything else is derived in `setup_camera`:

```c
focal_l = screen_hieght / (2 * tan(v_fov/2))     // 866.03 at a 1000px-high window
h_fov   = 2 * atan(screen_width / (2 * focal_l)) // 81.79° at 1500x1000
```

**`focal_l` is in pixels, not world units.** It is a units conversion between a chosen field of view and a chosen pixel count, which is why `perspective_projection` produces pixels from `world_offset * focal_l / world_depth`. Where the "image plane" sits in world space is a convention, not a fact about the code — slicing the view cone at any distance gives the same picture at a different scale.

`camera.x`/`x_end` and `camera.y`/`y_end` are the frustum's extents **at the far plane**, derived from `focal_l` and `z_end`. `is_vectex_visible` scales them back to a point's own depth to test containment.

### Screen-bounds contract

`is_vectex_visible` tests a world-space point against the frustum by scaling `camera.x`/`x_end` and `camera.y`/`y_end` — the extents at the far plane — back to that point's own depth. That test is the algebraic inverse of `perspective_projection`, which gives the invariant the renderer relies on:

> a point that passes `is_vectex_visible` always projects to a pixel inside `[0, screen_width)` x `[0, screen_hieght)`.

That is why the point-cloud path writes `frame_buffer[py][px]` without a bounds check — visibility is decided in world space, before projecting, and the two calculations cannot disagree. Verified against vertices at `z = -50` (behind the lens, frustum interval inverts and rejects), `z = 0` (interval collapses to empty) and `z = 0.001` (interval is a fraction of a world unit wide): all culled, nothing out of range.

The invariant is not asserted anywhere, so it is a real constraint on both functions: changing the projection formula without matching `is_vectex_visible` reintroduces out-of-bounds writes. The triangle path does not depend on it — Bresenham re-gates every pixel and the span fill clamps to the screen before writing.

### Near plane

`NEAR_PLANE_MARGIN` (1.0 world unit, `rasterization.c`) is the **minimum object distance** — nothing may come closer than that to the lens. It is not a surface anything is drawn on, and it is unrelated to `focal_l`.

It exists because the perspective divide degenerates at the lens: magnification runs to infinity as depth → 0, `depth == 0` bypasses the divide entirely and leaks a world coordinate into a pixel coordinate, and negative depth flips the sign so geometry behind the camera projects *mirrored onto the screen* rather than disappearing. Worse, a segment that crosses the camera plane has no straight-line image at all — its true image runs off to infinity in one direction and returns from the other — so no clamp applied after the divide can fix it.

`clip_triangle_near` therefore cuts in **world space, before projecting**, where the geometry is still linear. It returns 0, 1 or 2 triangles:

| inside corners | result |
| --- | --- |
| 0 | dropped |
| 1 | a smaller triangle: the survivor plus two edge crossings |
| 2 | a quad (2 survivors + 2 crossings), split on a diagonal into 2 triangles |

Surviving vertices are copied verbatim; new vertices are lerped along the original edges, so the output is a genuine sub-region of the source triangle — a cut, not a relocation. `lerp_vectex` solves for `t` from z alone, then applies that same `t` to x, y and colour. Any future per-vertex attribute rides the same `t`.

The clip bounds magnification at `focal_l / margin` and guarantees a positive divisor, but it does **not** bound the projected coordinate — a vertex on the near plane far off-axis still projects tens of thousands of pixels away. Row and span clamping in the fill loop handle that separately.

---

## 7. Coordinate system

- World units and pixel units coincide only at depth `focal_l`. Elsewhere the ratio is `focal_l / depth`.
- `+Y is down` on screen (importers must flip glTF's +Y-up).
- Row 0 is the top of the window.
- Colour is packed `0xAARRGGBB`, matching SDL's `SDL_PIXELFORMAT_ARGB8888`. Interpolation unpacks and lerps per channel — blending the packed word would carry bits between channels.

---

## 8. Scanline fill

Per triangle, after clipping and projecting:

1. Bresenham each of the 3 edges into its own heap strip, one `PixelCord` per step carrying interpolated z and colour.
2. Walk rows from `min_y` to `max_y`, clamped to the screen.
3. Gather each row's pixels using **one cursor per edge**. Bresenham strips are monotonic in `py`, so a cursor only ever advances — no rescanning. The cursor direction is chosen once per edge from whether the strip ascends or descends.
4. Sort the row's pixels by `px` (insertion sort; rows are short).
5. Fill between every consecutive pair, so a row ends up painted from its leftmost edge pixel to its rightmost.

The row table is sized from the **summed edge lengths**, not the bounding-box width: a near-horizontal edge can deposit an entire strip onto one row, so bbox width is not an upper bound on a row's occupancy.

Interpolation across the span is linear in screen space, which is not perspective-correct (`issues.md` §6).

---

## 9. Frame buffer ownership

- Allocated by `create_frame_buffer(w,h)` with `calloc`, stored as `void* frame`.
- Reinterpreted as `PixelCord (*)[screen_width]` inside `render()` and `rasterizer()`; as a flat `PixelCord*` inside `update_win()`.
- Never cleared per frame automatically — callers call `clear_frame_buffer(false)` (currently `renderer_resize` and the event handlers).
- Depth arbitration is the `.in_use` + `.z` compare at each write site. There is no separate depth array.
- `update_win` flattens it to ARGB: `.in_use` cells paint their own colour, empty cells paint white.
- `Object.vertices` and `Object.connectors_sequence` are owned by whoever created the `Object`; the renderer never frees them.

### Scene list

The scene is a flat `Object` array plus a length, held as `objects` / `objects_len` in `renderer.c`. `render()` indexes it directly.

**The renderer only reads it.** Allocating the array, growing it, reallocating it and freeing it are all the caller's job. `set_objects(objs, len)` re-points the renderer afterwards — necessary because `realloc` may move the array, which would leave the renderer's stored base pointer dangling.

The usual caller-side shape:

```c
if(len == cap){ cap = cap ? cap*2 : 8; scene = realloc(scene, cap*sizeof(Object)); }
scene[len++] = obj;
set_objects(scene, len);        // base pointer may have moved
```

Growth is amortised O(1) with doubling, and a contiguous array is what `render()` wants anyway — it walks every object every frame, and a linked list of separately allocated nodes costs roughly 3x at a thousand objects and far more beyond that, because each hop is a dependent load the prefetcher cannot anticipate.

`set_objects(NULL, n)` forces the length to 0 rather than trusting `n`, so a cleared scene cannot be walked through a null pointer. Not synchronised: it must be called from the same thread as `render()`.

### Mesh loading

`include/mesh.h` declares one entry point per format plus a detect-and-dispatch wrapper. Sources: `src/mesh.c` (dispatch, free, fit), `src/mesh_ply.c`, `src/mesh_obj.c`, `src/mesh_gltf.c`, and `src/json.c` — a minimal read-only JSON parser written for the glTF chunk.

```
mesh_load(path, out)
  detect by magic bytes, fall back to extension
  -> mesh_load_ply    ascii / binary_little_endian / binary_big_endian
  -> mesh_load_obj    text
  -> mesh_load_gltf   .glb container and .gltf (external or data: URI buffers)
```

Every loader emits a **triangle list**, because `render()` infers the primitive kind from `len_of_connectors` and reads three at a time. n-gons are fan-triangulated at load; anything that cannot be expressed as triangles is refused with `MESH_ERR_UNSUPPORTED` rather than reinterpreted.

Ownership matches the scene-array rule in §9: the loader `malloc`s `vertices` and `connectors_sequence`, the caller releases them with `mesh_free`, and the renderer only reads.

`mesh_fit_to_view` is separate from parsing on purpose. Loaders return the model in its own units and origin; the fit pass rescales the longest axis to a target extent, recentres, and mirrors y (glTF and OBJ are +y up, this renderer is +y down). Without it a model is either a speck or swallows the screen, since the camera is a fixed pinhole at the world origin.

What the loaders deliberately refuse rather than approximate: glTF sparse accessors, morph targets, skinned meshes, any `extensionsRequired` entry, Draco and meshopt compression, and any primitive mode other than TRIANGLES. Each of those would otherwise return geometry that is silently the wrong shape.

Colour is read where the format carries it — PLY `red/green/blue`, the non-standard OBJ `v x y z r g b` extension, glTF `COLOR_0`.

### Texture baking

glTF almost never stores colour per vertex; it stores materials pointing at texture images. The apartment model's five 4096x4096 textures hold 83.9 million texels against 145,932 vertices — the format deliberately decouples appearance resolution from mesh density, which is why per-vertex colour is rare outside scanner output.

Since `Vectex` has no UV field and the rasterizer lerps a colour rather than sampling an image, the loader closes the gap at load time: it decodes `baseColorTexture` and samples it once per vertex at that vertex's `TEXCOORD_n`, baking the result into `Vectex.colour`. Per primitive the precedence is `COLOR_0`, then texture sample (multiplied by `baseColorFactor` when both exist), then `baseColorFactor` alone, then `MESH_DEFAULT_COLOUR`.

`src/png.c` and `src/inflate.c` exist for this: a PNG decoder over a from-scratch DEFLATE/zlib implementation, no external dependencies. Decoded images are cached per image index and freed on every exit path — the tree's four 2048x2048 textures are 64MB of RGBA, and its 844,711 triangles share them.

This is a lossy stand-in for texturing, bounded by vertex density: it turned the tree from one flat grey into 54,791 distinct colours because that mesh is dense, but a room built from large flat walls would come out as soft blocks. Real texturing needs `u,v` on `Vectex` and a sampler in the span fill. JPEG textures are not decoded at all — see `issues.md` §8.

---

## 10. Deliberate omissions (see `issues.md`)

- No matrix pipeline → no camera rotation, no per-object transform.
- No back-face culling.
- No perspective-correct interpolation.
- No wireframe rendering. Dropped deliberately when rasterization moved to `rasterization.c`; the Bresenham walker remains as a triangle-edge utility.
- No explicit primitive tag on `Object` — dispatch infers it from connector count, and nothing records indices-per-primitive.
- No texture sampling in the mesh loaders — a model coloured only by a texture loads flat.
- No per-object colour. Every vertex carries its own.

---

## 11. Open design questions

<!-- your notes here. -->
<!-- - should Object gain an explicit `kind` tag instead of connector-count dispatch? -->
<!-- - when the view matrix lands, does `Camera` become a matrix or keep the frustum box for culling? -->
<!-- - partition rasterization by triangle (current, races on shared pixels) or by screen row/tile (no shared pixels, but every thread clips and projects every triangle)? -->

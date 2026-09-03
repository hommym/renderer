# Architecture

Design notes for the renderer. Companion to `issues.md` — that file tracks what's broken; this one tracks what exists and why.

---

## 1. Modules

| File | Responsibility |
| --- | --- |
| `src/renderer.c` | Renderer state (camera, frame buffer, object list), `render()` dispatch, thread pool lifecycle |
| `src/rasterization.c` | Near-plane clip, projection, scanline triangle fill. The worker body. |
| `src/wireframe.c` | `bresenhame_line_algo` — walks one segment, emits one `PixelCord` per step with interpolated z and texture coordinates |
| `src/projection.c` | `perspective_projection` — one axis of the perspective divide |
| `src/interpolation.c` | Scalar and per-channel colour lerps |
| `src/transform.c` | Camera basis and the world-to-view step |
| `src/utils.c` | `sort_pixelcords_by_px` (insertion sort), `get_number_of_cores` |
| `src/win_i_o.c` | SDL window, event loop, frame-buffer → texture blit |
| `src/mesh*.c`, `src/json.c` | Mesh file loading — see §9 |
| `src/image.c` | Format sniff and the packed-ARGB conversion both decoders feed |
| `src/png.c`, `src/inflate.c` | PNG container and DEFLATE/zlib (RFC 1951/1950) |
| `src/jpeg.c` | Baseline JPEG (T.81 sequential DCT, Huffman, 8-bit) |

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

### Frame buffer locking

Work is handed out **by triangle**, so two threads routinely reach the same
pixel, and the depth test there is a read-modify-write:

```c
existing = frame_buffer[y][x];   if(closer) frame_buffer[y][x] = pixel;
```

Unsynchronised that loses updates -- both threads read the same `existing`, both
conclude they are in front, and the second write clobbers the first -- and it
tears, because a `PixelCord` is 40 bytes and no store that wide is atomic.

A lock per pixel is impossible (1.5M of them) and one lock for the buffer would
remove the point of threading. The unit used is a **row band**: the scanline pass
writes one row at a time, so a whole triangle-row is a single acquire, and two
threads only wait on each other when their rows collide modulo the band count.

- 256 bands, indexed `row & 255`, each padded to its own cache line. Packed
  together, locking two different bands would still bounce one line between cores
  and reintroduce most of the contention the banding exists to avoid.
- Spinlocks, not mutexes: the critical section is one row of one triangle, so
  waiting is cheaper than a round trip into the kernel, and there is never more
  than one thread per core to be descheduled while holding one.
- Taken after the row's gather and sort, which touch only thread-local memory.
- A row outside the screen is never locked; nothing can be stored there.

Measured: torn writes go from 210 over six frames to **0**, and depth becomes
identical on every pixel of every run. The cost is 11-21% of frame time
(`dae_-_eco_house` 54.6 -> 66.3 ms, `woman_seated_v12` 28.2 -> 31.4 ms,
`corrupted_archangel` 361.9 -> 414.1 ms), which is the price of roughly half a
million uncontended atomics per frame.

The point-cloud path in `render()` takes no lock: it runs on the calling thread
and finishes before any worker is spawned.

What locking does *not* fix is which of two fragments at exactly the same depth
wins -- that is a tie the depth test resolves by arrival order (`issues.md` 1).

## 5. Core types

**`Vectex`** — world-space position (`x`,`y`,`z`), a texture coordinate (`u`,`v`), and a 32-bit colour. The input format.

`u` and `v` are the drawn colour; `colour` is not (`issues.md` §8). Both must lie in `[0,1)` — the rasterizer turns them into array subscripts with no bounds check, so `1.0` is one texel past the end. `mesh.h` makes that part of the load contract.

**`PixelCord`** — screen-space position (`px`,`py`), the world-space depth `z` it came from, its texture coordinate, its resolved colour, and two flags. Serves three roles: a projected vertex, one step along a Bresenham strip, and a frame-buffer cell.

- `.in_use` — this cell has been written by `render()`. Doubles as the z-buffer occupancy bit: an unused cell always loses the depth test.
- `.is_visible` — passed the frustum / screen-bounds test. Cells failing it are never stored.

**`Object`** — `vertices[]` plus a flat `connectors_sequence[]` of indices into it, read three at a time, plus one texture (`texture`, `texture_width`, `texture_height`) shared by every vertex. `len_of_connectors == 0` means point cloud.

`texture` is dereferenced unconditionally by both draw paths, so it is never allowed to be NULL: an object with no image gets a 1x1 texture of its flat colour instead. One texture per object is also the whole multi-material limitation (`issues.md` §10).

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

### Back-face culling

Roughly half the triangles of a closed mesh point away from the eye. They were
always discarded eventually -- by the depth test -- but only after paying for a
clip, three projections, three Bresenham walks and a full scanline fill.

The test is the sign of `N . (a - eye)`, where `N = (b-a) x (c-a)` is the plane
normal: it says which side of the triangle's plane the eye is on, which is the
same question as which face it can see. No projection and no division.

Two things about where it sits:

**Before the near-plane clip, not after.** Clipping only cuts a triangle up
within its own plane, so every piece it produces has the same normal and the
same facing as the whole. One test on the source triangle covers all of them --
and it sidesteps the fact that `clip_triangle_near` does not preserve winding
(`issues.md` 5).

**Gated on `Object.double_sided`.** glTF marks materials `doubleSided`, and a
material that says so must not be culled -- foliage cards and single-sided walls
are modelled from one side only and vanish otherwise. PLY and OBJ carry no such
concept, so they are treated as double sided: "we cannot tell" has to mean "do
not throw geometry away".

`set_backface_cull_forced(true)` overrides the flag for the cases where an
exporter set it by default on a mesh that does not need it. `issues.md` 11 has
the measured cost per model.

---

### View transform

The camera can turn, and **nothing downstream of the transform knows it can**.

`projection.c`, `clip_triangle_near` and `is_vectex_visible` were all written
against a camera that looks straight down +z out of an axis-aligned box. Rather
than teach three separate pieces of code about rotation, every vertex is rotated
into the camera's frame first (`src/transform.c`):

```
v' = eye + R^T * (v - eye)
```

Rotate about the eye, then put the result back at the eye. After that the
camera's forward direction *is* +z and the geometry sits in exactly the
situation the existing code already handles, so the perspective divide, the
near-plane clip and the frustum cull are untouched. At zero rotation the whole
function is the identity.

Putting the result back at the eye (rather than moving the eye to the origin, as
a textbook view matrix does) is what keeps `camera.z`, `x_end` and the rest of
the box meaningful: `depth = z - camera.z` is still the distance along the view
direction, and `x - x_center` is still the sideways offset.

`R` is built from two angles, not stored as a matrix. `yaw` turns about the
vertical, `pitch` about the horizontal, and the basis is

| axis | vector | note |
| --- | --- | --- |
| right | `( cos y, 0, -sin y )` | yaw only, so strafing never drifts vertically |
| down | `( sin y sin p, cos p, cos y sin p )` | `fwd x right`; +y is down, hence *down* not *up* |
| forward | `( sin y cos p, -sin p, cos y cos p )` | `-sin p` because pitching up moves towards -y |

Orthonormal, so `R^T` *is* `R^-1` and there is nothing to invert at runtime.
Pitch is clamped to +/-89 degrees: at exactly vertical the horizontal heading is
undefined and the view rolls through it.

The basis is rebuilt once per frame by `view_refresh()` at the top of `render()`
— the trigonometry is the expensive part and the camera does not move mid-frame.
`view_apply()` is then nine multiplies. The rasterization threads only read it.

`move_camera` uses the same basis: forward follows the gaze including pitch,
left/right strafe along the horizontal `right` axis, and up/down deliberately
stay on the world vertical so looking down and pressing up does not fly you into
the floor.

---

## 7. Coordinate system

- World units and pixel units coincide only at depth `focal_l`. Elsewhere the ratio is `focal_l / depth`.
- `+Y is down` on screen (importers must flip glTF's +Y-up).
- Row 0 is the top of the window.
- Colour is packed `0xAARRGGBB`, matching SDL's `SDL_PIXELFORMAT_ARGB8888`. Interpolation unpacks and lerps per channel — blending the packed word would carry bits between channels.

---

## 8. Scanline fill

Per triangle, after clipping and projecting:

1. Bresenham each of the 3 edges into its own heap strip, one `PixelCord` per step carrying interpolated z and `u`/`v`.
2. Walk rows from `min_y` to `max_y`, clamped to the screen.
3. Gather each row's pixels using **one cursor per edge**. Bresenham strips are monotonic in `py`, so a cursor only ever advances — no rescanning. The cursor direction is chosen once per edge from whether the strip ascends or descends.
4. Sort the row's pixels by `px` (insertion sort; rows are short).
5. Fill between every consecutive pair, so a row ends up painted from its leftmost edge pixel to its rightmost.

The row table is sized from the **summed edge lengths**, not the bounding-box width: a near-horizontal edge can deposit an entire strip onto one row, so bbox width is not an upper bound on a row's occupancy.

Colour is resolved last, per pixel: the span lerp carries `u`/`v`, and `texture[(size_t)(h*v)][(size_t)(w*u)]` is read at the write site. Interpolation across the span is linear in screen space, which is not perspective-correct (`issues.md` §6) and shows up more sharply on a texture than it did on a colour gradient.

---

## 9. Frame buffer ownership

**Two buffers, both owned by `src/renderer.c`, and nothing outside that file can
create, resize or release either one.**

- Allocated in exactly two places: `render_init()` and `renderer_resize()`. Never
  per frame.
- `render()` wipes the back buffer with `memset`, draws into it, and swaps at the
  end. The swap is a pointer flip; nothing is copied.
- `get_frame_buffer()` returns a `const PixelCord*` to the FRONT buffer -- always
  a complete frame, valid until the next `render()` or resize.
- `renderer_back_buffer()` returns the mutable half-drawn frame, for the
  rasterization workers only.
- `update_win` flattens the front buffer to ARGB: `.in_use` cells composite their
  own colour over the background by its alpha, empty cells paint the background.
- Depth arbitration is the `.in_use` + `.z` compare at each write site. There is
  no separate depth array.

This replaced a single buffer that was freed and `calloc`'d again *every frame*.
That was two problems in one. It was 60MB released and 60MB zero-filled per frame
at 1500x1000 -- worth 28% of the eco house's frame time and 47% of the zbrush
mech's, since the fixed allocation cost dominates a light scene. And it handed
out a raw `void*` that the next clear silently invalidated, so any caller holding
the result of `get_frame_buffer()` across a frame was reading freed memory.

Neither buffer is released at shutdown. They are reachable from a static for the
life of the process, which is deliberate: there is no teardown path, and freeing
them at exit would only give the allocator work to do on the way out.

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

One public entry point. Everything else in `mesh.h` is a step it is built from.

```
mesh_import(path, extent, cx,cy,cz, &model)      <-- the one callers use
  |
  +- mesh_detect_format   magic bytes first, filename extension as a fallback
  +- mesh_load_scene      parse + split by material
  |    -> mesh_load_ply     ascii / binary_little_endian / binary_big_endian
  |    -> mesh_load_obj     text
  |    -> mesh_load_gltf    .glb container and .gltf (external or data: URI buffers)
  +- mesh_model_fit       scale the longest axis to `extent`, recentre, flip y

  out: Model { Object* objects; uint64_t len; ... }
       model.objects IS the array render_init() takes.
```

`mesh_load` still exists and still returns a single merged Object, for callers
that want geometry and do not care about colour (a bounding box, a triangle
count, a converter). It is not the default: a caller cannot tell from a filename
whether a file is single-material, so choosing it is a guess that fails silently
when wrong.

Every loader emits a **triangle list**, because `render()` infers the primitive kind from `len_of_connectors` and reads three at a time. n-gons are fan-triangulated at load; anything that cannot be expressed as triangles is refused with `MESH_ERR_UNSUPPORTED` rather than reinterpreted.

Ownership matches the scene-array rule in §9: the loader `malloc`s `vertices`, `connectors_sequence` and `texture`, the caller releases all three with `mesh_free`, and the renderer only reads.

`mesh_fit_to_view` is separate from parsing on purpose. Loaders return the model in its own units and origin; the fit pass rescales the longest axis to a target extent, recentres, and mirrors y (glTF and OBJ are +y up, this renderer is +y down). Without it a model is either a speck or swallows the screen, since the camera is a fixed pinhole at the world origin.

What the loaders deliberately refuse rather than approximate: glTF sparse accessors, morph targets, skinned meshes, any `extensionsRequired` entry, Draco and meshopt compression, and any primitive mode other than TRIANGLES. Each of those would otherwise return geometry that is silently the wrong shape.

#### Colour: texture coordinates and one image

Colour reaches the screen through the texture, not the vertex, so a loader has three jobs past geometry: fill `u`/`v`, keep them inside `[0,1)`, and always produce a texture.

| | texture coordinates | image |
| --- | --- | --- |
| glTF / GLB | `TEXCOORD_0` (FLOAT, or normalized UBYTE/USHORT) | `material.pbrMetallicRoughness.baseColorTexture` → `textures[].source` → a bufferView, a base64 `data:` URI, or a sibling file |
| OBJ | `vt`, indexed separately from `v` | `mtllib` → `usemtl` → `map_Kd` |
| PLY | `s`/`t`, `u`/`v`, `texture_u`/`texture_v`, `texture_s`/`texture_t` | `comment TextureFile <name>` |

Three things follow from the formats rather than from choice:

**OBJ and PLY put the uv origin at the bottom-left**, glTF at the top-left. The renderer indexes texture rows from the top, so the first two are loaded as `1 - v` and glTF is passed through. This is the single line to suspect if a model ever appears vertically mirrored.

**OBJ indexes positions and texture coordinates separately** — `f 4/1413/4` is position 4 with uv 1413 — so a position reused with a different uv is two different vertices here. Corners are keyed on the `(position, uv)` pair and split where the atlas seam runs. The castle OBJ turns 500,665 positions into 555,282 vertices that way. De-duplicating by position alone would pick one of the uvs arbitrarily and smear the texture across the seam.

**Coordinates outside the unit square are ordinary**, not an error — a tiled floor is authored with `u` running 0 to 8. `mesh_wrap_uv` folds those the way a REPEAT sampler would, but leaves a value already inside `[0,1]` alone apart from nudging exactly `1.0` down to the largest float below it. Wrapping `1.0` to `0.0` would be right for a tile boundary and catastrophic for an atlas: every full-range quad would collapse onto texel `(0,0)`.

#### One Object per material

`Object` holds one image, so a file painted with several materials cannot be one Object. Merging them does not lose the colour, it puts the *wrong* colour on most of the mesh: the uv coordinates stay valid numbers, so geometry belonging to material 7 addresses whatever happens to sit at those coordinates in material 2's picture. On the appartement GLB that turned an interior into an almost entirely black shape, because the surviving image is a mostly-black atlas.

So `mesh_load_scene` returns a `Model`: one Object per material, each with its own texture, handed to the renderer as the object array it already walks. Textures are shared *between* those Objects and owned by the scene, because several materials commonly sample one atlas and a 2048x2048 image is 16MB — decoding per material instead of per image would multiply a 40-material model's texture memory by 40.

`mesh_load` still exists and still returns a single Object; it merges the scene and keeps the dominant material's texture. That is the lossy path, kept because a single-material file is the common case and the caller should not need a scene to load one.

A material also chooses *which* uv set it samples, via `baseColorTexture.texCoord`. Baked-lighting exports routinely put the baked atlas on `TEXCOORD_1` and leave `TEXCOORD_0` for a tiling detail map — 4 of the appartement's 5 materials do exactly this — so the loader reads the set the material names rather than assuming set 0.

A file with no usable image — no texture, an undecodable one, or a path that escapes the model's directory — gets a 1x1 texture instead of a NULL pointer, coloured from `baseColorFactor` (glTF), `Kd` (OBJ), the average vertex colour (PLY), or `MESH_DEFAULT_COLOUR`. `mesh.c`'s `validate()` re-checks both invariants before any Object is handed back, the same way it re-checks the index list: the rasterizer trusts them absolutely.

A texture path read out of a model file is resolved against that file's own directory and refused if it is absolute or contains `..` — an mtl is opened with the process's privileges, and a mesh file is untrusted input.

#### Image decoding

`include/image.h` is the one front door; the loaders never branch on PNG vs JPEG.

- **PNG** (`png.c` + `inflate.c`): every non-interlaced file — colour types 0/2/3/4/6, bit depths 1–16, PLTE, tRNS, IDAT across any number of chunks. Adam7 is refused rather than mis-decoded. Verified byte-identical to Pillow.
- **JPEG** (`jpeg.c`): baseline and extended sequential (SOF0/SOF1), 8-bit, 1 or 3 components, any sampling factors, restart intervals. Progressive, arithmetic, lossless and 12-bit are refused with a reason. Within ±3/255 of Pillow on 0.07% of channels — integer-IDCT rounding.

`MESH_TEXTURE_MAX_DIM` (2048) caps the longest side. Model diffuse maps run to 8192x8192, which is 268MB as 32-bit pixels for a rasterizer drawing into a window a fraction of that. The JPEG decoder reduces during its own colour-convert pass, so the full-size buffer is never allocated: the 8192² castle map peaks at 132MB instead of 268MB. PNG is decoded then box-filtered.

---

## 10. Deliberate omissions (see `issues.md`)

- No matrix pipeline → no camera rotation, no per-object transform.
- No perspective-correct interpolation.
- No wireframe rendering. Dropped deliberately when rasterization moved to `rasterization.c`; the Bresenham walker remains as a triangle-edge utility.
- No explicit primitive tag on `Object` — dispatch infers it from connector count, and nothing records indices-per-primitive.
- No texture filtering. Sampling is nearest-neighbour at the texel the truncated `u`/`v` lands on; no bilinear, no mipmaps, so a minified texture aliases.
- No texture wrap or clamp at the sample site — the `[0,1)` invariant is held by the loaders (`issues.md` §7).
- One texture per `Object`. Multi-material files are split into several Objects (`mesh_load_scene`) rather than the renderer growing multi-texture support.
- No alpha. The decoders keep the channel and the frame buffer carries it, but nothing tests it, so a cutout texture paints as a solid card (`issues.md` §10).
- No lighting, so glTF `NORMAL` and OBJ `vn` are parsed past rather than stored.

---

## 11. Open design questions

<!-- your notes here. -->
<!-- - should Object gain an explicit `kind` tag instead of connector-count dispatch? -->
<!-- - when the view matrix lands, does `Camera` become a matrix or keep the frustum box for culling? -->
<!-- - partition rasterization by triangle (current, races on shared pixels) or by screen row/tile (no shared pixels, but every thread clips and projects every triangle)? -->

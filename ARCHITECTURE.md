# Architecture

Design notes for the renderer. Companion to `issues.md` — that file tracks what's broken; this one tracks what exists and why.

---

## 1. Modules

| File | Responsibility |
| --- | --- |
| `src/renderer.c` | Renderer state (camera, frame buffer, object list), `render()` dispatch, thread pool lifecycle |
| `src/rasterization.c` | Near-plane clip, projection, binning, scanline triangle fill. Both worker passes. |
| `src/wireframe.c` | `bresenhame_line_algo` — walks one segment, emits one `PixelCord` per step. **No longer on the frame path**: the edge-walk rasterizer replaced it. Kept as a line utility. |
| `src/projection.c` | `perspective_projection` — one axis of the perspective divide |
| `src/interpolation.c` | Scalar and per-channel colour lerps |
| `src/transform.c` | Camera basis and the world-to-view step |
| `src/utils.c` | `sort_pixelcords_by_px` (insertion sort, no longer on the frame path), `get_number_of_cores` |
| `src/win_i_o.c` | SDL window, event loop, frame-buffer → texture upload, present |
| `src/ui.c` | In-window model browser (§12). Owns the loaded `Model`. |
| `src/mesh*.c`, `src/json.c` | Mesh file loading — see §9 |
| `src/image.c` | Format sniff and the packed-ARGB conversion both decoders feed |
| `src/png.c`, `src/inflate.c` | PNG container and DEFLATE/zlib (RFC 1951/1950) |
| `src/jpeg.c` | Baseline JPEG (T.81 sequential DCT, Huffman, 8-bit) |

`renderer.c` owns the state. `rasterization.c` snapshots the camera once per frame in `rasterization_frame_begin()` rather than calling `get_camera_pos()` per vertex — it was being called 2.94M times a frame for 95K triangles, each one a ~104-byte struct copy.

---

## 2. Pipeline

```
Object[] ──▶ render() ──┬─▶ point-cloud path   (calling thread, before any worker)
                        └─▶ frame worker pool  (spawned once per FRAME)
                               │
                               │  per object, per chunk of 32768 triangles:
                               │
                               ├─ PASS A  setup, parallel by TRIANGLE
                               │    ├─ view_apply x3         world ──▶ view space
                               │    ├─ back-face cull        sign(N · (a − eye))
                               │    ├─ clip_triangle_near    against z = camera.z + 1
                               │    ├─ project x3            with 1/w in hand
                               │    ├─ screen bbox reject
                               │    └─ bin by row band       into this thread's own bins
                               │  ─────── barrier ───────
                               ├─ PASS B  fill, parallel by ROW BAND
                               │    ├─ sort 3 verts by y, walk the two active edges
                               │    └─ span fill + depth test ──▶ PixelCord grid
                               │  ─────── barrier ───────
                               └─ reset bins
```

Dispatch is by connector count, not by an explicit tag:

```
for obj in objects:
    if obj.len_of_connectors == 0:  point-cloud path (one pixel per vertex)
    else:                           triangle raster path (3 indices per triangle)
```

**Wireframe mode is no longer supported.** `render()` has no mode flag and there
is no dispatch to a line path; the renderer draws point clouds and filled
triangles only. `bresenhame_line_algo` is no longer called by anything — the
edge-walk rasterizer (§8) replaced it — and survives only as a line utility.

---

## 3. API

- `render_init(objs, len, win_w, win_h)` — allocates the frame buffer, records the object array and screen size, derives the camera lens, and caches the worker count. Call once.
- `set_objects(objs, len)` — re-point the renderer at the caller's object array. See §9. Not safe to call concurrently with `render()`.
- `render()` — walks every `Object` and fills the frame buffer. Returns false only if the frame buffer is NULL.
- `renderer_resize(win_w, win_h)` — updates screen size, re-derives the camera, reallocates both frame buffers and drops the rasterizer's bins (the band count follows screen height).
- `camera_reset()` — back to the startup camera. What a model swap calls.
- `renderer_shutdown()` — releases both frame buffers and the rasterizer scratch.
- `move_camera(unit, direction)` — translates the camera by `unit` world units along one axis. Near value and matching far-plane extent shift together, so the view volume slides rigidly. Does *not* clear the frame buffer; the caller does that.
- `get_camera_pos()` — snapshot copy of the `Camera`. Mutating the result does nothing.
- `get_frame_buffer()` — raw pointer to the `PixelCord` grid.
- `get_current_object()` — the `Object` the worker pool is currently on. Only meaningful between thread spawn and join.

---

## 4. Threading model

Work is partitioned **twice**, and which axis is used is the whole design.

**Threads are created once per FRAME**, not once per object. A 61-material model
was paying 61 × 7 `pthread_create` + `pthread_join` every frame — at ~25 µs each
that is 20 ms of bookkeeping before a pixel is drawn.

Inside a frame, each object is processed in chunks of 32768 source triangles, and
each chunk runs two passes separated by `pthread_barrier_wait`:

**Pass A — setup, partitioned by TRIANGLE.** Thread `t` takes a *statically
assigned, contiguous* range of the chunk. It transforms, culls, clips, projects
and screen-bbox-rejects each triangle, writes the survivors into **its own slice**
of a shared setup array, and records each survivor's index in **its own bins**,
one per row band it touches. Nothing is shared, so nothing is locked.

**Pass B — fill, partitioned by ROW BAND.** A band is 16 consecutive screen rows.
Bands are claimed with an atomic cursor, so **one thread owns a band outright**.
Every pixel therefore has exactly one writer for the whole frame.

Chunking is what makes this fit in memory: a setup record is ~128 bytes and the
near clip can double the count, so a 2M-triangle model's setup array would be
hundreds of MB. 32768 triangles is ~8 MB, reused for every chunk.

### Why the frame buffer needs no locks any more

There used to be 256 cache-line-padded spinlocks here, because work was handed
out by triangle only, and two threads therefore landed on the same pixel
routinely — and the depth test is a read-modify-write:

```c
existing = frame_buffer[y][x];   if(closer) frame_buffer[y][x] = pixel;
```

Unsynchronised that loses updates and tears a 40-byte `PixelCord` across two
writers. Locking fixed the corruption but cost **28% of profiled frame time**,
and it could not fix the other half of the problem: which of two fragments at
*exactly* the same depth wins was still decided by arrival order, so ~1000 pixels
changed between two renders of an identical frame.

Partitioning the fill by band solves both at once:

- **No lock.** One writer per pixel, so there is nothing to serialise.
- **Bit-exact reproducibility.** Thread `t`'s setup slice sits entirely below
  thread `t+1`'s, so walking a band's bins in thread order visits triangles in
  increasing source order. A depth tie now resolves the same way every run, on
  any number of cores — no tie-break comparison needed.

Measured across all 25 models in `3dmodels/`: two renders of the same frame are
**identical on every one of 307,200 pixels**, where the locked version differed
on up to 0.05% of them.

The point-cloud path still runs on the calling thread, before any worker exists.

### What is shared, and how it is published

- The camera snapshot (`rasterization_frame_begin`) and the view basis
  (`view_refresh`) are written before any thread starts and only read after.
- The setup array and the bins are written in pass A and read in pass B, ordered
  by the barrier between them.
- `bin_push` can fail to grow under memory pressure; it drops that triangle
  rather than aborting the frame.

---

## 5. Core types

**`Vectex`** — world-space position (`x`,`y`,`z`), a texture coordinate (`u`,`v`), and a 32-bit colour. The input format.

`u` and `v` are the drawn colour; `colour` is not (`issues.md` §3). The loaders still hold them in `[0,1)` as a load contract (`mesh.h`), but the rasterizer no longer *depends* on that: it clamps at the sample site (§8).

**`PixelCord`** — screen-space position (`px`,`py`), the world-space depth `z` it came from, its texture coordinate, its resolved colour, and two flags. Now serves one role: a frame-buffer cell. The rasterizer's own projected vertices live in a leaner `SetupTri` instead.

- `.in_use` — this cell has been written by `render()`. Doubles as the z-buffer occupancy bit: an unused cell always loses the depth test.
- `.is_visible` — passed the frustum / screen-bounds test. Cells failing it are never stored.

**`Object`** — `vertices[]` plus a flat `connectors_sequence[]` of indices into it, read three at a time, plus one texture (`texture`, `texture_width`, `texture_height`) shared by every vertex. `len_of_connectors == 0` means point cloud.

`texture` is dereferenced unconditionally by both draw paths, so it is never allowed to be NULL: an object with no image gets a 1x1 texture of its flat colour instead. One texture per object is also the whole multi-material limitation (`issues.md` §5).

**`Camera`** — axis-aligned frustum: position `(x,y,z)`, far-plane extents `(x_end,y_end,z_end)`, lens fields `focal_l`, `v_fov`, `h_fov`, and orientation `yaw`/`pitch` (§6). What it still lacks is a *model*-side transform (`issues.md` §1).

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

The invariant is not asserted anywhere, so it is a real constraint on both functions: changing the projection formula without matching `is_vectex_visible` reintroduces out-of-bounds writes. Only the point-cloud path depends on it now — the triangle path clamps every span to the screen before writing, and rejects on the projected bounding box rather than on vertex visibility (§8).

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
clip, three projections and a full scanline fill.

The test is the sign of `N . (a - eye)`, where `N = (b-a) x (c-a)` is the plane
normal: it says which side of the triangle's plane the eye is on, which is the
same question as which face it can see. No projection and no division.

Two things about where it sits:

**Before the near-plane clip, not after.** Clipping only cuts a triangle up
within its own plane, so every piece it produces has the same normal and the
same facing as the whole. One test on the source triangle covers all of them --
and it sidesteps the fact that `clip_triangle_near` does not preserve winding
(`issues.md` §2).

**Gated on `Object.double_sided`.** glTF marks materials `doubleSided`, and a
material that says so must not be culled -- foliage cards and single-sided walls
are modelled from one side only and vanish otherwise. PLY and OBJ carry no such
concept, so they are treated as double sided: "we cannot tell" has to mean "do
not throw geometry away".

`set_backface_cull_forced(true)` overrides the flag for the cases where an
exporter set it by default on a mesh that does not need it. `issues.md` §4 has
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

Per setup triangle, restricted to the rows of the band being filled:

1. The three projected vertices arrive already **sorted by y**, so edge 0→2 is
   the long one and 0→1, 1→2 are the two short ones.
2. For each row, find where the long edge crosses it and where whichever short
   edge covers that row crosses it. Those two crossings are the span.
3. Fill from the left crossing to the right, clamped to the screen.

There is **no allocation, no per-edge pixel strip, no gather and no sort**. The
previous form Bresenham-walked each of the three edges into its own `calloc`'d
strip, gathered each row's pixels with a cursor per edge, insertion-sorted them
by `px`, and filled between consecutive pairs — four heap allocations per
triangle, which at 1.96M triangles is ~8M `calloc`/`free` per frame. Worse, a
triangle clipped near the camera could project to a strip of ~250,000 `PixelCord`
(9 MB) per edge.

### Interpolation is perspective correct, and costs one divide per pixel

The three quantities carried across a span are `u/w`, `v/w` and `1/w`, where
`w = z − camera.z` is true depth in front of the eye. Those are the things that
are **linear in screen space**; `u`, `v` and `z` are not. They are stepped
incrementally along the row (one divide per row to form the step, none per
pixel), and unprojected at the sample site:

```c
w = 1.0/iw;          // the one divide per pixel
z = camera.z + w;    // exact depth
u = uw * w;          // exact texture coordinate
v = vw * w;
```

One divide buys correct depth *and* correct texture coordinates together. The old
span fill paid three divides per pixel (`interpolate()` on z, u and v) for
screen-space-linear values that were subtly wrong. The near clip guarantees
`w >= 1`, so `1/w` is finite and positive.

### The sample site holds its own bounds

```c
int64_t f_row=(int64_t)(vv*(double)th);      // SIGNED, deliberately
if(f_row<0)f_row=0; else if(f_row>=(int64_t)th)f_row=(int64_t)th-1;
```

The cast is to a signed type on purpose: a negative `v` cast straight to `size_t`
is an enormous subscript, and no clamp afterwards can undo that. The renderer now
holds this invariant itself rather than trusting every producer of an `Object`.

A texel with alpha < 128 is **skipped entirely** — not painted and not
depth-written. That is what an alpha-cutout material wants; without it a
transparent texel wins the depth test and punches a hole through the geometry
behind it. 37.7% of `dae_-_eco_house`'s atlas is fully transparent, and its trees
rendered as pale ghosts until this landed.

### Culling

- **Back face**, before the clip: `sign(N · (a − eye))`. Clipping only cuts a
  triangle up within its own plane, so every piece has the same facing as the
  whole and one test covers all of them.
- **Screen bounding box**, after projection. This replaced a test that asked
  whether any of the three *vertices* was inside the camera's frustum box — which
  is wrong for any triangle bigger than the screen, because all three of its
  corners are outside one. That silently deleted walls and floors whenever the
  camera moved inside a room.
- **Far plane**: a triangle wholly beyond `z_end` is dropped, and per pixel the
  depth is range-checked before it is stored.

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

- No matrix pipeline on the model side → no per-object transform. The view side
  exists (`transform.c`).
- No wireframe rendering. Dropped deliberately when rasterization moved to
  `rasterization.c`; `bresenhame_line_algo` is now called by nothing.
- No explicit primitive tag on `Object` — dispatch infers it from connector
  count, and nothing records indices-per-primitive.
- No texture filtering. Sampling is nearest-neighbour at the texel the truncated
  `u`/`v` lands on; no bilinear, no mipmaps, so a minified texture aliases.
- No sampler wrap modes: every texture is wrapped as REPEAT at load time
  (`issues.md` §9).
- One texture per `Object`. Multi-material glTF files are split into several
  Objects; multi-material OBJ files are **not** (`issues.md` §5).
- **Alpha is a cutout, not a blend.** A texel below alpha 128 is skipped; one
  above it is written fully opaque into the frame buffer and composited against
  the background once, in `win_upload_frame`. True alpha blending needs the
  triangles sorted back to front, which a z-buffer-only pipeline has no
  machinery for.
- No lighting, so glTF `NORMAL` and OBJ `vn` are parsed past rather than stored.
  This is why `KHR_materials_*` extensions can be ignored wholesale: there is no
  shading model for them to change.
- No skinning. Skinned glTF meshes render their bind pose (`issues.md` §7).

---

## 11. Model browser (`src/ui.c`)

An overlay drawn **onto the SDL renderer**, after the scene texture and before
the flip — never into the `PixelCord` buffer. That is the whole point: a frame
costs tens of milliseconds and a keystroke should not.

- `update_win` is split into `win_upload_frame` (flatten the grid into the
  streaming texture) and `win_present` (blit that texture, draw the overlay,
  flip). Moving the selection re-presents; only a camera change re-rasterizes.
- Text is `SDL_RenderDebugText`, SDL3's built-in 8×8 font — no SDL_ttf, no font
  file, no new dependency. The file list is `SDL_EnumerateDirectory`, sorted,
  because enumeration order is the filesystem's rather than alphabetical.
- `L` or `F1` toggles it. While open it takes **first refusal on every event**,
  so the arrow keys move the selection instead of the camera.
- `ui.c` owns the loaded `Model`, because the swap has to free the old one at
  exactly one moment relative to `set_objects()`:

  ```
  1. set_objects(fresh.objects, fresh.len)   // the renderer reads the new array
  2. mesh_model_free(&current)               // only now release the old one
  3. current = fresh                         // take ownership
  ```

  Reversing 1 and 2 is a use-after-free the moment `render()` stops joining its
  workers before returning. It does today; nothing should rely on that.
- A failed import needs no cleanup — `mesh_import` promises a zeroed `Model` on
  error — and leaves the previous model installed and on screen.
- A load blocks for seconds, so the "Loading…" notice is latched and presented
  *before* `mesh_import` is called, and the mouse queue is flushed afterwards so
  seconds of buffered motion do not fling the camera.
- `camera_reset()` runs on every successful swap. Every model is fitted into the
  same box, so a camera left somewhere else would show a blank screen and read
  as a failed load.

---

## 12. Open design questions

<!-- your notes here. -->
<!-- - should Object gain an explicit `kind` tag instead of connector-count dispatch? -->
<!-- - when the model matrix lands, does `Camera` become a matrix or keep the frustum box for culling? -->
<!-- - PixelCord is 40 bytes and the whole grid is memset every frame (60MB at 1500x1000).
       update_win reads only .in_use and .colour; the depth test reads only .z and .in_use.
       Would a split colour plane + depth plane be worth the churn? -->
<!-- - band height is 16 rows. Smaller = better load balance, more bin entries per triangle.
       Has not been swept. -->

# Renderer Issues

## Issue 1: no visibility / occlusion system

The pipeline's story on "which pixel wins" moved from an unconditional write to per-pixel arbitration. Two of the three sub-issues are cleaned up; the third is untouched.

### (a) Per-pixel depth buffer

**Status: resolved (screen-space linear).**

- Frame buffer is a `Vectex` grid. Each wireframe raster write (`renderer.c:108`) reads back `frame_buffer[py][px]` (guarded, `renderer.c:106`) and drops the incoming pixel if `point0.in_use && point0.z < point.z`.
- `z` is interpolated inside `bresenhame_line_algo` (`wireframe.c`). The four Bresenham branches each set `z` and `z_step` from the perspective of their walk-start endpoint, so the ramp always runs walk-start → walk-end regardless of which endpoint is v1.
- `Vectex.in_use` (`renderer.h:29`) is the empty/occupied sentinel. Bresenham sets it (`wireframe.c:52,98`) on every rasterized pixel. A vertex projecting to (0,0) is no longer indistinguishable from empty.

**Deferred:** perspective-correct z (`1/z` lerp). Screen-space linear is close but not exact — foreshortening biases true z toward the farther endpoint. Fine for wireframe depth arbitration; upgrade when filled-triangle textured rasterization arrives.

### (b) Frustum culling granularity

**Status: per-edge, both-endpoints-out case skipped.**

The separate object-level and per-vertex culling passes are both gone. Culling now happens per edge in the wireframe loop (`renderer.c:83`):

```c
if(!(is_vectex_visible(v1) || is_vectex_visible(v2))) continue;
```

An edge is drawn if at least one endpoint is inside the frustum. Both-out-of-view edges are dropped. Edges with one endpoint out of view still project both endpoints and rasterize the full projected line — the pixel-write guards (`renderer.c:106,111`) drop pixels that fall off-screen.

Remaining rough edges:

1. **Mixed-visibility edges over-allocate.** For a line with one endpoint that projects far off-screen, `lines_len` is proportional to the full unclipped projected span, not the visible portion. The extra `calloc` + Bresenham iterations are wasted; every pixel outside `[0, screen_width) × [0, screen_hieght)` gets rejected by the guards. See "Edge clipping" below.
2. **Objects with no in-view vertex are still walked at the edge level.** Each edge's `is_vectex_visible` check catches them, but there's no early-out at the object level. Cheap unless you have many fully-off-screen Objects.

Fix path: Cohen-Sutherland / Liang-Barsky clipping on the projected endpoints before the calloc + Bresenham call cleans up both (edge over-alloc and the "off-screen tail" waste) and gives correct partial-visibility rendering for the mixed case.

### (c) Hidden line / hidden surface removal

**Status: unchanged.**

Every edge in a closed mesh — including edges on the far side — is rasterized. Where two edges overlap in screen space, (a)'s depth compare picks a winner. Where a far edge doesn't overlap a near edge, it draws through unopposed. That's what gave the tree GLB its "spaghetti" look before the dihedral edge-filter pass.

This is deeper than (a): even with a perfect depth buffer, the pipeline rasterizes every edge first, so correctness is only enforced at contested pixels. Fix needs either upstream back-face / dihedral filtering or a per-edge occlusion test before rasterization.

---

## Issue 2: camera rotation and translation

**Status: translation done; rotation still absent.**

`CameraPos` (defined in `renderer.h`, storage now `static` in `renderer.c:5`) is used only as:
- frustum bounds (`is_vectex_visible` inside the per-edge cull), and
- projection center reference in `perspective_projection` (via `start_p`, `end_p`).

**Translation** now has an encapsulated setter — `move_camera(unit, Movement)` (`renderer.c:136`) — that shifts `.x/.x_end`, `.y/.y_end`, or `.z/.z_end` by `unit` in the requested direction. Since projection uses `centre = (start_p + end_p)/2`, shifting both bounds by the same delta moves the projection center too, so an object in the world appears to move opposite the camera. That's the expected translation behavior. `Movement` enum values are `MOV_RIGHT / MOV_LEFT / MOV_UP / MOV_DOWN / MOV_FORWARD / MOV_BACKWARD`.

**Rotation** is still not expressible. World points are never transformed into camera space — no matrix pipeline, no view matrix, no per-Object model matrix. That gap collapses two distinct concepts:

- **Object rotation** — one Object's pose changes, others unchanged.
- **Camera rotation** — the camera turns; all Objects stay put in world coords but appear to rotate opposite relative to the camera.

Neither is expressible cleanly. Rotating an Object today means mutating its `vertices` array in place — the model literally moves in the world. Adding a transform field to `Object` and a view matrix on `CameraPos` is the next natural step; both require the missing matrix pipeline.

---

## Related issues

### Near-plane clipping in `perspective_projection`

**Status: partially guarded through the edge visibility check.**

`is_vectex_visible` gates `z >= camera_position.z` (0 by default), so an edge with **both** endpoints behind the camera or beyond the far plane is dropped. But mixed-visibility edges still project their out-of-view endpoint — so an edge from an in-view vertex to a `z < 0` or tiny-`z` vertex reaches `perspective_projection`, where the pinhole formula `centre + (xy - centre) * focal / z` sign-flips through negative z and blows up as z approaches 0.

Two related risks inside `projection.c`:
- `if (z != 0)` shields against divide-by-zero but not small z. `z = 0.1` with `focal = 24` amplifies the offset 240× — anything more than a pixel off-center projects far off-screen.
- Return type is `uint64_t`. A negative computed pixel wraps silently to ~1.8×10¹⁹. The pixel-write guards in `render()` reject those OOB indices, so the crash class is contained — but see "Edge clipping" for the `lines_len` blowup that a wrapped-huge projected coord still causes.

Proper fix: near-plane clipping on the edge (not just the vertex) — i.e., where an edge crosses `z = near`, split the edge and use the intersection point as the new endpoint before projection. Simpler stopgap: reject an edge where either endpoint has `z < near`.

### Edge clipping at window borders

**Status: crash class closed, waste and one-hazard remaining.**

Both the framebuffer read and write now check bounds:
```c
Vectex point0 = point.py<screen_hieght && point.px<screen_width
              ? frame_buffer[point.py][point.px] : (Vectex){0.0};   // renderer.c:106
if (point.py<screen_hieght && point.px<screen_width)
    frame_buffer[point.py][point.px] = point;                       // renderer.c:111
```

Off-by-one is fixed (`<`, not `<=`), and the `abs()` narrowing UB is gone — the code now does a signed subtract + explicit sign-flip (`renderer.c:92-96`).

What's left:

1. **Lines aren't clipped, only individual pixel writes are.** Bresenham still iterates the whole projected line even when large parts are OOB, and `calloc(lines_len, sizeof(Vectex))` sizes to the full unclipped span. For an edge whose out-of-view endpoint projects to a wrapped-huge `px`, the caller's `ch_x = v1.px - v2.px` may fit (unsigned wrap → small signed number → small `lines_len` → normal-size calloc), but `bresenhame_line_algo` recomputes `ch_x` from the raw `px` values and iterates from `x_start+1` to `x_end-1` — potentially billions of iterations writing into a `lines_arr` sized for far fewer. **This is a latent OOB heap write.** Only unhit today because none of the test scenes wrap the projection.

2. **Wasted work on the off-screen tail.** Even without the wrap case, an in-view/out-of-view edge rasterizes the entire projected span; the guard just drops each OOB pixel. On a heavy scene that's real overhead.

Both close by clipping the projected endpoints (Cohen-Sutherland or Liang-Barsky) against the `[0, screen_width) × [0, screen_hieght)` rectangle before the `calloc` and Bresenham call. That also aligns the caller's `ch_x` with what Bresenham will iterate over.

### `calc_screen_cordinate` compounds on repeated calls

**Status: dormant behind one remaining shield.**

`calc_screen_cordinate` (`renderer.c:28-39`) still treats `camera_position.x_end` / `.y_end` as accumulators — first call sets them correctly *only if* they start at `0`, second call drifts them by `2*win_size`, etc. The function itself is unchanged.

What used to be a two-part shield is now down to one:

1. `render_init` is idempotent (`is_init_called` guard, `renderer.c:53`), so `calc_screen_cordinate` fires at most once per program run.
2. ~~`camera_position` was extern-mutable and could be tainted before init.~~ Closed: `camera_position` is now `static` (`renderer.c:5`) and removed from the header. Only `renderer.c` code (`render_init`, `move_camera`, `is_vectex_visible`, the projection callers) can touch it.

The remaining hole: if `render_init` ever needs to accept a new scene / camera / window size (animation, resize), the `is_init_called` guard has to be released, and the second call to `calc_screen_cordinate` drifts the bounds. Note also that `move_camera` now shifts `.x` and `.x_end` in lockstep for every direction (`renderer.c:139-163`), so post-init camera moves don't desynchronize the pair — that specific failure mode is closed.

Fix: recompute `.x_end` / `.y_end` from `.x + win_size` each call (no accumulator). Then the guard becomes cosmetic rather than load-bearing.

### `Object.vertices` is mutated by `render()`

**Status: resolved.**

The current wireframe loop copies each connector's endpoints into local `Vectex v1, v2` values (`renderer.c:80-81`) and writes `.px, .py` back onto those locals only. The caller's `Object.vertices` array is never mutated. Multi-frame rendering can share vertex arrays freely.

### Wireframe colour is bitwise-OR of endpoints

`renderer.c:110` sets a line pixel's colour to `v1.colour | v2.colour`. Bitwise OR isn't linear blending — `0xFF0000FF | 0xFFFF0000 = 0xFFFF00FF` (blue | red = magenta) is a specific mix, not "halfway between". Current tests use single-colour edges so it looks right. Per-pixel colour lerping along an edge means unpacking ARGB channels, lerping each with the same `t` used for z, and repacking.

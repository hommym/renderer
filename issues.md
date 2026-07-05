# Renderer Issues

Open problems in the renderer. Each entry: what breaks, why it breaks, fix direction. Resolved items removed; historical narrative removed.

---

## 1. Hidden-surface removal for wireframes

**What breaks.** A closed mesh looks like spaghetti instead of an outline. Edges on the *back* side of the mesh draw right through the front, everywhere they don't overlap a front edge.

**Why.** The wireframe path rasterizes every edge in `Object.connectors_sequence` unconditionally. Where two edges overlap in screen space, the per-pixel depth compare picks the front — that's correct. Where a back edge doesn't overlap anything, it draws unopposed, because the pipeline never asks "is this edge even visible from the camera?"

**Fix path.** Filter out non-visible edges *before* rasterization. Two approaches:
- Upstream: back-face / dihedral filter at mesh load time. That's what the GLB import script (`tools/glb_to_header.py`) is doing at 80° crease — it drops edges shared by two nearly-coplanar triangles.
- In-pipeline: per-edge occlusion test against the current depth buffer before rasterizing.

---

## 2. Camera rotation

**What breaks.** There is no way to rotate the camera. `move_camera(unit, Movement)` handles translation only.

**Why.** World points are consumed by the projection directly. There is no view matrix, no per-Object model matrix, no matrix pipeline at all. Rotating an Object today means mutating its `vertices` array in place — which literally moves the model in the world, so you can't tell "Object rotated" apart from "camera rotated the other way relative to a stationary Object."

**Fix path.** Add a 4×4 transform matrix to `Object` (the model matrix) and one to `CameraPos` (the view matrix). Multiply each vertex by `view * model` before projection.

---

## 3. Near-plane blowup on mixed-visibility edges

**What breaks.** An edge with one endpoint in front of the camera and one endpoint behind (or at very small z) projects the bad endpoint to a wildly wrong pixel. At `focal_len=600` (current), a tiny-z endpoint can project hundreds of screens off. If z is negative, the offset sign-flips and the endpoint reflects to the wrong side of the screen entirely.

**Why.**
```c
offset = (xy - centre) * focal / z;   // projection.c
```
- `z < 0` flips the sign of the offset.
- Tiny `z > 0` amplifies the offset by `focal/z`. At `focal=600`, `z=1` amplifies 600×.
- `perspective_projection` returns `uint64_t`, so a negative computed pixel wraps silently to ~1.8·10¹⁹, which then feeds directly into `calloc(lines_len, ...)` — see issue 4.

`is_vectex_visible` only catches the edge when *both* endpoints are out of the frustum. Mixed-visibility edges get through with the bad endpoint intact.

**Fix path.** Clip the edge against a finite near plane *before* projecting. Where an edge crosses `z = near`, split it and use the intersection as the new endpoint. Cheaper stopgap: reject the whole edge if either endpoint has `z < near`.

---

## 4. Lines aren't clipped against the screen rectangle

**What breaks.** Two symptoms:

- **Wasted work.** An edge from an in-view vertex to an off-screen vertex allocates a Bresenham buffer sized for the *full* projected span, walks the whole line, and computes every OOB pixel before the write-side guard drops it. On a scene with many partially-visible edges (a large mesh at the frustum boundary), this stacks up into real time.
- **Latent OOB heap write.** When issue 3 wraps a projection through `uint64_t`, the caller's `ch_x = v1.px - v2.px` (signed subtract + explicit sign-flip) may collapse back to a small number — so `calloc(lines_len, ...)` allocates a small buffer. But `bresenhame_line_algo` recomputes `ch_x` from the raw `px` values and iterates from `x_start+1` to `x_end-1` — potentially billions of writes, well past the calloc'd region. Unhit today because no test scene wraps the projection, but any camera move that pushes a vertex through the near plane hits it.

The write-side pixel guards (`renderer.c:122, 127`) protect against *writing* OOB, not against the compute or allocation leading up to it.

**Fix path.** Cohen-Sutherland or Liang-Barsky line clipping on the projected endpoints, *before* the `calloc` and Bresenham call. Same clip covers both symptoms — the caller and Bresenham then agree on the visible line length.

---

## 5. `calc_screen_cordinate` accumulator

**What breaks (potentially).** If `render_init` is ever called more than once — for a resize, a new scene, or a reset — `camera_position.x_end / .y_end` drift outward by `2 * win_size` on the second call and worse on each subsequent call. Every frustum-bounds check and every projection center that reads those values goes wrong.

**Why.** `calc_screen_cordinate` (`renderer.c:28-39`) treats `.x_end` / `.y_end` as accumulators, not outputs. First call, when the value is `0.0`, gives the right answer. Second call, it sees the previous result and drifts.

Not hit today because `render_init` is idempotent (`is_init_called` guard at `renderer.c:53`), so the function only fires once per program run. The guard is load-bearing: the moment it's released, the accumulator bug is live.

**Fix path.** Recompute `.x_end / .y_end` from `.x + win_size` each call — no accumulator. Then the `is_init_called` guard becomes cosmetic instead of load-bearing.

---

## 6. Wireframe colour is bitwise-OR, and `Object.colour` isn't a real fallback

**What breaks.** Two things:

- **The blend is wrong.** Along a wireframe edge, every rasterized pixel takes `v1.colour | v2.colour`. For same-colour endpoints this looks right. For different-colour endpoints, `red | blue = magenta` is a specific bit-flip, not a lerp — the mix looks nothing like halfway between the two.
- **The Object default is inconsistent.** The point-cloud path honours `Object.colour` as a per-object fallback (`if(pt.colour == 0) pt.colour = obj.colour`), but the wireframe path does not. A wireframe object with `.colour = 0xFFFFFFFF` set on the Object and vertices at `.colour = 0` draws as invisible black.

**Fix path.** Along each Bresenham step, use the same `t` you already interpolate `z` with, unpack ARGB channels, lerp each, repack. Fold the `obj.colour` fallback into `v1`/`v2` colour *before* the lerp, so both primitive paths behave the same way.

---

## 7. Deferred: perspective-correct z

**What breaks (later).** Depth arbitration uses screen-space linear interpolation of `z` between edge endpoints. Under perspective projection this is close but slightly wrong — foreshortening biases the true `z` toward the farther endpoint. Invisible at wireframe fidelity today. Matters as soon as filled/textured triangles land.

**Fix path when it matters.** Interpolate `1/z` linearly in screen space, invert per pixel. Same machinery gives you perspective-correct texture coordinates.

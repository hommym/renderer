# Renderer Issues

Open problems in the renderer. Each entry: what breaks, why it breaks, fix direction. Resolved items removed; historical narrative removed.

---

## 1. Hidden-surface removal for wireframes

**What breaks.** A closed mesh looks like spaghetti instead of an outline. Edges on the _back_ side of the mesh draw right through the front, everywhere they don't overlap a front edge.

**Why.** The wireframe path rasterizes every edge in `Object.connectors_sequence` unconditionally. Where two edges overlap in screen space, the per-pixel depth compare picks the front — that's correct. Where a back edge doesn't overlap anything, it draws unopposed, because the pipeline never asks "is this edge even visible from the camera?"

**Fix path.** Filter out non-visible edges _before_ rasterization. Two approaches:

- Upstream: back-face / dihedral filter at mesh load time. That's what the GLB import script (`tools/glb_to_header.py`) is doing at 80° crease — it drops edges shared by two nearly-coplanar triangles.
- In-pipeline: per-edge occlusion test against the current depth buffer before rasterizing.

---

## 2. Camera rotation

**What breaks.** There is no way to rotate the camera. `move_camera(unit, Movement)` handles translation only.

**Why.** World points are consumed by the projection directly. There is no view matrix, no per-Object model matrix, no matrix pipeline at all. Rotating an Object today means mutating its `vertices` array in place — which literally moves the model in the world, so you can't tell "Object rotated" apart from "camera rotated the other way relative to a stationary Object."

**Fix path.** Add a 4×4 transform matrix to `Object` (the model matrix) and one to `Camera` (the view matrix). Multiply each vertex by `view * model` before projection.

---

## 3. The wireframe path still has no near-plane clip

**What breaks.** An edge with one endpoint deep in the scene and one endpoint close to the near plane (`z ≈ camera.z`) draws the correct pixels — the per-pixel visibility gate in `bresenhame_line_algo` drops off-screen steps — but the loop bound and the `lines_arr` allocation are sized off the _true_ projected distance, which explodes as `z - camera.z → 0`. `calloc` can request hundreds of MB, or return `NULL` and get written through.

**Why.** The raster path now clips against the near plane before projecting (`clip_triangle_near`), but the wireframe branch of `render()` does not. It accepts an edge when _either_ endpoint is visible, then projects both. `perspective_projection` clamps the divisor to 1.0, which bounds the divide but not the result: a vertex one unit in front of the camera at x = -100 still projects ~86,000 pixels off screen.

**Fix path.** Clip the edge against `z = camera.z + NEAR_PLANE_MARGIN` before projecting, using the intersection as the new endpoint — the same treatment `clip_triangle_near` gives triangles. A shared `clip_segment_near` helper would serve both paths.

---

## 4. Frame-buffer indexing is unguarded in the point-cloud path

**What breaks.** The point-cloud write sites in `render()` index `frame_buffer[py][px]` with no bounds check, in both the wireframe and raster branches.

**Why.** The invariant is "pass `is_vectex_visible` with strict `<` bounds ⇒ projected pixel ∈ [0, screen_s)". That holds because the offset computed in `is_vectex_visible` is the algebraic inverse of the projection — but the contract is spread across `renderer.c` and `projection.c` and neither asserts it. If the projection formula changes without a matching update, silent out-of-bounds heap writes come back.

The triangle and line paths are safe by construction: Bresenham re-gates every pixel, and the span fill clamps to the screen before writing.

**Fix path.** Either bounds-check the point-cloud writes, or make the contract explicit — an `assert` at the end of `perspective_projection`, plus an upstream NaN guard (all comparisons against NaN are false, so a NaN vertex is culled by luck rather than by design).

---

## 5. `Object.colour` is dead

**What breaks.** Nothing reads `obj.colour`. A vertex with `.colour == 0` draws as transparent black regardless of what the Object sets, so the per-object fallback documented in `ARCHITECTURE.md` does not exist.

**Fix path.** Fold the fallback into the vertex colour at the top of each primitive path, before any interpolation, so all three paths behave the same way.

---

## 6. Depth and colour interpolation is not perspective-correct

**What breaks.** Depth arbitration and the per-pixel colour lerp both interpolate linearly in _screen_ space. Under perspective projection that is close but wrong — foreshortening biases the true value toward the farther endpoint. Visible today as a faint seam along the diagonal where the two triangles of a quad meet, on any face with a strong colour gradient.

**Fix path.** Interpolate `1/z` linearly in screen space and invert per pixel; interpolate `colour/z` the same way. The same machinery gives perspective-correct texture coordinates later.

---

## 7. No back-face culling

**What breaks.** Every triangle of a closed mesh is rasterized, including the roughly half that face away from the camera. They are then discarded by the depth compare, so the output is correct but up to twice the fill work is wasted.

**Why.** There is no winding-order test. `clip_triangle_near` also does not preserve winding when it splits a triangle, so a culling test would need to establish orientation from the projected vertices rather than trusting index order.

**Fix path.** After projecting the three vertices, take the sign of the 2D cross product of two edges and skip triangles facing away. Decide a winding convention first, and make the clip respect it.

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

## 3. Wireframe edges crossing the near plane cost unbounded time and memory

**What breaks.** An edge with one endpoint deep in the scene and one endpoint close to the near plane (`z ≈ camera.z`) still rasterizes to the correct pixels — the per-pixel visibility gate in `bresenhame_line_algo` drops off-screen steps. But the loop bound and the `lines_arr` allocation are sized off the _true_ projected distance between the two endpoints, which explodes as `z - camera.z → 0`:

- `int64_t ch_x = p1.px - p2.px` at `renderer.c:138` can be millions or billions of pixels when one endpoint's projection amplifies through `focal / (z - camera.z)`.
- `calloc(lines_len, sizeof(PixelCord))` at `renderer.c:146` can request hundreds of MB, or fail and return `NULL` — then `bresenhame_line_algo` writes to `NULL` → segfault.
- Bresenham steps `x` in `double`; past ~2⁵³ the `x++` stops making progress and the loop hangs.

**Why.** `is_vectex_visible` correctly culls vertices outside the screen-mapped frustum, but the wireframe path accepts an edge if _either_ endpoint is visible (`renderer.c:129`). Both endpoints then get projected and handed to Bresenham. The projection at `projection.c:15-17` guards only `(z - camera.z) == 0` exactly — for `z - camera.z = ε` the offset amplifies by `focal / ε`.

**Fix path.** Clip the edge against the near plane _before_ projecting. Where an edge crosses `z = camera.z + near_epsilon`, split it and use the intersection as the new endpoint. That keeps both projected endpoints within a bounded distance of the screen, so `lines_len` stays sensible.

---

## 4. Frame-buffer indexing is unguarded, safety leans on the visibility check

**What breaks.** Both write sites in `render()` — the point-cloud path at `renderer.c:116` and the wireframe path at `renderer.c:158` — index into `frame_buffer[py][px]` with no bounds check. Reads at lines 113 and 153 are also unguarded.

**Why.** The invariant is now "pass `is_vectex_visible` with strict `<` bounds ⇒ projected pixel ∈ (0, screen_s)". That holds because `(screen/2) * (z - camera.z) / focal_l` in `is_vectex_visible` is the algebraic inverse of the projection. But:

- The contract is spread across two files — `renderer.c` and `projection.c` — and neither asserts it. If the projection formula changes without a matching update to `is_vectex_visible`, silent OOB heap writes come back — the same class of bug that produced the recent "double free or corruption" abort when `is_vectex_visible` used a wider frustum than the projection.
- The wireframe path only requires _one_ endpoint visible, then projects both. The unchecked-projected other endpoint is what Bresenham walks toward. Per-pixel `is_visible` in Bresenham re-gates before writing to `frame_buffer`, so the _writes_ stay in bounds — but see issue #3 for the runaway cost.
- A NaN sneaking in still bypasses the visibility check quietly (all comparisons against NaN are false, so the vertex is culled — currently safe by luck). The known NaN path via `focal_l == 0` on a zero-height window is closed now that `focal_l` is a compile-time constant.

**Fix path.** Either add write-side bounds checks back (belt-and-suspenders), or make the projection function's contract explicit — e.g. `assert(pix >= 0 && pix < screen_s)` at the end of `perspective_projection` and handle NaN upstream.

---

## 5. Wireframe colour is bitwise-OR, and `Object.colour` isn't a fallback anywhere

**What breaks.** Two things:

- **The blend is wrong.** Along a wireframe edge, every rasterized pixel takes `v1.colour | v2.colour`. For same-colour endpoints this looks right. For different-colour endpoints, `red | blue = magenta` is a specific bit-flip, not a lerp — the mix looks nothing like halfway between the two.
- **`Object.colour` is dead.** Neither the point-cloud path (`renderer.c:106-119`) nor the wireframe path (`renderer.c:121-161`) reads `obj.colour`. A vertex with `.colour = 0` draws as transparent black regardless of what the Object sets.

**Fix path.** Along each Bresenham step, use the same `t` you already interpolate `z` with, unpack ARGB channels, lerp each, repack. Fold the `obj.colour` fallback into `v1`/`v2` colour _before_ the lerp, so both primitive paths behave the same way.

---

## 6. Deferred: perspective-correct z

**What breaks (later).** Depth arbitration uses screen-space linear interpolation of `z` between edge endpoints. Under perspective projection this is close but slightly wrong — foreshortening biases the true `z` toward the farther endpoint. Invisible at wireframe fidelity today. Matters as soon as filled/textured triangles land.

**Fix path when it matters.** Interpolate `1/z` linearly in screen space, invert per pixel. Same machinery gives you perspective-correct texture coordinates.

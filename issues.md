# Renderer Issues

## Issue 1: no visibility / occlusion system

The renderer paints every vertex and every edge in every Object passed to `render()`, regardless of whether the geometry should be visible from the camera's viewpoint. This breaks into three distinct layers.

### (a) No depth buffer when writing pixels

The two pixel-writing sites in `renderer.c` are unconditional with respect to depth:

```c
// renderer.c:30  (vertex pixels)
if(px<win_w && py<win_h) frame_buffer[py][px] = objects[i].colour;

// renderer.c:56  (wireframe pixels)
frame_buffer[lines_arr[i+1]][lines_arr[i]] = obj.colour;
```

When two edges (or vertex pixels) project to the same screen pixel, the second write wins regardless of which point is actually closer to the camera in 3D. The pixel's owner is determined by the order in which Objects, vertices, and edges happen to be iterated — not by their depth. As soon as a model has any front/back overlap in screen space, which edge owns a contested pixel becomes essentially arbitrary.

Adding a depth buffer is not just an allocation. To make the depth test usable for the wireframe path, the rasterizer itself has to be extended:

- `Vectex` (or the projection output) must retain a camera-space `z` per vertex. Today `perspective_projection` consumes `z` and only writes back `(px, py)`; the `z` value at the line endpoints is then unavailable downstream.
- `bresenhame_line_algo` must interpolate `z` along the line, the same way it currently interpolates the minor axis (`y` in the x-major branch, `x` in the y-major branch). One extra accumulator stepping `(z2 - z1) / steps` per iteration.
- The pixel write becomes a 3-step compare-write-update against `depth_buffer[py][px]`: read the stored depth, compare with the new pixel's interpolated `z`, write the color and the new `z` only if it's closer.

The same per-pixel `z` machinery is the foundation that filled-triangle rasterization will later need — so the work here is not throw-away.

### (b) Wasted projection work on non-visible vertices

The projection loop (`renderer.c:25-32`) runs `perspective_projection` for every vertex of every Object, with no test for whether the vertex is:

- behind the camera (`z <= 0`),
- beyond some far cutoff,
- so far to the side of the camera that its screen pixel would land outside the window.

For tiny scenes this is irrelevant. For the LAS cloud at ~500k vertices and the GLB tree at ~510k vertices, a meaningful fraction of the per-frame work is spent computing pixel coordinates for points we already could have known would never be displayed.

### (c) Hidden line / hidden surface removal for the wireframe

Edges on the *far* side of a closed mesh are drawn the same way as edges on the *near* side. Where two edges overlap in screen space, sub-issue (a) arbitrates which one is visible by iteration order. Where a far edge does NOT overlap a near edge, the far edge simply shows through, and the model looks like spaghetti instead of an outline. This is what produced the dense, "filled" appearance of the gnarled-tree GLB model before the dihedral edge-filtering pass — many of the kept edges were structurally on the back side of the trunk and canopy and should never have been visible from the camera.

Conceptually this is related to (a) but stronger: even if a depth buffer fixed per-pixel correctness, the wireframe path still rasterizes every edge first. Correctness applies only at contested pixels; sparse far-side edges still leak through everywhere they don't overlap a near edge.

---

## Issue 2: camera rotation vs object rotation

There is no camera abstraction in the renderer. `perspective_projection` is called with raw world coordinates:

```c
// renderer.c:28-29
objects[i].vertices[a].px = perspective_projection(
    objects[i].vertices[a].x, objects[i].vertices[a].z,
    focal_len, true, 0, win_w);
```

The implicit camera is fixed at `(0, 0, 0)`, facing `+z`, with `+y` up. Nothing in the data structures or function signatures represents the camera's position or orientation; there is no mechanism to move it.

"Rotating the object" currently means mutating that Object's `vertices` array in place — the model literally moves in the world. This conflates two distinct concepts:

- **Object rotation**: a single Object's pose in the world changes. Different Objects can be rotated independently. Camera unchanged.
- **Camera rotation**: the camera turns. All Objects stay where they are in world coordinates, but they appear to rotate the opposite way relative to the camera.

The renderer cannot express the second concept at all. There is no view transform between a vertex's world coordinates and the projection step. To make the camera rotate or move, every world point would need to be transformed into camera space first; right now there is nowhere for that transform to live.

---

## Related issues

### Near-plane clipping in `perspective_projection`

The projection function (`projection.c:7`) only guards against exact zero in z:

```c
uint64_t perspective_projection(double xy, double z, int32_t focal, bool is_x, uint32_t win_h, uint32_t win_w){
    if(z != 0){
        xy = (xy * focal) / z;
    }
    ...
}
```

A vertex at `z = 0.1` produces a projected coordinate of `xy * focal * 10` — an order-of-magnitude blowup. A vertex with negative `z` (behind the camera) flips the projection's sign and lands on the opposite side of the screen, with no flag that anything went wrong. There is no "near plane" cutoff; the GLB and LAS loaders had to be tuned to keep every vertex at `z > ~200` because anything closer produced visual chaos or out-of-bounds writes.

### Edge clipping at window borders

The vertex pixel write (`renderer.c:30`) has a per-pixel bounds check; the wireframe pixel write (`renderer.c:56`) does not. An edge whose endpoints are both inside the window is safe, but an edge with one endpoint inside and one outside is not: Bresenham walks from the inside endpoint toward the outside one and writes into out-of-bounds memory along the way. The line itself is never clipped against the window rectangle before rasterization.


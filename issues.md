# Renderer Issues

## Issue 1: no visibility / occlusion system

The renderer paints every vertex and every edge in every Object passed to `render()`, regardless of whether the geometry should be visible from the camera's viewpoint. This breaks into three distinct layers.

### (a) No depth buffer when writing pixels

**Status: partially resolved (vertex-level).**

`render()` now maintains a `v_track[py][px]` array of `Vectex*` pointers. At projection time, if a second vertex projects to a pixel already claimed by another vertex, the two are compared by `z` and the farther one has its `(px, py)` zeroed, so the wireframe culled-vertex skip drops every edge originating from it. This handles the *vertex-level* contest at any pixel.

The **Bresenham pixel write** is still unconditional:

```c
// renderer.c (wireframe loop)
frame_buffer[lines_arr[i+1]][lines_arr[i]] = v1.colour | v2.colour;
```

When two edges cross in screen space and neither endpoint is the contested pixel, the second write still wins regardless of depth. Making the depth test usable along the line requires the rasterizer itself to be extended:

- `Vectex` already retains `z`, but `bresenhame_line_algo` currently sees only `(px, py)` at the endpoints. The endpoints' `z` values need to reach it.
- The Bresenham loop must interpolate `z` along the line the same way it interpolates the minor axis (`y` in the x-major branch, `x` in the y-major branch). One extra accumulator stepping `(z2 - z1) / steps` per iteration.
- The pixel write becomes a 3-step compare-write-update against `depth_buffer[py][px]`: read the stored depth, compare with the new pixel's interpolated `z`, write the color and the new `z` only if it's closer.

The same per-pixel `z` machinery is the foundation that filled-triangle rasterization will later need — so the work here is not throw-away.

### (b) Wasted projection work on non-visible vertices

**Status: resolved.**

The projection loop now performs an in-view check before calling `perspective_projection`:

```c
bool is_x_in_view = point.x >= camera_position.x && point.x <= camera_position.x_end;
bool is_y_in_view = point.y >= camera_position.y && point.y <= camera_position.y_end;
bool is_z_in_view = point.z >= camera_position.z && point.z <= camera_position.z_end;
if(!(is_x_in_view && is_y_in_view && is_z_in_view)) continue;
```

Vertices behind the camera (`z < camera_position.z`), beyond the far plane (`z > camera_position.z_end`), or outside the lateral frustum walls skip the projection step entirely.

### (c) Hidden line / hidden surface removal for the wireframe

Edges on the *far* side of a closed mesh are drawn the same way as edges on the *near* side. Where two edges overlap in screen space, sub-issue (a) arbitrates which one is visible (now per-vertex; later per-pixel). Where a far edge does NOT overlap a near edge, the far edge simply shows through, and the model looks like spaghetti instead of an outline. This is what produced the dense, "filled" appearance of the gnarled-tree GLB model before the dihedral edge-filtering pass — many of the kept edges were structurally on the back side of the trunk and canopy and should never have been visible from the camera.

Conceptually this is related to (a) but stronger: even when the depth buffer fixes per-pixel correctness, the wireframe path still rasterizes every edge first. Correctness applies only at contested pixels; sparse far-side edges still leak through everywhere they don't overlap a near edge.

---

## Issue 2: camera rotation vs object rotation

A `CameraPos` struct exists (`renderer.h`) and `perspective_projection` now centers its output around `start_p`/`end_p` derived from `camera_position`. Shifting `camera_position.x`/`x_end` therefore shifts what counts as "screen center" along that axis. But this only changes the *projection center*; world points are still consumed by the projection as-is. Specifically:

- World coordinates are never multiplied through a camera rotation matrix.
- World coordinates are never offset by a camera position vector before projection — `camera_position` is used as frustum culling bounds and as the projection center reference, not as the camera's world location subtracted from each vertex.

"Rotating the object" currently means mutating that Object's `vertices` array in place — the model literally moves in the world. This conflates two distinct concepts:

- **Object rotation**: a single Object's pose in the world changes. Different Objects can be rotated independently. Camera unchanged.
- **Camera rotation**: the camera turns. All Objects stay where they are in world coordinates, but they appear to rotate the opposite way relative to the camera.

The renderer cannot express the second concept at all. To make the camera rotate or translate, every world point would need to be transformed into camera space first; there is still nowhere for that transform to live.

---

## Related issues

### Near-plane clipping in `perspective_projection`

**Status: partially resolved.** Vertices with `z < camera_position.z` (default `0`) are now culled before reaching the projection, so negative-z (behind-camera) vertices no longer sign-flip the output. However, a vertex at `z = 0.1` still produces a projected offset of `(xy - centre) * focal * 10` — an order-of-magnitude blowup — because the in-view floor is `0`, not a finite near plane. The GLB and LAS loaders still have to keep every vertex at `z` well above zero to avoid visual chaos.

### Edge clipping at window borders

The wireframe pixel write does not bounds-check `px`/`py` against the framebuffer dimensions. In the current setup this is partially shielded by two things: the in-view check rejects vertices outside the frustum, and the new projection — where world `[start_p, end_p]` maps to screen `[0, win]` at `z = focal_len` — keeps in-view vertices on-screen for `z >= focal_len`. But for `z < focal_len` the projection magnifies points outside the visible window, and writes to `frame_buffer[py][px]` past `win_w` quietly wrap into the next row (or, with large enough overflow, out of the allocated buffer entirely). The line itself is still never clipped against the window rectangle before rasterization.

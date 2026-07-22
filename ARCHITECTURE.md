# Architecture

Design notes for the renderer. Companion to `issues.md` — that file tracks what's broken; this one tracks what exists and why.

---

## 1. Pipeline

```
Object[] ─▶ render() ─▶ PixelCord grid (`frame`)
```

## 1. API Interface

- `render_init` creates the fame buffer, sets up the screen size and the camera's coordinates and lense info.
- `render(wireframe_mode)` walks every `Object`, projects, rasterizes or create wireframes (this depends on the value of the wireframe_mode argument), writes cells and return tue when whole process is successful else false.
-`clear_frame_buffer(keep_frame)` clears your frame buffer and creates a new base on the current screen dimensions. if keep_frame is true the frame buffer is not removed from memory and still exist can be reference and used.
-`get_frame_buffer()` gets a pointer to current framebuffer been used for rendering
-`renderer_resize(win_w,win_h)` resizes the framebuffer been used and camera's coordinate data and lense info
- `move_camera(unit, direction)` translates the camera by `unit` world-units along one axis. Both the near value and the matching far-plane extent (`x`/`x_end`, `y`/`y_end`, `z`/`z_end`) shift by the same amount, so the view volume slides rigidly without changing shape. `direction` picks the axis and sign: `MOV_LEFT`/`MOV_RIGHT` on x, `MOV_UP`/`MOV_DOWN` on y, `MOV_FORWARD`/`MOV_BACKWARD` on z. Does *not* clear the frame buffer — the caller is responsible for calling `clear_frame_buffer(false)` before the next `render()` if a fresh grid is needed.
- `get_camera_pos()` returns the current `Camera` by value: position (`x`,`y`,`z`), far-plane extents (`x_end`,`y_end`,`z_end`), and lens fields (`focal_l`, `v_fov`, `h_fov`). It's a snapshot copy — mutating the returned struct does not affect the renderer's camera.

---

## 2. Core types

**`Vectex`** — is used to represent the world space data or coordinate(x,y,z) and colour (32 bit colour)


**`PixelCord`** — is used to represent  data or coordinates (x and y represented as px and py respectively) of the screen and the depth(ie z from world space) and the colour of the vectex from world space 

| Role                           |
| ------------------------------ | 
| Stores Projected pixel 
| Frame-buffer cell (output)     
`.in_use` is used to determine which parts of frame buffer has a projected point from world space



**`Object`** — a drawable. `vertices[]` + `connectors_sequence[]` (flat index-pair edge list). `.colour` is the per-object fallback. `len_of_connectors == 0` means "point cloud" — no edges, one pixel per vertex.

**`Camera`** — axis-aligned frustum box: `(x, x_end)`, `(y, y_end)`, `(z, z_end)`. No rotation yet.

---

## 3. Coordinate system

- World units == pixel units. No world→NDC step.
- `+Y is down` on screen (importers must flip glTF's +Y-up).
- `focal_len` is a plain scalar (currently 600). Perspective divide is `(xy - centre) * focal / z`.
- No near-plane clip today — see `issues.md` §3.

---

## 4. Dispatch inside `render()`

```
for obj in objects:
    if obj.len_of_connectors == 0: point-cloud path (one pixel per vertex)
    else:                          wireframe path (Bresenham per edge)
```

No enum tag. Adding one is a candidate cleanup when a third primitive type shows up.

---

## 5. Frame buffer ownership

- Allocated in `create_frame_buffer(w, h)` via `calloc`; stored as `void* frame`.
- Reinterpreted as `Vectex (*)[screen_width]` inside `render()`; as flat `Vectex*` inside `update_win()`.
- **Not** cleared per frame automatically. Callers that need a fresh frame call `clear_frame_buffer()` (currently: `move_camera`, `renderer_resize`).
- `Object.vertices` and `Object.connectors_sequence` are owned by the caller of `render_init` — the renderer never frees them.



## 8. Deliberate omissions (see `issues.md`)

- No matrix pipeline → no camera rotation, no per-object transform.
- No line clipping → mixed-visibility edges can wrap `uint64_t`.
- No z-buffer beyond the `.in_use` sentinel.
- No filled-triangle rasterizer — `render(false)` branch is empty.
- Wireframe colour is `v1.colour | v2.colour` (bitwise, not a lerp).

---

## 9. Open design questions

<!-- your notes here. examples of the kind of thing worth writing down: -->
<!-- - should Object gain an explicit `kind` tag instead of connector-count dispatch? -->
<!-- - when the view matrix lands, does `Camera` become a matrix or keep the frustum box for culling? -->
<!-- - point-cloud colour honours `Object.colour`; wireframe doesn't. Unify or intentional? -->

# Architecture

Design notes for the renderer. Companion to `issues.md` — that file tracks what's broken; this one tracks what exists and why.

---

## 1. Pipeline

```
Object[] ─▶ render() ─▶ Vectex grid (`frame`) ─▶ update_win() ─▶ SDL texture ─▶ present
```

- `render_init` allocates the grid once at startup.
- `render(wireframe_mode)` walks every `Object`, projects, rasterizes, writes cells.
- `update_win` flattens the grid into ARGB and hands it to SDL every event tick.
- Camera motion / window resize call `clear_frame_buffer()` to wipe between frames.

---

## 2. Core types

**`Vectex`** — universal cell. Same struct plays three roles:

| Role | Fields that matter |
|---|---|
| World-space vertex (input) | `x`, `y`, `z`, `colour` |
| Projected pixel (mid-pipeline) | `px`, `py`, `z`, `colour` |
| Frame-buffer cell (output) | `px`, `py`, `z`, `colour`, `in_use` |

`.in_use` is the depth-buffer sentinel: `false` = empty cell (paint background), `true` = occupied (compare `.z` before overwriting).

**`Object`** — a drawable. `vertices[]` + `connectors_sequence[]` (flat index-pair edge list). `.colour` is the per-object fallback. `len_of_connectors == 0` means "point cloud" — no edges, one pixel per vertex.

**`CameraPos`** — axis-aligned frustum box: `(x, x_end)`, `(y, y_end)`, `(z, z_end)`. No rotation yet.

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

---

## 6. Windowing / input (win_i_o.c)

Single-threaded event loop. Every tick ends with `update_win(frame)`. Handlers:

| Event | Action |
|---|---|
| `WINDOW_CLOSE_REQUESTED` | destroy + quit |
| `WINDOW_RESIZED / MAXIMIZED / RESTORED` | re-query pixel size, `renderer_resize`, `render(true)` |
| `MOUSE_WHEEL` | `move_camera(10, MOV_FORWARD/BACKWARD)` |
| Arrow keys (non-repeat) | `move_camera(30, MOV_LEFT/RIGHT/UP/DOWN)` |

MAXIMIZED and RESTORED are separate SDL events and don't carry new dims — must re-query with `SDL_GetWindowSizeInPixels`.

---

## 7. Test data pipeline

`tools/` (gitignored) contains two Python importers, stdlib only:

- `glb_to_header.py` — binary glTF → wireframe. Filters edges by dihedral angle (keeps sharp creases + boundary).
- `las_to_header.py` — LAS 1.2 LiDAR → point cloud. Subsamples, swaps axes.

Both emit into `include/` (also gitignored). Hardcoded frustum centers place the GLB tree at world x≈450 and the point cloud at x≈1050 so both fit side-by-side in a 1500×1000 window.

---

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
<!-- - when the view matrix lands, does `CameraPos` become a matrix or keep the frustum box for culling? -->
<!-- - point-cloud colour honours `Object.colour`; wireframe doesn't. Unify or intentional? -->

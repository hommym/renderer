# Renderer Issues

Open problems in the renderer. Each entry: what breaks, why it breaks, fix direction. Resolved items removed; historical narrative removed.

---

## 2. `rasterization_worker` falls off the end without returning

**What breaks.** Undefined behaviour on every worker exit.

**Why.** `rasterization.c:391` declares `void* rasterization_worker(void* args)` and the function body ends after the `while` loop with no `return`. Reaching the closing brace of a non-`void` function and having the caller use the value is UB. `pthread_join` is passed `NULL` for the result today, so nothing reads it and it does not misbehave in practice.

**Fix path.** `return NULL;` at the end.

---

## 3. `Object` cannot distinguish a triangle list from an edge list

**What breaks.** `render()` sends every object's `connectors_sequence` to the triangle rasterizer, which reads it three at a time. Nothing records how many indices per primitive the array actually holds.

**Why.** `model_data.h` ships two index arrays over the same vertices: `model_connectors` (pairs, 392,102 entries) and `model_tri_connectors` (triples, 2,534,133 entries). They differ only by which field an `Object` literal points at. 392,102 is not divisible by 3, so feeding the pair list to the raster path draws nonsense. Because `connector_pool` holds every object's indices end to end, it also shifts the triangle boundaries of every object after it, and the last triple can over-read the pool — the worker's `current_triangle_pointer < connector_pool_len` bound only checks the first index of a triple.

**Fix path.** Either add an explicit primitive-kind tag to `Object` (which would also let `render()` draw point clouds again), or refuse index counts that are not a multiple of 3 when building `connector_pool` and bound the worker on the whole triple: `current_triangle_pointer + 2 < connector_pool_len`.

---

## 4. Camera rotation

**What breaks.** There is no way to rotate the camera. `move_camera(unit, Movement)` handles translation only.

**Why.** World points are consumed by the projection directly. There is no view matrix, no per-Object model matrix, no matrix pipeline at all. Rotating an Object today means mutating its `vertices` array in place — which literally moves the model in the world, so you cannot tell "Object rotated" apart from "camera rotated the other way relative to a stationary Object."

**Fix path.** Add a 4x4 transform matrix to `Object` (the model matrix) and one to `Camera` (the view matrix). Multiply each vertex by `view * model` before projection.

---

## 5. No back-face culling

**What breaks.** Every triangle of a closed mesh is rasterized, including the roughly half facing away from the camera. They are then discarded by the depth compare, so the output is correct but up to twice the fill work is wasted — and with several threads, back faces add contention on the same pixels.

**Why.** There is no winding-order test. `clip_triangle_near` also does not preserve winding when it splits a triangle, so a culling test would need to establish orientation from the projected vertices rather than trusting index order.

**Fix path.** After projecting the three vertices, take the sign of the 2D cross product of two edges and skip triangles facing away. Decide a winding convention first, and make the clip respect it.

---

## 6. Depth and texture-coordinate interpolation is not perspective-correct

**What breaks.** Depth arbitration and the per-pixel `u`/`v` lerp both interpolate linearly in *screen* space. Under perspective projection that is close but wrong — foreshortening biases the true value toward the farther endpoint. Visible as a faint seam along the diagonal where the two triangles of a quad meet, on any face with a strong colour gradient.

Note this applies only to the raster-time lerp. `lerp_vectex` in the near clip interpolates in world space along a straight 3D edge, which is exact and unaffected.

**How much it currently costs.** Measured by computing both the affine and the perspective-correct sample for every span pixel in a frame and comparing:

| model | span px | sampled a different texel | differed by >32 per channel |
| --- | --- | --- | --- |
| `hand_painted_forest.glb` | 710,002 | 49.9% | **0.0%** |
| `woman_seated_v12.glb` | 45,647 | 0.2% | **0.0%** |
| `shareModel.obj` | 12,178 | 0.1% | **0.0%** |
| `model.glb` | 777 | 0.0% | **0.0%** |

So it is real but currently invisible: half the pixels of the forest land on a *neighbouring* texel, none land somewhere visibly different. These models are dense, so a triangle covers few pixels and the affine error has no room to accumulate. It becomes visible the moment a single triangle spans a lot of screen and a lot of depth — a floor, a wall, a ground plane — which is exactly what a low-poly test scene is made of.

**Fix path.** Store `u/w`, `v/w` and `1/w` in the `PixelCord` at projection time (`w = z - camera.z`), let Bresenham and the span fill interpolate those three linearly exactly as they already do, and divide at the sample site: `u = (u/w)/(1/w)`. No interpolation code changes — only what goes in and what comes out. It needs one new `PixelCord` field.

---

## 7. Frame-buffer allocations are never checked

**What breaks.** If memory runs out while allocating, the renderer crashes instead of failing cleanly.

**Why.** `create_frame_buffer` never checks its two `calloc` results, and `render_init` does not check the pixel-pool `calloc` either. `render()` tests `frame == NULL` only *before* swapping buffers, so a failed `frame2` is swapped in and drawn into anyway. `update_win()` never checks its buffer at all, and both callers (`main.c` and the event loop in `win_i_o.c`) ignore `render()`'s return value.

**Fix path.** Check all three allocations and have `render_init` report failure (it returns `void` today). Test the buffer after the swap in `render()`, and only call `update_win()` when `render()` returned true.

---

## 8. The rasterizer indexes the texture with no bounds check

**What breaks.** `rasterization.c:343-344` and `:367-368` compute

```c
size_t f_row=(size_t)(obj->texture_height*fill_pixel.v);
size_t f_col=(size_t)(obj->texture_width *fill_pixel.u);
fill_pixel.colour=texture[f_row][f_col];
```

with nothing between the multiply and the subscript. `v == 1.0` gives `row == texture_height`, one row past the end; a negative `v` converts to an enormous `size_t`. Both are out-of-bounds reads.

**Why.** The bound is currently held up entirely by the loaders: `mesh.h` makes "every `u` and `v` is inside `[0,1)`" part of the load contract, `mesh_wrap_uv` enforces it per vertex, and `mesh.c`'s `validate()` re-checks every vertex before an Object is handed back. Linear interpolation between two in-range values stays in range, so the invariant survives the raster walk — *except* through the near-plane clip lerp, which is exactly how it can be broken.

A hand-built `Object` (the cube in `main.c`) is not covered by any of that; its `u`/`v` are whatever the initialiser left.

**Fix path.** Clamp at the sample site — `if(row>=h)row=h-1;` and the same for the column — so the renderer holds its own invariant instead of trusting every producer of an `Object`. Wrapping (`row%h`) is the other option and is what a REPEAT sampler does, but clamping is one comparison and cannot turn a small error into a jump across the image.

---

## 9. `Vectex.colour` is no longer drawn

**What breaks.** A mesh whose colour lives per vertex and not in an image — PLY `red/green/blue`, glTF `COLOR_0`, the non-standard OBJ `v x y z r g b` — renders as one flat colour.

**Why.** The rasterizer overwrites every pixel's colour with a texture sample (`rasterization.c:346`, `:369`) rather than using the interpolated `current.colour`. The loaders still fill `Vectex.colour`, and the flat fallback texture is built from it (PLY averages every vertex colour into its 1x1 texture), so the model gets its overall tone but none of its variation.

There is no way to fix this in the loader: a texture lookup cannot reproduce per-vertex colour, because interpolating `u,v` across a triangle sweeps a rectangle of unrelated texels rather than blending three corner colours.

**Fix path.** In the rasterizer, use the texture only when the object has a real one:

```c
fill_pixel.colour = (obj->texture_width>1 || obj->texture_height>1)
    ? texture[f_row][f_col]
    : interpolate_colour(current.colour,next.colour,span,fill_i);
```

or give `Object` an explicit flag rather than inferring from the 1x1 fallback.

---

## 10. Three of fourteen models are refused by the `extensionsRequired` gate

**What breaks.** `citlali.glb`, `citlali/source/.../scene.gltf` and `a_salsa_dance.glb` load as `MESH_ERR_UNSUPPORTED`.

**Why.** `mesh_load_gltf` refuses any non-empty `extensionsRequired` array. These three require `KHR_materials_pbrSpecularGlossiness`, which is a *material model* extension: it changes how the base colour is combined with metal/roughness, not how a vertex or an index is stored. The geometry and `TEXCOORD_0` are perfectly readable.

The blanket refusal is right in principle — a required extension may change the meaning of the buffers (Draco does) — but it is too coarse to tell a geometry extension from a shading one.

**Fix path.** Keep the refusal as the default and allow-list the extensions that provably do not touch geometry: `KHR_materials_pbrSpecularGlossiness`, `KHR_materials_unlit`, `KHR_texture_transform` (it only offsets/scales uv, so it is a wrong-mapping risk, not a wrong-geometry one), `KHR_materials_emissive_strength`.

---

## 11. Texture alpha is discarded

**What breaks.** A texture's transparent regions paint as opaque colour. Anything authored as an alpha cutout — foliage, fences, grates, hair — renders as a solid card instead of a cut-out shape.

**Why.** The decoders keep the alpha channel and `Object.texture` stores it in the top byte, but nothing reads it. `rasterization.c` writes the sampled word into the frame buffer whole, and `update_win` copies `.colour` straight into an `SDL_PIXELFORMAT_ARGB8888` texture whose blend mode is the default `SDL_BLENDMODE_NONE`, so the alpha byte is carried all the way to SDL and then ignored.

Measured over the current model set, `dae_-_eco_house.glb` is the case that has it: **37.7%** of its dominant texture is fully transparent and another **13.2%** is partially transparent; 27.8% of its vertices sample a fully transparent texel. Every other model in the set is fully opaque, which is why this has not shown up as an obvious defect yet.

**Fix path.** The cheap version is an alpha cutout: at the sample site, `if((texel>>24) < 128) continue;` — skip the pixel entirely so it is neither painted nor depth-written. That is what a cutout material wants and it costs one comparison. True alpha blending needs the triangles sorted back-to-front, which the current z-buffer-only pipeline has no machinery for.

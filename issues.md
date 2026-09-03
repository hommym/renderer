# Renderer Issues

Open problems in the renderer. Each entry: what breaks, why it breaks, fix direction. Resolved items removed; historical narrative removed.

---

## 1. The frame buffer is raced by the rasterization threads

**What breaks.** The same scene renders differently every run. Some pixels end up holding a combination of values that no triangle ever produced.

**Why.** `rasterization.c:289-294` (span fill) and `rasterization.c:303-308` (edge pixel) both do an unsynchronised read-modify-write:

```c
PixelCord existing = frame_buffer[y][x];              // 1. read
if(!(existing.in_use && existing.z < fill_pixel.z))   // 2. decide
    frame_buffer[y][x] = fill_pixel;                  // 3. write
```

Work is partitioned **by triangle**, so two threads routinely land on the same pixel. Two failures follow:

- *Lost update.* Both threads read the same `existing`, both conclude they are in front, and the second write clobbers the first. The depth test was answered correctly about a state that no longer existed, so the farther fragment can win.
- *Torn write.* `sizeof(PixelCord) == 32` — four 64-bit words, not one atomic store. Two threads writing the same pixel interleave inside the copy.

Measured on 240 overlapping triangles with 8 workers: identical input, 8 runs, 8 different frame hashes at a constant 922,437 painted pixels. Between 78 and 943 pixels per run carried one triangle's colour beside another triangle's depth — a pairing that cannot exist in the input.

The cube hides this because 12 triangles barely contend for the same pixel; a real mesh contends at every overlap and silhouette.

**Fix path.** Partition so no two threads can address the same pixel, rather than locking. Interleave by screen row (thread `i` owns rows where `row % nthreads == i`) or by tile: every thread still walks every triangle, but only writes rows it owns, so the depth test stays a plain read-modify-write on private memory. Costs redundant clip/project per thread; the fill work — the expensive part — still divides. A mutex per pixel is impractical and one global lock removes the point of threading.

---

## 2. `rasterization_worker` falls off the end without returning

**What breaks.** Undefined behaviour on every worker exit.

**Why.** `rasterization.c:321` declares `void* rasterization_worker(void* args)` and the function body ends after the `while` loop with no `return`. Reaching the closing brace of a non-`void` function and having the caller use the value is UB. `pthread_join` is passed `NULL` for the result today, so nothing reads it and it does not misbehave in practice.

**Fix path.** `return NULL;` at the end. `args` is also unused (`-Wunused-parameter`) — cast it to void or use it, since the worker index is currently passed and discarded.

---

## 3. `Object` cannot distinguish a triangle list from an edge list

**What breaks.** `render()` sends any object with `len_of_connectors != 0` to the triangle rasterizer, which reads `connectors_sequence` three at a time. Nothing records how many indices per primitive the array actually holds.

**Why.** `model_data.h` ships two index arrays over the same vertices: `model_connectors` (pairs, 392,102 entries) and `model_tri_connectors` (triples, 2,534,133 entries). They differ only by which field an `Object` literal points at. 392,102 is not divisible by 3, so feeding the pair list to the raster path both draws nonsense *and* over-reads the end of the array — the worker loop's `p < len_of_connectors` bound is only safe because a triangle list's length is always a multiple of 3.

**Fix path.** Either add an explicit primitive-kind tag to `Object` (also removes the `len_of_connectors == 0` point-cloud dispatch trick), or make the worker bound independent of the assumption: `while(p + 2 < obj.len_of_connectors)`.

---

## 4. No per-object transform

**What breaks.** A model cannot be moved, turned or scaled independently of the
camera. The only way to place one is to mutate its `vertices` array, which
literally relocates it in the world -- so "rotate this object" and "rotate the
camera the other way" are indistinguishable, and two objects cannot be posed
differently from one another.

**Why.** The camera now has an orientation (`yaw`/`pitch`) and every vertex is
rotated into view space before projection, so the *view* side of a matrix
pipeline exists. The model side does not: `Object` carries no transform, and
`mesh_fit_to_view` places a model by rewriting every vertex once at load time.

**Fix path.** Give `Object` a 4x4 model matrix and multiply it into the same
pass that already applies the view transform in `rasterizer()` -- one
`model * view` composition per object per frame rather than per vertex. That
also removes the need for `mesh_fit_to_view` to rewrite geometry, since the fit
becomes a matrix like any other.

---

## 5. No back-face culling

**What breaks.** Every triangle of a closed mesh is rasterized, including the roughly half facing away from the camera. They are then discarded by the depth compare, so the output is correct but up to twice the fill work is wasted — and under issue 1, back faces are extra contention on the same pixels.

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

## 7. `clear_frame_buffer(true)` leaks the old buffer

**What breaks.** Calling it with `keep_frame == true` leaks the previous frame buffer — one full `screen_width * screen_height * sizeof(PixelCord)` allocation per call (48 MB at 1500x1000).

**Why.** `renderer.c:158`:

```c
if(!keep_frame) free(frame);
if(screen_hieght!=0 && screen_width!=0) create_frame_buffer(screen_width,screen_hieght);
```

`create_frame_buffer` unconditionally overwrites `frame` with a fresh `calloc`, so skipping the `free` does not keep the old buffer reachable — it strands it. No caller passes `true` today (all three call sites pass `false`), but `ARCHITECTURE.md` documents the flag as meaning the old buffer "still exists and can be referenced".

**Fix path.** Decide what the flag means. If it is "reuse the existing allocation", return early instead of reallocating. If it is "hand ownership of the old buffer to the caller", it has to return the pointer. `create_frame_buffer` also never checks its `calloc` result; `render()` guards on `frame == NULL` but `update_win()` does not.

---

## 8. The rasterizer indexes the texture with no bounds check

**What breaks.** `rasterization.c:290-291` and `:305-306` compute

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

**Why.** The rasterizer overwrites every pixel's colour with a texture sample (`rasterization.c:293`, `:307`) rather than using the interpolated `current.colour`. The loaders still fill `Vectex.colour`, and the flat fallback texture is built from it (PLY averages every vertex colour into its 1x1 texture), so the model gets its overall tone but none of its variation.

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

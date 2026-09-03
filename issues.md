# Renderer Issues

Open problems in the renderer. Each entry: what breaks, why it breaks, fix direction. Resolved items removed; historical narrative removed.

---

## 1. Shared triangle edges break depth ties by arrival order

**What breaks.** The same scene renders slightly differently every run. Roughly a
thousand pixels out of 400,000 change colour between two renders of an identical
frame.

**Why.** Not a data race any more -- the row locks fixed that, and it is worth
being precise about what is left:

| | before locks | after locks |
| --- | --- | --- |
| torn 40-byte writes | 210 over 6 frames | **0** |
| pixels where `z` differs run to run | many | **0** |
| pixels where `u`/`v` differ run to run | many | ~1,000 |

Depth is now identical on every pixel of every run, so the correct nearest
fragment always wins. What varies is *which of two fragments at exactly the same
depth* wins, and the depth test breaks that tie by whoever writes last:

```c
if(!(existing.in_use && existing.z < fill_pixel.z))   // strict <, so a tie overwrites
```

Measured: 21,685 exact ties on `dae_-_eco_house.glb` and 7,050 on
`woman_seated_v12.glb` -- **all of them in the edge-pixel path, none in the span
fill**. That is the signature of adjacent triangles both rasterizing the edge
they share. Both land on the same pixel at the same depth carrying different
interpolated `u`/`v`, and whichever thread gets there second wins.

**Fix path.** Give the tie a stable answer instead of an order-dependent one:
on exact equality keep whichever fragment sorts first by some key the threads
agree on regardless of when they arrive -- the packed `u`/`v`, or a triangle
index if `Object` ever carries one. One comparison at each of the two write
sites. It cannot be fixed by locking harder; both writers are equally correct
and the renderer simply has not said which it prefers.

---

## 2. `rasterization_worker` falls off the end without returning

**What breaks.** Undefined behaviour on every worker exit.

**Why.** `rasterization.c:384` declares `void* rasterization_worker(void* args)` and the function body ends after the `while` loop with no `return`. Reaching the closing brace of a non-`void` function and having the caller use the value is UB. `pthread_join` is passed `NULL` for the result today, so nothing reads it and it does not misbehave in practice.

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

## 5. `clip_triangle_near` does not preserve winding

**What breaks.** Nothing today, but it rules out any test that needs to know
which way a clipped triangle faces.

**Why.** The clip sorts vertices into `inside[]`/`outside[]` in *index* order,
which loses their cyclic position. In the one-corner-survives case with the
surviving corner at index 1, the two generated vertices come out swapped and the
output triangle is wound the opposite way from its input; the two-corner case has
the same problem for some orientations.

The back-face cull dodges this by testing before the clip rather than after:
clipping only cuts a triangle up within its own plane, so every piece has the
same normal and the same facing as the whole, and one test on the source
triangle covers all of them.

**Fix path.** Replace the branchy sort with a Sutherland-Hodgman walk, which is
winding-preserving by construction and shorter than what is there now: step the
three edges in order, emit each inside vertex, and emit a crossing wherever an
edge changes side. Three or four vertices come back; fan-triangulate them.

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

## 7. The rasterizer indexes the texture with no bounds check

**What breaks.** `rasterization.c:353-354` and `:368-369` compute

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

## 8. `Vectex.colour` is no longer drawn

**What breaks.** A mesh whose colour lives per vertex and not in an image — PLY `red/green/blue`, glTF `COLOR_0`, the non-standard OBJ `v x y z r g b` — renders as one flat colour.

**Why.** The rasterizer overwrites every pixel's colour with a texture sample (`rasterization.c:356`, `:370`) rather than using the interpolated `current.colour`. The loaders still fill `Vectex.colour`, and the flat fallback texture is built from it (PLY averages every vertex colour into its 1x1 texture), so the model gets its overall tone but none of its variation.

There is no way to fix this in the loader: a texture lookup cannot reproduce per-vertex colour, because interpolating `u,v` across a triangle sweeps a rectangle of unrelated texels rather than blending three corner colours.

**Fix path.** In the rasterizer, use the texture only when the object has a real one:

```c
fill_pixel.colour = (obj->texture_width>1 || obj->texture_height>1)
    ? texture[f_row][f_col]
    : interpolate_colour(current.colour,next.colour,span,fill_i);
```

or give `Object` an explicit flag rather than inferring from the 1x1 fallback.

---

## 9. Three of fourteen models are refused by the `extensionsRequired` gate

**What breaks.** `citlali.glb`, `citlali/source/.../scene.gltf` and `a_salsa_dance.glb` load as `MESH_ERR_UNSUPPORTED`.

**Why.** `mesh_load_gltf` refuses any non-empty `extensionsRequired` array. These three require `KHR_materials_pbrSpecularGlossiness`, which is a *material model* extension: it changes how the base colour is combined with metal/roughness, not how a vertex or an index is stored. The geometry and `TEXCOORD_0` are perfectly readable.

The blanket refusal is right in principle — a required extension may change the meaning of the buffers (Draco does) — but it is too coarse to tell a geometry extension from a shading one.

**Fix path.** Keep the refusal as the default and allow-list the extensions that provably do not touch geometry: `KHR_materials_pbrSpecularGlossiness`, `KHR_materials_unlit`, `KHR_texture_transform` (it only offsets/scales uv, so it is a wrong-mapping risk, not a wrong-geometry one), `KHR_materials_emissive_strength`.

---

## 10. Texture alpha is discarded

**What breaks.** A texture's transparent regions paint as opaque colour. Anything authored as an alpha cutout — foliage, fences, grates, hair — renders as a solid card instead of a cut-out shape.

**Why.** The decoders keep the alpha channel and `Object.texture` stores it in the top byte, but nothing reads it. `rasterization.c` writes the sampled word into the frame buffer whole, and `update_win` copies `.colour` straight into an `SDL_PIXELFORMAT_ARGB8888` texture whose blend mode is the default `SDL_BLENDMODE_NONE`, so the alpha byte is carried all the way to SDL and then ignored.

Measured over the current model set, `dae_-_eco_house.glb` is the case that has it: **37.7%** of its dominant texture is fully transparent and another **13.2%** is partially transparent; 27.8% of its vertices sample a fully transparent texel. Every other model in the set is fully opaque, which is why this has not shown up as an obvious defect yet.

**Fix path.** The cheap version is an alpha cutout: at the sample site, `if((texel>>24) < 128) continue;` — skip the pixel entirely so it is neither painted nor depth-written. That is what a cutout material wants and it costs one comparison. True alpha blending needs the triangles sorted back-to-front, which the current z-buffer-only pipeline has no machinery for.

---

## 11. Every material in the current model set is marked `doubleSided`

**What breaks.** Back-face culling never engages on any model that is loaded
today, so none of them get the roughly one third off frame time it offers.

**Why.** The cull honours glTF's per-material `doubleSided` flag, and all 47
materials across the model set set it to true. That is almost certainly exporter
default rather than intent -- Blender's glTF exporter writes `doubleSided: true`
whenever a material's "Backface Culling" box is unticked, which is the default --
but the file says what it says and guessing against it deletes geometry.

`set_backface_cull_forced(true)` overrides it. What that costs, measured against
the unculled render at 900x700:

| model | pixels changed | frame time |
| --- | --- | --- |
| `corrupted_archangel` (1.96M tris) | 154 (**0.02%**) | 418 -> 263 ms |
| `fallen_paladin` (1.99M tris) | 129 (**0.02%**) | 418 -> 275 ms |
| `model.glb` tree (845k tris) | 490 (**0.08%**) | 204 -> 120 ms |
| `dae_-_eco_house` (124k tris) | 26,054 (**4.14%**) | 57 -> 41 ms |
| `shareModel.obj` castle (1M tris) | 52,730 (**8.37%**) | 205 -> 123 ms |

The closed character meshes lose nothing measurable -- 0.02% is the depth-test
noise floor from issue 1 -- and run a third faster. The eco house loses foliage
cards seen from behind, which are genuinely single-sided geometry the flag is
there to protect. The castle is worse than either: it is a photogrammetry scan
with inconsistently wound triangles, so culling shreds its top surface into
patches rather than removing a coherent set of back faces.

**Fix path.** Nothing to fix in the renderer -- the flag is being honoured
correctly. What would help is deciding per model: force it on for closed meshes,
leave it off for scans and foliage. A per-Object override set at import time from
something better than the exporter default (a triangle-adjacency check for
"is this mesh closed and consistently wound") would make it automatic.

# Renderer Issues

Open problems in the renderer. Each entry: what breaks, why it breaks, fix
direction. Resolved items removed; historical narrative removed.

---

## 1. No per-object transform

**What breaks.** A model cannot be moved, turned or scaled independently of the
camera. The only way to place one is to mutate its `vertices` array, which
literally relocates it in the world -- so "rotate this object" and "rotate the
camera the other way" are indistinguishable, and two objects cannot be posed
differently from one another.

**Why.** The camera has an orientation (`yaw`/`pitch`) and every vertex is
rotated into view space before projection, so the *view* side of a matrix
pipeline exists. The model side does not: `Object` carries no transform, and
`mesh_model_fit` places a model by rewriting every vertex once at load time.

**Fix path.** Give `Object` a 4x4 model matrix and compose it with the view
transform once per object per frame in `rasterization_setup_pass`, rather than
per vertex. That also removes the need for `mesh_fit_to_view` to rewrite
geometry, since the fit becomes a matrix like any other.

---

## 2. `clip_triangle_near` does not preserve winding

**What breaks.** Nothing today, but it rules out any test that needs to know
which way a clipped triangle faces.

**Why.** The clip sorts vertices into `inside[]`/`outside[]` in *index* order,
which loses their cyclic position. In the one-corner-survives case with the
surviving corner at index 1, the two generated vertices come out swapped and the
output triangle is wound the opposite way from its input; the two-corner case
has the same problem for some orientations.

The back-face cull dodges this by testing before the clip rather than after:
clipping only cuts a triangle up within its own plane, so every piece has the
same normal and the same facing as the whole, and one test on the source
triangle covers all of them.

**Fix path.** Replace the branchy sort with a Sutherland-Hodgman walk, which is
winding-preserving by construction and shorter than what is there now: step the
three edges in order, emit each inside vertex, and emit a crossing wherever an
edge changes side. Three or four vertices come back; fan-triangulate them.

---

## 3. `Vectex.colour` is never drawn

**What breaks.** A mesh whose colour lives per vertex and not in an image --
PLY `red/green/blue`, glTF `COLOR_0`, the non-standard OBJ `v x y z r g b` --
renders as one flat colour.

**Why.** The rasterizer's fill loop takes every pixel's colour from a texture
sample. The loaders still fill `Vectex.colour`, and the flat fallback texture is
built from it (PLY averages every vertex colour into its 1x1 texture), so the
model gets its overall tone but none of its variation.

There is no way to fix this in the loader: a texture lookup cannot reproduce
per-vertex colour, because interpolating `u,v` across a triangle sweeps a
rectangle of unrelated texels rather than blending three corner colours.

**Fix path.** Carry colour through the setup record the way `u`/`v` already are
-- as three per-vertex values interpolated across the span -- and choose between
it and the texture sample with an explicit `Object` flag set at load time.
Inferring "has a real texture" from `texture_width>1` is not sound: a genuine
1x1 texture from a file would be misread as the fallback.

---

## 4. Sidedness is exporter default, not intent

**What breaks.** Back-face culling engages only where a file happens to say
`doubleSided: false`, and most exporters write `true` regardless. A closed model
that would lose nothing to a cull often still pays to draw both sides of every
triangle.

**Why.** The cull honours glTF's per-material `doubleSided` flag, which is what
a loader should do -- the file says what it says and guessing against it deletes
geometry. But Blender's glTF exporter writes `doubleSided: true` whenever a
material's "Backface Culling" box is unticked, which is the default.

`C` toggles `set_backface_cull_forced()` at runtime, and forcing it on is worth
roughly a third of the frame time on a closed mesh. It is a manual choice
because it is genuinely wrong for some models: `dae_-_eco_house` loses foliage
cards seen from behind, and `shareModel.obj` is a photogrammetry scan with
inconsistently wound triangles, so culling shreds its top surface.

**Fix path.** Decide it per object at import time from something better than the
exporter default -- a triangle-adjacency check for "is this mesh closed and
consistently wound" -- and store the answer on the `Object`.

---

## 5. A multi-material OBJ is painted with one material's texture

**What breaks.** An OBJ that uses several `usemtl` materials is merged into a
single `Object` and textured with whichever material covers the most triangles.
Every other material's geometry samples an image that is not its own, so the
result is not blank, it is confidently wrong.

**Why.** `mesh_load_gltf_scene` splits a glTF into one `Object` per material,
but `mesh_load_obj` has no equivalent: it tallies triangles per material and
picks a single winner (`src/mesh_obj.c`, the `best` selection near the end).
`mesh_load_scene` then wraps that one Object in a one-element `Model`.

**Fix path.** Bucket OBJ faces by material index exactly as `mesh_gltf.c` does
with its `Bucket` array, and return a multi-object `Model`. The glTF path is the
worked example; the shape of the fix is the same.

---

## 6. PLY per-face texture coordinates are discarded

**What breaks.** A textured photogrammetry PLY -- the dominant textured-PLY
layout in the wild -- samples one texel for the entire mesh.

**Why.** The loader reads texture coordinates only as vertex properties (`s`/`t`
or `u`/`v`). The common layout instead puts them on the *face* element, as a
list property (`property list uchar float texcoord`) holding six floats per
triangle. Nothing reads that, so every vertex keeps u=v=0.

**Fix path.** Read the face `texcoord` list alongside the vertex indices, and
key vertices on (position index, uv pair) the way `mesh_obj.c`'s `CornerMap`
already does -- the same problem with the same solution, because a position
shared between two faces with different uvs has to become two vertices.

---

## 7. Skinned glTF meshes render their bind pose, not their scene pose

**What breaks.** A rigged character loads and renders, but in whatever pose its
inverse bind matrices describe -- usually a T-pose or A-pose -- rather than the
pose the file's node hierarchy puts it in.

**Why.** The loader does not skin. It draws skinned meshes with an identity
model matrix, which is exactly right when the rest hierarchy *is* the bind pose:
`jointMatrix = globalJoint * inverseBindMatrix` is then identity for every
joint, and the weighted sum collapses to the vertex itself. Measured against a
full linear-blend-skinning reference, all three skinned models in `3dmodels/`
agree to within 8.7e-6 of model extent -- so for this corpus the bind pose *is*
the file's pose. A file exported with a posed skeleton would differ.

**Fix path.** Implement linear blend skinning: read `JOINTS_0` and `WEIGHTS_0`,
walk the node hierarchy for each joint's global transform, multiply by the
skin's `inverseBindMatrices`, and blend per vertex. The accessor readers,
`mat_mul` and `node_matrix` all already exist. Cheaper interim step: compute
`globalJoint * IBM` over the weighted joints and warn when it is not identity,
so the case is at least visible rather than silent.

---

## 8. glTF `baseColorFactor` is dropped whenever a texture is present

**What breaks.** A material that tints its texture -- one image reused across
several materials, each with a different `baseColorFactor` -- renders untinted,
so all of them look identical.

**Why.** `prim_material` reads the factor into `*flat`, but `*flat` is only used
to build the 1x1 fallback when there is no image. When there is one, the texture
is sampled raw and the factor never multiplies it.

**Fix path.** When a material has both an image and a non-white factor, multiply
the factor into a private copy of the decoded texture at load time, and key the
texture cache on (image, factor) rather than image alone so the copy is shared
by every material with that pair.

---

## 9. Texture sampler wrap modes are ignored

**What breaks.** Texture coordinates are wrapped as if every sampler were
REPEAT. A material whose sampler asks for CLAMP_TO_EDGE and whose uvs run
outside [0,1] gets the image tiled instead of its edge held, which shows up as
the far edge of the atlas bleeding in along a seam.

**Why.** `mesh_wrap_uv` applies one policy to every coordinate, at load time,
before anything knows which sampler the material uses. `texture.sampler` ->
`sampler.wrapS`/`wrapT` is never read.

**Fix path.** Read the sampler and store the mode on the `Object`, then apply it
at the sample site (where the rasterizer already clamps for safety) rather than
folding it into the vertex at load time.

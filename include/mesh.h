#ifndef MESH_H
#define MESH_H

// Mesh file loading. Every loader produces an Object holding a flat vertex array
// and a TRIANGLE index list (3 indices per triangle), because render() infers the
// primitive kind from len_of_connectors and reads connectors three at a time.
// Anything that is not triangles is either triangulated at load time or rejected
// with MESH_ERR_UNSUPPORTED -- never silently reinterpreted.
//
// Colour reaches the rasterizer through the texture, not the vertex. Each vertex
// carries a (u,v) pair and the Object carries one image; the rasterizer
// interpolates u,v across a triangle and looks the colour up per pixel. So a
// loader has three jobs beyond geometry:
//
//   1. fill Vectex.u and Vectex.v for every vertex,
//   2. leave both inside [0,1) -- the rasterizer indexes the texture with
//      (size_t)(texture_height*v) and (size_t)(texture_width*u) and does no
//      bounds check of its own, so u==1.0 or a negative v is an out-of-bounds
//      read, not a wrapped sample,
//   3. always set texture / texture_width / texture_height. The rasterizer
//      samples unconditionally, so a NULL texture is a null dereference. A file
//      with no image gets a 1x1 texture of its flat colour instead.
//
// Vectex.colour is still filled where the file provides one (PLY vertex colour,
// OBJ's non-standard trailing triple, glTF COLOR_0), but nothing downstream
// reads it: the rasterizer overwrites every pixel's colour with a texture
// sample. It is kept because it costs nothing and is what a flat fallback
// texture is built from.
//
// Ownership: a loader allocates Object.vertices, Object.connectors_sequence and
// Object.texture with malloc. The caller owns them and releases them with
// mesh_free(). The renderer only reads the arrays and never frees anything, so a
// loaded Object is handed to it the same way a static one is.

#include <stdint.h>
#include <stdbool.h>
#include "renderer.h"

// Used when a file carries no colour at all: no texture, no material base
// colour and no per-vertex colour.
#define MESH_DEFAULT_COLOUR 0xFFB4B4B4u

// Longest side a loaded texture may have. Model diffuse maps are routinely
// 8192x8192, which is 268MB of 32-bit pixels for a software rasterizer drawing
// into a window a fraction of that size; anything larger is box-filtered down.
#define MESH_TEXTURE_MAX_DIM 2048u

typedef enum MeshResult {
    MESH_OK = 0,
    MESH_ERR_OPEN,          // could not open the file
    MESH_ERR_READ,          // truncated or I/O error partway through
    MESH_ERR_FORMAT,        // malformed for the format it claims to be
    MESH_ERR_UNSUPPORTED,   // well-formed but uses a feature this loader does not handle
    MESH_ERR_OOM,           // allocation failed
    MESH_ERR_EMPTY          // parsed cleanly but contains no triangles
} MeshResult;

typedef enum MeshFormat {
    MESH_FORMAT_UNKNOWN = 0,
    MESH_FORMAT_PLY,
    MESH_FORMAT_OBJ,
    MESH_FORMAT_GLTF,       // .gltf, JSON with external or data: URI buffers
    MESH_FORMAT_GLB         // .glb, single binary container
} MeshFormat;

// ============================================================================
// THE ONE YOU WANT
// ============================================================================
//
// A model file goes in, something render_init() can take comes straight out:
//
//     Model m={0};
//     if(mesh_import(path,420.0,180.0,0.0,550.0,&m)==MESH_OK)
//         render_init(m.objects,m.len,win_w,win_h);
//     ...
//     mesh_model_free(&m);
//
// It is a `Model` rather than a single Object because an Object carries ONE
// texture and a model file routinely paints its geometry with several. That is
// not a detail you have to think about -- m.objects IS the flat array render()
// walks, and m.len is its length -- but it is why the count is not always 1.

typedef struct Model {
    Object*    objects;         // hand straight to render_init() / set_objects()
    uint64_t   len;
    // bookkeeping. textures are shared between the objects, so the model owns
    // them and mesh_model_free releases them; nothing here needs touching.
    uint32_t** textures;
    size_t     texture_count;
    // Primitives the loader could not draw and dropped: point/line primitives,
    // morph targets, Draco-compressed geometry. Non-zero means what you are
    // looking at is a PART of the file. Zero for every other format.
    uint64_t   primitives_skipped;
} Model;

// Detects the format, parses it, splits it by material, decodes its textures,
// and scales and centres it for the view. Everything below this line is the
// machinery it is built from; you do not need any of it.
//
// target_extent is how many world units the model's longest axis should span,
// and (cx,cy,cz) is where to centre it -- a model file carries its own units and
// origin, so without this it is either a speck or swallows the screen. Pass 0
// for target_extent to keep the file's own coordinates untouched.
//
// On MESH_OK the Model is yours and mesh_model_free releases it. On any error it
// comes back zeroed with nothing to free, and mesh_result_string(r) says why.
MeshResult mesh_import(const char* path, double target_extent,
                       double cx, double cy, double cz, Model* out);

// Frees every object's arrays and every distinct texture. Safe on a zeroed or
// already-freed Model, and safe on NULL.
void mesh_model_free(Model* m);

// ============================================================================
// Lower-level pieces. mesh_import is built out of these; reach for them only if
// you want one of the steps on its own.
// ============================================================================

// Sniffs magic bytes first ("ply", "glTF", '{' for gltf) and falls back to the
// filename extension, so a correctly-formed file loads under any name.
MeshFormat mesh_detect_format(const char* path);

// Detect and dispatch. On MESH_OK, *out is fully populated and owned by the
// caller; on any error *out is left zeroed and nothing needs freeing.
MeshResult mesh_load(const char* path, Object* out);

MeshResult mesh_load_ply(const char* path, Object* out);
MeshResult mesh_load_obj(const char* path, Object* out);
MeshResult mesh_load_gltf(const char* path, Object* out);   // handles both .gltf and .glb

// glTF split by material; see Model below.
MeshResult mesh_load_gltf_scene(const char* path, Model* out);

// Frees the arrays and the texture, and zeroes the Object. Safe on a zeroed or
// already-freed Object, and safe on NULL.
//
// NOT for an Object that came out of a Model -- those share their textures
// with each other and the scene owns them. Use mesh_model_free for those.
void mesh_free(Object* obj);

// ---- the two steps mesh_import is made of ---------------------------------

// Load and split by material, WITHOUT fitting. An Object carries one texture, so
// merging a multi-material file into a single Object means most of the mesh
// samples an image that is not its own: the uv coordinates stay valid numbers,
// so the result is not blank, it is confidently wrong. One Object per material
// keeps them apart. PLY and OBJ come back as a one-object Model, which is what
// they almost always are.
MeshResult mesh_load_scene(const char* path, Model* out);

// mesh_fit_to_view over the whole Model at once. Fitting each object separately
// would scale every material's share to the same size and pile them on top of
// each other, so the bounding box has to be taken across all of them and the one
// resulting transform applied to all of them.
//
// flip_y mirrors the y axis; every format here is +y up and this renderer is
// +y down, so mesh_import always passes true.
bool mesh_model_fit(Model* m, double target_extent,
                    double cx, double cy, double cz, bool flip_y);

const char* mesh_result_string(MeshResult r);

// ---- shared loader helpers ----------------------------------------------

// Folds a raw file texture coordinate into [0,1). Values outside the unit square
// are the norm rather than an error -- a tiled floor is authored as u running 0
// to 8 -- so they wrap the way a REPEAT sampler would. A non-finite coordinate
// becomes 0.
float mesh_wrap_uv(double t);

// A path read out of a mesh file -- an mtl reference, a ply TextureFile comment
// -- names a sibling of the model. mesh_path_is_safe rejects the ones that do
// not: an absolute path or a ".." component would let a crafted model file point
// the loader at any file the process can open. mesh_path_sibling resolves a safe
// one against base_path's directory, normalising Windows separators, and returns
// a malloc'd path the caller frees.
bool  mesh_path_is_safe(const char* rel);
char* mesh_path_sibling(const char* base_path, const char* rel);

// What a loader should actually call for a texture reference. mesh_path_sibling
// alone rejects anything that is not already a sibling, and exporters routinely
// write an absolute path that was only ever valid on the machine that made the
// file -- "map_Kd C:/Users/.../Textures/wall.png". Those are not siblings, so
// the texture was being dropped and the model rendered flat.
//
// So: try the path as written, and if that is not a safe sibling, retry with
// just its last component. The basename retry is still resolved against
// base_path's directory, so it cannot escape it -- the security property
// mesh_path_is_safe exists for is unchanged.
//
// Returns a malloc'd path the caller frees, or NULL if neither form resolves to
// a file that exists.
char* mesh_texture_sibling(const char* base_path, const char* rel);

// Replaces obj's texture with a 1x1 image of one colour. This is what a file
// with no usable image gets, so that the rasterizer always has something to
// sample. Returns false only on allocation failure.
bool mesh_set_flat_texture(Object* obj, uint32_t argb);

// Model files are in their own units and origin; this camera is a fixed pinhole
// at the world origin looking down +z. Rescales the mesh so its longest axis is
// target_extent world units and recentres it on (cx, cy, cz).
//
// flip_y mirrors the y axis, which glTF and OBJ both need: they are +y up, this
// renderer is +y down. Texture coordinates are untouched -- v already points the
// way the renderer indexes rows.
//
// Returns false and leaves obj untouched if it has no vertices or zero extent.
bool mesh_fit_to_view(Object* obj, double target_extent,
                      double cx, double cy, double cz, bool flip_y);

#endif

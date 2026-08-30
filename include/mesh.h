#ifndef MESH_H
#define MESH_H

// Mesh file loading. Every loader produces an Object holding a flat vertex array
// and a TRIANGLE index list (3 indices per triangle), because render() infers the
// primitive kind from len_of_connectors and reads connectors three at a time.
// Anything that is not triangles is either triangulated at load time or rejected
// with MESH_ERR_UNSUPPORTED -- never silently reinterpreted.
//
// Ownership: a loader allocates Object.vertices and Object.connectors_sequence
// with malloc. The caller owns them and releases them with mesh_free(). The
// renderer only reads the arrays and never frees anything, so a loaded Object is
// handed to it the same way a static one is.

#include <stdint.h>
#include <stdbool.h>
#include "renderer.h"

// Used when a file carries no per-vertex colour. glTF meshes usually have none
// (colour lives in a texture, and this renderer has no texture units), and OBJ
// vertex colour is a non-standard extension.
#define MESH_DEFAULT_COLOUR 0xFFB4B4B4u

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

// Sniffs magic bytes first ("ply", "glTF", '{' for gltf) and falls back to the
// filename extension, so a correctly-formed file loads under any name.
MeshFormat mesh_detect_format(const char* path);

// Detect and dispatch. On MESH_OK, *out is fully populated and owned by the
// caller; on any error *out is left zeroed and nothing needs freeing.
MeshResult mesh_load(const char* path, Object* out);

MeshResult mesh_load_ply(const char* path, Object* out);
MeshResult mesh_load_obj(const char* path, Object* out);
MeshResult mesh_load_gltf(const char* path, Object* out);   // handles both .gltf and .glb

// Frees the arrays and zeroes the Object. Safe on a zeroed or already-freed
// Object, and safe on NULL.
void mesh_free(Object* obj);

const char* mesh_result_string(MeshResult r);

// Model files are in their own units and origin; this camera is a fixed pinhole
// at the world origin looking down +z. Rescales the mesh so its longest axis is
// target_extent world units and recentres it on (cx, cy, cz).
//
// flip_y mirrors the y axis, which glTF and OBJ both need: they are +y up, this
// renderer is +y down.
//
// Returns false and leaves obj untouched if it has no vertices or zero extent.
bool mesh_fit_to_view(Object* obj, double target_extent,
                      double cx, double cy, double cz, bool flip_y);

#endif

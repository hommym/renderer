#ifndef TRANSFORM_H
#define TRANSFORM_H

// The world-to-view step, and the camera basis it is built from.
//
// The whole point of this file is that NOTHING downstream of it changes.
// projection.c, clip_triangle_near() and is_vectex_visible() all assume the
// camera looks straight down +z out of an axis-aligned box. That assumption is
// what a rotating camera appears to break -- and it is also exactly what view
// space restores.
//
// So instead of teaching the projection about rotation, every vertex is rotated
// into the camera's frame first:
//
//     v' = eye + R^T * (v - eye)
//
// Rotate about the eye and put the result back at the eye. After that, the
// camera's forward direction IS +z again and the geometry sits in the same
// axis-aligned situation the existing code already handles. The near-plane
// clip, the perspective divide and the frustum cull are untouched.
//
// The basis is rebuilt once per frame (view_refresh) rather than per vertex,
// because the sines and cosines are the expensive part and the camera does not
// move during a frame. view_apply is then nine multiplies and no trigonometry.

#include "renderer.h"

// Recomputes the camera basis from the current camera. Call once at the top of
// a frame, before any vertex is transformed. Not safe to call while the
// rasterization threads are running -- they only read it.
void view_refresh(void);

// World space -> view-aligned space, using the basis from the last
// view_refresh(). Position, and nothing else: u, v and colour pass through.
Vectex view_apply(Vectex v);

// The camera's own axes in world space, for movement that follows where you are
// looking. `right` is always horizontal (yaw only), `fwd` includes pitch, and
// `down` completes the set. Any of the three may be NULL.
//
// This does its own trigonometry, so it is for key and mouse handling, not for
// anything that runs per vertex.
void camera_axes(double fwd[3], double right[3], double down[3]);

// Where the eye actually is. The Camera stores the far-plane extents, and the
// eye sits at the centre of that box, so this is not just (camera.x, y, z).
void camera_eye(double eye[3]);

#endif

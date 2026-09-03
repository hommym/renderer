#ifndef RENDERER
#define RENDERER
#include <stdint.h>
#include <stdio.h>
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <math.h>
#include <stdatomic.h>
#include <pthread.h>

typedef struct Camera{
double x; // starting positions
double y;
double z;

double x_end;
double y_end;
double z_end;  // far plane distance
double focal_l; // focal length, derived from screen_hieght and v_fov
const double v_fov; // vertical fov (rad), constant input
double h_fov; // horizontal fov (rad), derived from aspect ratio

// Where the camera is looking. The box above stays axis-aligned; these two
// rotate the WORLD into the camera's frame instead (see transform.h), which is
// what lets the projection, the near clip and the frustum cull stay exactly as
// they were. Both zero is the original camera: straight down +z.
double yaw;     // radians, about the vertical axis, positive turns right
double pitch;   // radians, about the horizontal axis, positive looks up
} Camera;



typedef struct Vectex
{
double x;
double y;
double z;
float  u;
float  v;
uint32_t colour;

} Vectex;



typedef struct Object
{
    Vectex*   vertices;
    uint64_t  len_of_vertices;
    uint64_t* connectors_sequence;
    uint64_t  len_of_connectors;
    uint32_t* texture;
    size_t    texture_width;
    size_t    texture_height;

} Object;






typedef struct PixelCord{
double px;  
double py;
double z;
bool is_visible;
bool in_use;
float  u;
float  v;
uint32_t colour;
} PixelCord;







typedef enum Movement{
    MOV_RIGHT,
    MOV_LEFT,
    MOV_DOWN,
    MOV_UP,
    MOV_FORWARD,
    MOV_BACKWARD,
} Movement;




extern uint32_t screen_width;
extern uint32_t screen_hieght;
extern _Atomic size_t triangle_tracker;






void render_init(Object* objs,uint64_t len,uint32_t win_w,uint32_t win_h);

// Re-point the renderer at the caller's object array after it has been grown,
// reallocated or replaced. The renderer never grows, copies or frees it.
void set_objects(Object* objs,uint64_t len);
bool render();
void clear_frame_buffer(bool keep_frame);
void renderer_resize(uint32_t win_w,uint32_t win_h);
// Movement is relative to where the camera is looking: MOV_FORWARD follows the
// gaze including pitch, MOV_LEFT/RIGHT strafe along the horizontal right axis,
// and MOV_UP/DOWN stay on the world vertical so they cannot be tilted into a
// dive.
void move_camera(double unit,Movement direction);

// Turn the camera in place. Deltas in radians, added to the current angles.
// Pitch is clamped just short of straight up and straight down: at exactly
// vertical the horizontal heading is undefined and the view rolls.
void rotate_camera(double d_yaw,double d_pitch);
Camera get_camera_pos();
void* get_frame_buffer();
Object* get_current_object();


#endif

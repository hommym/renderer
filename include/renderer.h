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
} Camera;

typedef struct Vectex
{
double x;
double y;
double z;
uint32_t colour;

} Vectex;


typedef struct PixelCord{
double px;  
double py;
double z;
bool is_visible;
bool in_use;
uint32_t colour;
} PixelCord;




typedef struct Object
{
    Vectex* vertices;
    uint64_t len_of_vertices;
    uint64_t* connectors_sequence;
    uint64_t  len_of_connectors;

    // intrusive scene-list link, owned by the renderer. set only through
    // add_object / remove_object -- never assign it yourself, and never walk it
    // on a by-value copy of an Object, where it is a stale pointer.
    struct Object* next;
} Object;

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






void render_init(uint32_t win_w,uint32_t win_h);

// Scene list. Both take a caller-allocated Object and only link/unlink it --
// the renderer never frees an Object or the arrays it points at. Not safe to
// call concurrently with render().
void add_object(Object* obj);
void remove_object(Object* obj);
bool render();
void clear_frame_buffer(bool keep_frame);
void renderer_resize(uint32_t win_w,uint32_t win_h);
void move_camera(double unit,Movement direction);
Camera get_camera_pos();
void* get_frame_buffer();
Object* get_current_object();


#endif

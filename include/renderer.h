#ifndef RENDERER
#define RENDERER
#include <stdint.h>
#include <stdio.h>
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <math.h>
#include "projection.h"
#include "wireframe.h"

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
uint64_t px;
uint64_t py;
uint32_t colour;
bool in_use;

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
    uint32_t colour;   // default colour; a vertex's own .colour overrides when non-zero


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
extern Object* objects;
extern void* frame;





void  render_init(Object* objs,uint64_t len,uint32_t win_w,uint32_t win_h,bool wirefame_mode);
bool  render(bool wirefame_mode);
void clear_frame_buffer();
void renderer_resize(uint32_t win_w,uint32_t win_h);
void move_camera(double unit,Movement direction);
Camera get_camera_pos();

#endif

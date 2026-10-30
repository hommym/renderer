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
extern uint64_t* connector_pool;
extern size_t connector_pool_len;
extern uint64_t* obj_end_idx;




void render_init(Object* objs,uint64_t len,uint32_t win_w,uint32_t win_h);

// Re-point the renderer at the caller's object array after it has been grown,
// reallocated or replaced. The renderer never grows, copies or frees it.
void set_objects(Object* objs,uint64_t len);
bool render();
void renderer_resize(uint32_t win_w,uint32_t win_h);
void move_camera(double unit,Movement direction);
Camera get_camera_pos();
void* get_frame_buffer();
Object* get_current_object(size_t index);
size_t get_objects_len();

// The frame buffer is a grid of atomic PixelCord pointers (PixelCord *_Atomic),
// NULL where nothing has been drawn. The cells are atomic; the PixelCords they
// point at are not, and live in these pools: one per rasterizer thread, rewound
// at the start of every render().
typedef struct PixelPool PixelPool;
PixelPool* get_pixel_pool(size_t index);
PixelCord* pixel_pool_take(PixelPool* pool);


#endif

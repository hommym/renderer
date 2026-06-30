#ifndef RENDERER
#define RENDERER
#include <stdint.h>
#include <stdio.h>
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <math.h>
#include "projection.h"
#include "wireframe.h"

typedef struct CameraPos{
double x; // starting positions
double y;
double z;

double x_end;
double y_end;
double z_end; // far plane distance
} CameraPos;

typedef struct Vectex
{
double x;
double y;
double z;
uint64_t px;
uint64_t py;
uint32_t colour;

} Vectex;



typedef struct Object
{
    Vectex* vertices;
    uint64_t len_of_vertices;
    uint64_t* connectors_sequence;
    uint64_t  len_of_connectors;


} Object;



extern uint32_t focal_len;
extern uint32_t screen_width;
extern uint32_t screen_hieght;
extern CameraPos camera_position;






void*  render(Object* objects,uint64_t len,uint32_t win_w,uint32_t win_h,bool wirefame_mode);
void release_frame_buffer(uint32_t* address);


#endif

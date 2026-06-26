#ifndef RENDERER
#define RENDERER
#include <stdint.h>
#include <stdio.h>
#include <SDL3/SDL.h>
#include <stdlib.h>
#include "projection.h"
#include "wireframe.h"

typedef struct Vectex
{
double x;
double y;
double z;
uint64_t px;
uint64_t py;

} Vectex;



typedef struct Object
{
    Vectex* vertices;
    uint64_t len_of_vertices;
    uint64_t* connectors_sequence;
    uint64_t  len_of_connectors;
    uint32_t colour;


} Object;



extern uint32_t focal_len;
extern uint32_t screen_width;
extern uint32_t screen_hieght;






void*  render(Object* objects,uint64_t len,uint32_t win_w,uint32_t win_h,bool wirefame_mode);
void release_frame_buffer(uint32_t* address);


#endif

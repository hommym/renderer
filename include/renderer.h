#ifndef RENDERER
#define RENDERER
#include <stdint.h>
#include <stdio.h>
#include <SDL3/SDL.h>
#include <stdlib.h>

typedef struct Vectex
{
int64_t x;
int64_t y;
int64_t z;
} Vectex;



typedef struct Object
{
    Vectex* vertices;
    uint64_t len_of_vertices;
    uint64_t* wireframe;
    uint64_t  len_of_wireframe;
    uint32_t colour;


} Object;



extern uint32_t focal_len;
extern uint32_t screen_width;
extern uint32_t screen_hieght;






uint32_t* render(Object* objects,uint64_t len);
void release_frame_buffer(uint32_t* address);


#endif

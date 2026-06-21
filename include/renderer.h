#ifndef RENDERER
#define RENDERER
#include <stdint.h>
#include <stdio.h>
#include <SDL3/SDL.h>
#include <stdlib.h>

typedef struct Vectex3d
{
int64_t x;
int64_t y;
int64_t z;
} Vectex3d;


typedef struct Vectex2d
{
int64_t x;
int64_t y;
} Vectex2d;

typedef struct Object3d
{
    Vectex3d* vertices;
    uint64_t len_of_vertices;
    uint64_t* wireframe;
    uint64_t  len_of_wireframe;
    uint32_t colour;


} Object3d;

typedef struct Object2d
{
    Vectex2d* vertices;
    uint64_t len_of_vertices;
    uint64_t* wireframe;
    uint64_t  len_of_wireframe;
    uint32_t colour;

} Object2d;

extern uint32_t focal_len;
extern uint32_t screen_width;
extern uint32_t screen_hieght;
extern uint32_t* frame_buffer; // memory representation of the screen 




void render2D(Object2d* objects,uint64_t len);
void render3D(Object3d* objects,uint64_t len);


#endif

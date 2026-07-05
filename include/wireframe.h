#ifndef WIREFRAME
#define WIREFRAME
#include <stdint.h>
#include <stdlib.h>

typedef struct Vectex Vectex;
void bresenhame_line_algo(uint64_t x1,uint64_t y1,uint64_t x2,uint64_t y2,double z1,double z2,Vectex* lines_arr);
#endif
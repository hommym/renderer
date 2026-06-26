#ifndef PROJECTION
#define PROJECTION

#include <stdio.h>
#include <stdint.h>

uint64_t perspective_projection(double xy,double z,int32_t focal,bool is_x,uint32_t win_h,uint32_t win_w);

#endif
#ifndef PROJECTION
#define PROJECTION

#include <stdio.h>
#include <stdint.h>

uint64_t perspective_projection(double xy,double z,int32_t focal,double start_p,double end_p);

#endif
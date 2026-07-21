#ifndef PROJECTION
#define PROJECTION

#include <stdio.h>
#include <stdint.h>

double perspective_projection(double xy,double z,double z_start,double focal,double start_p,double end_p,uint32_t screen_s);


#endif
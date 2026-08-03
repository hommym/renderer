#ifndef INTERPOL
#define INTERPOL
#include <stdlib.h>
#include <stdint.h>

double interpolate(double p1,double p2,size_t num_of_items,size_t item_num);
uint64_t interpolate_u( uint64_t p1,uint64_t p2,size_t num_of_items,size_t item_num);

#endif
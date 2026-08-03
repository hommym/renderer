#include "interpolation.h"





double interpolate(double p1,double p2,size_t num_of_items,size_t item_num){
// this method does interpolation for decimals
double ch_v=p1-p2;
return p1+(ch_v/num_of_items)*item_num;
}


uint64_t interpolate_u( uint64_t p1,uint64_t p2,size_t num_of_items,size_t item_num){
// this method does interpolation for whole numbers
int64_t ch_v=p1-p2;
return p1+(ch_v/num_of_items)*item_num;
}


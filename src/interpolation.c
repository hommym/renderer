#include "interpolation.h"





double interpolate(double p1,double p2,size_t num_of_items,size_t item_num){
// this method does interpolation for decimals
if(num_of_items==0)return p1; // a zero-length span has nowhere to walk to
double ch_v=p2-p1;
return p1+(ch_v*item_num/num_of_items);
}


uint64_t interpolate_u( uint64_t p1,uint64_t p2,size_t num_of_items,size_t item_num){
// this method does interpolation for whole numbers.
// multiply before dividing: dividing first throws the whole gradient away
// whenever the change is smaller than the span (255/300 == 0, so every step
// lands back on p1). keep every term signed too — item_num is a size_t, so a
// downward step promotes the negative change to unsigned and wraps mid-ramp.
if(num_of_items==0)return p1; // integer divide by zero would trap
int64_t ch_v=(int64_t)p2-(int64_t)p1;
return (uint64_t)((int64_t)p1+(ch_v*(int64_t)item_num)/(int64_t)num_of_items);
}

// per-channel interpolation for packed 0xAARRGGBB colour. treating the whole
// 32-bit word as one scalar (as interpolate_u does) makes subtraction bleed
// bits between channels; unpack, lerp each byte, repack.
uint32_t interpolate_colour(uint32_t c1,uint32_t c2,size_t num_of_items,size_t item_num){
uint8_t a1=(c1>>24)&0xFF, r1=(c1>>16)&0xFF, g1=(c1>>8)&0xFF, b1=c1&0xFF;
uint8_t a2=(c2>>24)&0xFF, r2=(c2>>16)&0xFF, g2=(c2>>8)&0xFF, b2=c2&0xFF;
uint8_t a=(uint8_t)interpolate_u(a1,a2,num_of_items,item_num);
uint8_t r=(uint8_t)interpolate_u(r1,r2,num_of_items,item_num);
uint8_t g=(uint8_t)interpolate_u(g1,g2,num_of_items,item_num);
uint8_t b=(uint8_t)interpolate_u(b1,b2,num_of_items,item_num);
return ((uint32_t)a<<24)|((uint32_t)r<<16)|((uint32_t)g<<8)|b;
}


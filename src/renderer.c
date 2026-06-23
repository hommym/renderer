#include "renderer.h"


uint32_t focal_len=24;
uint32_t screen_width; //max column on fram_buffer
uint32_t screen_hieght; // max row on frame_buffer









static uint32_t* create_frame_buffer(uint32_t win_w,uint32_t win_h){
return (uint32_t*) calloc((win_h*win_w),4);}




uint32_t*  render(Object* objects,uint64_t len,uint32_t win_w,uint32_t win_h){
uint32_t* frame_buffer= create_frame_buffer(win_w,win_h);
//code impl

return frame_buffer;
}

void release_frame_buffer(uint32_t* address){
    free(address);}
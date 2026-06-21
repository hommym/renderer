#include "renderer.h"
#include "win_i_o.h"



int main(){ 
Object2d obj={}; 
Object3d obj2={}; 

uint32_t* frame1= render2D(&obj,1);
uint32_t* frame2=  render3D(&obj2,1);

create_window();
set_up_event_handler();
release_frame_buffer(frame1);
release_frame_buffer(frame2);
return   EXIT_SUCCESS;  
}
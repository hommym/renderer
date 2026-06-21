#include "renderer.h"
#include "win_i_o.h"



int main(){ 
Object obj={}; 
uint32_t* frame1= render(&obj,1);


create_window();
set_up_event_handler();
release_frame_buffer(frame1);
return   EXIT_SUCCESS;  
}
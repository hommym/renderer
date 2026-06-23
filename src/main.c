#include "renderer.h"
#include "win_i_o.h"



int main(){ 
create_window();    
int w,h;    
get_window_size(&w,&h);
Object obj={}; 
uint32_t* frame1= render(&obj,1,w,h);



set_up_event_handler();
release_frame_buffer(frame1);
return   EXIT_SUCCESS;  
}
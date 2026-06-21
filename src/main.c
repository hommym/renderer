#include "renderer.h"
#include "win_i_o.h"



int main(){ 
Object2d obj={}; 
Object3d obj2={}; 

render2D(&obj,1);
render3D(&obj2,1);

create_window();
set_up_event_handler();
return   EXIT_SUCCESS;  
}
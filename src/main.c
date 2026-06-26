#include "renderer.h"
#include "win_i_o.h"
// #include "model_data.h"

int main(){
create_window();
int w,h;
get_window_size(&w,&h);

// Object model={
//     .vertices=model_vertices,
//     .len_of_vertices=MODEL_VERT_COUNT,
//     .connectors_sequence=model_connectors,
//     .len_of_connectors=MODEL_CONNECTOR_COUNT,
//     .colour=0xFFFFFFFF,
// };

Vectex cube_vertices[8]={
    {-3750,-3750,300},
    { 3750,-3750,300},
    { 3750, 3750,300},
    {-3750, 3750,300},
    {-3750,-3750,600},
    { 3750,-3750,600},
    { 3750, 3750,600},
    {-3750, 3750,600},
};

uint64_t cube_connectors[24]={
    0,1, 1,2, 2,3, 3,0,
    4,5, 5,6, 6,7, 7,4,
    0,4, 1,5, 2,6, 3,7,
};

Object cube={
    .vertices=cube_vertices,
    .len_of_vertices=8,
    .connectors_sequence=cube_connectors,
    .len_of_connectors=24,
    .colour=0xFFFFFFFF,
};

Object scene[1]={cube};
uint32_t* frame1= render(scene,1,w,h,true);

update_win(frame1);

set_up_event_handler();
release_frame_buffer(frame1);
return   EXIT_SUCCESS;
}

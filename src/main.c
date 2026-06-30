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
    { 100, 100, 50},
    {1400, 100, 50},
    {1400, 900, 50},
    { 100, 900, 50},
    { 100, 100,100},
    {1400, 100,100},
    {1400, 900,100},
    { 100, 900,100},
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

Vectex tri_right_vertices[3]={
    { 200, 600, 80},
    {1800, 600, 80},
    {1000, 900, 80},
};
uint64_t tri_right_connectors[6]={0,1, 1,2, 2,0};
Object tri_right={
    .vertices=tri_right_vertices,
    .len_of_vertices=3,
    .connectors_sequence=tri_right_connectors,
    .len_of_connectors=6,
    .colour=0xFF00FF00,
};

Vectex bar_below_vertices[4]={
    { 700, 300, 60},
    { 900, 300, 60},
    { 900,1300, 60},
    { 700,1300, 60},
};
uint64_t bar_below_connectors[8]={0,1, 1,2, 2,3, 3,0};
Object bar_below={
    .vertices=bar_below_vertices,
    .len_of_vertices=4,
    .connectors_sequence=bar_below_connectors,
    .len_of_connectors=8,
    .colour=0xFFFFFF00,
};

Object scene[3]={cube};
uint32_t* frame1= render(scene,1,w,h,true);

update_win(frame1);

set_up_event_handler();
release_frame_buffer(frame1);
return   EXIT_SUCCESS;
}

#include "renderer.h"
#include "win_i_o.h"

int main(){
create_window();
int w,h;
get_window_size(&w,&h);

// cube centered on world (0,0), half-size 100, front face z=500, back face z=600.
// vertex colours chosen so interpolation across triangles is visible.
Vectex cube_vertices[8]={
    {.x=-100,.y=-100,.z=500,.colour=0xFFFF0000}, // 0 red
    {.x= 100,.y=-100,.z=500,.colour=0xFF00FF00}, // 1 green
    {.x= 100,.y= 100,.z=500,.colour=0xFF0000FF}, // 2 blue
    {.x=-100,.y= 100,.z=500,.colour=0xFFFFFF00}, // 3 yellow
    {.x=-100,.y=-100,.z=600,.colour=0xFFFF00FF}, // 4 magenta
    {.x= 100,.y=-100,.z=600,.colour=0xFF00FFFF}, // 5 cyan
    {.x= 100,.y= 100,.z=600,.colour=0xFFFFFFFF}, // 6 white
    {.x=-100,.y= 100,.z=600,.colour=0xFFFF8800}, // 7 orange
};

// 6 faces * 2 triangles = 12 tris = 36 indices.
// each quad split on diagonal a-c: (a,b,c) + (a,c,d).
uint64_t cube_connectors[36]={
    0,1,2, 0,2,3,   // front  (z=500)
    5,4,7, 5,7,6,   // back   (z=600)
    4,0,3, 4,3,7,   // left   (x=-100)
    1,5,6, 1,6,2,   // right  (x= 100)
    4,5,1, 4,1,0,   // bottom (y=-100)
    3,2,6, 3,6,7,   // top    (y= 100)
};

Object cube={
    .vertices=cube_vertices,
    .len_of_vertices=8,
    .connectors_sequence=cube_connectors,
    .len_of_connectors=36,
};

Object scene[1]={cube};
render_init(scene,1,w,h,false);
render(false);
update_win((PixelCord*)get_frame_buffer());
set_up_event_handler();
return   EXIT_SUCCESS;
}

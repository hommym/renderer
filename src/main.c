#include "renderer.h"
#include "win_i_o.h"
#include "model_data.h"
#include "point_cloud_data.h"

int main(){
create_window();
int w,h;
get_window_size(&w,&h);

Object model={
    .vertices=model_vertices,
    .len_of_vertices=MODEL_VERT_COUNT,
    .connectors_sequence=model_connectors,
    .len_of_connectors=MODEL_CONNECTOR_COUNT,
    .colour=0xFF000000,
};

Vectex cube_vertices[8]={
    {.x= 100,.y=100,.z= 50,.colour=0xFF000000},
    {.x=1400,.y=100,.z= 50,.colour=0xFF000000},
    {.x=1400,.y=900,.z= 50,.colour=0xFF000000},
    {.x= 100,.y=900,.z= 50,.colour=0xFF000000},
    {.x= 100,.y=100,.z=100,.colour=0xFF000000},
    {.x=1400,.y=100,.z=100,.colour=0xFF000000},
    {.x=1400,.y=900,.z=100,.colour=0xFF000000},
    {.x= 100,.y=900,.z=100,.colour=0xFF000000},
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
};

Vectex tri_right_vertices[3]={
    {.x= 200,.y=600,.z=80,.colour=0xFF000000},
    {.x=1800,.y=600,.z=80,.colour=0xFF000000},
    {.x=1000,.y=900,.z=80,.colour=0xFF000000},
};
uint64_t tri_right_connectors[6]={0,1, 1,2, 2,0};
Object tri_right={
    .vertices=tri_right_vertices,
    .len_of_vertices=3,
    .connectors_sequence=tri_right_connectors,
    .len_of_connectors=6,
};

Vectex bar_below_vertices[4]={
    {.x=700,.y= 300,.z=60,.colour=0xFF000000},
    {.x=900,.y= 300,.z=60,.colour=0xFF000000},
    {.x=900,.y=1300,.z=60,.colour=0xFF000000},
    {.x=700,.y=1300,.z=60,.colour=0xFF000000},
};
uint64_t bar_below_connectors[8]={0,1, 1,2, 2,3, 3,0};
Object bar_below={
    .vertices=bar_below_vertices,
    .len_of_vertices=4,
    .connectors_sequence=bar_below_connectors,
    .len_of_connectors=8,
};

// partial_view: rectangle straddling the right window border.
// left edge in-view; right edge (world x=2500) is outside the frustum
// (x_end=1500) AND projects past the screen right edge (px~1590 at z=50).
// Expect: cyan rectangle with the right side clipped off.
Vectex partial_view_vertices[4]={
    {.x=1200,.y=400,.z=50,.colour=0xFF000000},
    {.x=2500,.y=400,.z=50,.colour=0xFF000000},
    {.x=2500,.y=800,.z=50,.colour=0xFF000000},
    {.x=1200,.y=800,.z=50,.colour=0xFF000000},
};
uint64_t partial_view_connectors[8]={0,1, 1,2, 2,3, 3,0};
Object partial_view={
    .vertices=partial_view_vertices,
    .len_of_vertices=4,
    .connectors_sequence=partial_view_connectors,
    .len_of_connectors=8,
};

Object tree_pointcloud={
    .vertices=point_cloud_vertices,
    .len_of_vertices=POINTCLOUD_VERT_COUNT,
    .connectors_sequence=point_cloud_connectors,
    .len_of_connectors=POINTCLOUD_CONNECTOR_COUNT,   // 0 -> point-cloud mode in render()
    .colour=0xFF000000,
};

Object scene[4]={model,tree_pointcloud};
render_init(scene,2,w,h,true);
render(true);

// update_win((Vectex*)frame);

set_up_event_handler();
return   EXIT_SUCCESS;
}

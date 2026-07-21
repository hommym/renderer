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

// centered on world (0,0). half-size 100, depth 100. at z=500 with focal_l~=866
// this projects to roughly a 350x350 front face on a 1500x1000 window.
Vectex cube_vertices[8]={
    {.x=-100,.y=-100,.z=500,.colour=0xFF000000},
    {.x= 100,.y=-100,.z=500,.colour=0xFF000000},
    {.x= 100,.y= 100,.z=500,.colour=0xFF000000},
    {.x=-100,.y= 100,.z=500,.colour=0xFF000000},
    {.x=-100,.y=-100,.z=600,.colour=0xFF000000},
    {.x= 100,.y=-100,.z=600,.colour=0xFF000000},
    {.x= 100,.y= 100,.z=600,.colour=0xFF000000},
    {.x=-100,.y= 100,.z=600,.colour=0xFF000000},
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

// upward-pointing triangle centered on world (0,0). apex above, base below.
Vectex tri_right_vertices[3]={
    {.x=-150,.y= 100,.z=400,.colour=0xFF000000},
    {.x= 150,.y= 100,.z=400,.colour=0xFF000000},
    {.x=   0,.y=-100,.z=400,.colour=0xFF000000},
};
uint64_t tri_right_connectors[6]={0,1, 1,2, 2,0};
Object tri_right={
    .vertices=tri_right_vertices,
    .len_of_vertices=3,
    .connectors_sequence=tri_right_connectors,
    .len_of_connectors=6,
};

// tall thin bar centered on world (0,0).
Vectex bar_below_vertices[4]={
    {.x=-30,.y=-150,.z=400,.colour=0xFF000000},
    {.x= 30,.y=-150,.z=400,.colour=0xFF000000},
    {.x= 30,.y= 150,.z=400,.colour=0xFF000000},
    {.x=-30,.y= 150,.z=400,.colour=0xFF000000},
};
uint64_t bar_below_connectors[8]={0,1, 1,2, 2,3, 3,0};
Object bar_below={
    .vertices=bar_below_vertices,
    .len_of_vertices=4,
    .connectors_sequence=bar_below_connectors,
    .len_of_connectors=8,
};

// partial_view: rectangle whose right edge extends past the visible screen.
// at z=400 with focal_l~=866, screen half-width in world units is
// ~750*400/866 = 346, so a right edge at x=500 will project past px=1499
// and get clamped by projection.c.
// left edge visible (x=100 -> px~866), right edge clipped.
Vectex partial_view_vertices[4]={
    {.x= 100,.y=-100,.z=400,.colour=0xFF000000},
    {.x= 500,.y=-100,.z=400,.colour=0xFF000000},
    {.x= 500,.y= 100,.z=400,.colour=0xFF000000},
    {.x= 100,.y= 100,.z=400,.colour=0xFF000000},
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

// model and tree_pointcloud are baked in include/model_data.h and
// include/point_cloud_data.h with world coords around (~450, ~750, ~350) and
// (~1050, ...) respectively, which was correct under the old screen-center
// frustum. re-import them through tools/*.py with a translation to origin
// before adding them back to the scene.
Object scene[5]={cube,tri_right,bar_below,tree_pointcloud,model};
render_init(scene,5,w,h,true);
render(true);
update_win((PixelCord*)frame);
set_up_event_handler();
return   EXIT_SUCCESS;
}

#include "renderer.h"
#include "win_i_o.h"
#include "utils.h"
#include "mesh.h"

#define CUBE_COLOUR 0xFFCC3344   // one flat colour for the whole cube

// The loaded model is scaled so its longest axis spans this many world units and
// recentred here. Model files carry their own units and origin -- the tree GLB is
// roughly 1 unit tall -- and this camera is a fixed pinhole at the world origin,
// so without a fit pass a model is either a speck or swallows the screen.
// note the spelling: the file is appartement.glb, not appartment.glb
#define MODEL_PATH "/home/arthur-herberth/Documents/models/appartement/source/appartement.glb"

#define MODEL_EXTENT   420.0
#define MODEL_CENTRE_X 180.0     // right of centre; the cube sits on the left
#define MODEL_CENTRE_Y 0.0
#define MODEL_CENTRE_Z 550.0

int main(void){
create_window();
int w,h;
get_window_size(&w,&h);

// cube half-size 100, front face z=500, back face z=600, pushed left of centre so
// it clears a loaded model. every vertex carries the same colour: there is no
// lighting, so a solid mesh renders as a flat silhouette either way.
Vectex cube_vertices[8]={
    {.x=-350,.y=-100,.z=500,.colour=CUBE_COLOUR}, // 0
    {.x=-150,.y=-100,.z=500,.colour=CUBE_COLOUR}, // 1
    {.x=-150,.y= 100,.z=500,.colour=CUBE_COLOUR}, // 2
    {.x=-350,.y= 100,.z=500,.colour=CUBE_COLOUR}, // 3
    {.x=-350,.y=-100,.z=600,.colour=CUBE_COLOUR}, // 4
    {.x=-150,.y=-100,.z=600,.colour=CUBE_COLOUR}, // 5
    {.x=-150,.y= 100,.z=600,.colour=CUBE_COLOUR}, // 6
    {.x=-350,.y= 100,.z=600,.colour=CUBE_COLOUR}, // 7
};

// 6 faces * 2 triangles = 12 tris = 36 indices.
// each quad split on diagonal a-c: (a,b,c) + (a,c,d).
uint64_t cube_connectors[36]={
    0,1,2, 0,2,3,   // front  (z=500)
    5,4,7, 5,7,6,   // back   (z=600)
    4,0,3, 4,3,7,   // left   (x=-350)
    1,5,6, 1,6,2,   // right  (x=-150)
    4,5,1, 4,1,0,   // bottom (y=-100)
    3,2,6, 3,6,7,   // top    (y= 100)
};

Object cube={
    .vertices=cube_vertices,
    .len_of_vertices=8,
    .connectors_sequence=cube_connectors,
    .len_of_connectors=36,
};

// mesh_load allocates; mesh_free at the end releases it. the renderer only reads
// the arrays, so a loaded Object is handed over exactly like the static cube.
// a load failure is not fatal -- the cube still renders, which makes it obvious
// the model is the thing that went wrong rather than the renderer.
Object model={0};
bool have_model=false;
{
    MeshResult r=mesh_load(MODEL_PATH,&model);
    if(r!=MESH_OK)printf("could not load %s: %s\n",MODEL_PATH,mesh_result_string(r));
    else if(!mesh_fit_to_view(&model,MODEL_EXTENT,MODEL_CENTRE_X,MODEL_CENTRE_Y,MODEL_CENTRE_Z,true)){
        printf("%s has no usable geometry to fit\n",MODEL_PATH);
        mesh_free(&model);
    }
    else{
        have_model=true;
        printf("loaded %s: %llu vertices, %llu triangles\n",MODEL_PATH,
               (unsigned long long)model.len_of_vertices,
               (unsigned long long)(model.len_of_connectors/3));
    }
}

Object scene[2]={cube};
uint64_t scene_len=1;
if(have_model)scene[scene_len++]=model;

int cores=get_number_of_cores();
printf("cores: %d\n",cores);
render_init(scene,scene_len,(uint32_t)w,(uint32_t)h);
render();
update_win((PixelCord*)get_frame_buffer());
set_up_event_handler();
mesh_free(&model);
return   EXIT_SUCCESS;
}

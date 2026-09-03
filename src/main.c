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
#define MODEL_PATH "/home/arthur-herberth/Documents/models/corrupted_archangel_six_wings_boss.glb"

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

// The rasterizer reads every pixel's colour out of the object's texture, so an
// Object built by hand needs one exactly as much as a loaded one does -- a NULL
// here is a null dereference on the first span. One texel is enough to keep the
// cube the flat reference shape it has always been: every (u,v) lands on it.
uint32_t cube_texture[1]={CUBE_COLOUR};

Object cube={
    .vertices=cube_vertices,
    .len_of_vertices=8,
    .connectors_sequence=cube_connectors,
    .len_of_connectors=36,
    .texture=cube_texture,
    .texture_width=1,
    .texture_height=1,
};

// One call: path in, renderer-ready objects out. It detects the format, parses
// it, splits it by material, decodes its textures and scales/centres it for the
// view. model.objects is the flat array render() walks and model.len is its
// length -- a multi-material file just comes back with more than one entry.
//
// a load failure is not fatal -- the cube still renders, which makes it obvious
// the model is the thing that went wrong rather than the renderer.
Model model={0};
{
    MeshResult r=mesh_import(MODEL_PATH,MODEL_EXTENT,MODEL_CENTRE_X,MODEL_CENTRE_Y,MODEL_CENTRE_Z,&model);
    if(r!=MESH_OK)printf("could not import %s: %s\n",MODEL_PATH,mesh_result_string(r));
    else{
        uint64_t tris=0;
        for(uint64_t i=0;i<model.len;i++)tris+=model.objects[i].len_of_connectors/3;
        printf("imported %s: %llu objects, %zu textures, %llu triangles\n",MODEL_PATH,
               (unsigned long long)model.len,model.texture_count,(unsigned long long)tris);
    }
}

// the renderer walks one flat array, so the cube and the imported objects share
// a block. to drop the cube: copy from index 0 and set scene_len to model.len.
uint64_t scene_len=model.len;
Object* scene=malloc((size_t)scene_len*sizeof *scene);
if(scene==NULL){
    printf("could not allocate the scene array\n");
    return EXIT_FAILURE;
}
// scene[0]=cube;
for(uint64_t i=0;i<model.len;i++)scene[0+i]=model.objects[i];

int cores=get_number_of_cores();
printf("cores: %d\n",cores);
render_init(scene,scene_len,(uint32_t)w,(uint32_t)h);
render();
update_win((PixelCord*)get_frame_buffer());
set_up_event_handler();
free(scene);
// releases every object's arrays and every distinct texture. never mesh_free an
// object out of a Model: they share textures, so it would double free.
mesh_model_free(&model);
return   EXIT_SUCCESS;
}

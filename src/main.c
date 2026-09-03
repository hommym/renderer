#include "renderer.h"
#include "win_i_o.h"
#include "utils.h"
#include "mesh.h"
#include "ui.h"
#include <string.h>

// Where to look for models, and which one to open with. Both are defaults now
// rather than the only option: argv[1] overrides the directory and argv[2] the
// startup model, and once the window is up `L` opens the browser and loads any
// of them without a rebuild.
#define MODEL_DIR     "3dmodels"
#define STARTUP_MODEL "3dmodels/woman_seated_v12.glb"

// The loaded model is scaled so its longest axis spans this many world units and
// recentred here. Model files carry their own units and origin, and this camera
// is a pinhole at the world origin, so without a fit pass a model is either a
// speck or swallows the screen. ui.c uses the same numbers, so a swapped model
// lands exactly where the startup one did.
#define MODEL_EXTENT   420.0
#define MODEL_CENTRE_X 0.0
#define MODEL_CENTRE_Y 0.0
#define MODEL_CENTRE_Z 550.0

static const char* base_name(const char* path){
const char* b=path;
for(const char* p=path;*p;p++)if(*p=='/'||*p=='\\')b=p+1;
return b;
}

int main(int argc,char** argv){
const char* dir  = argc>1 ? argv[1] : MODEL_DIR;
const char* start= argc>2 ? argv[2] : STARTUP_MODEL;

create_window();
int w,h;
get_window_size(&w,&h);

// One call: path in, renderer-ready objects out. It detects the format, parses
// it, splits it by material, decodes its textures and scales/centres it for the
// view. model.objects is the flat array render() walks and model.len is its
// length -- a multi-material file just comes back with more than one entry.
//
// A load failure is not fatal. The window still opens on an empty scene, and
// the browser can load something that works, which beats exiting with a message
// the user has to go and read in a terminal.
Model model={0};
MeshResult r=mesh_import(start,MODEL_EXTENT,MODEL_CENTRE_X,MODEL_CENTRE_Y,MODEL_CENTRE_Z,&model);
if(r!=MESH_OK){
    printf("could not import %s: %s\n",start,mesh_result_string(r));
}else{
    uint64_t tris=0;
    for(uint64_t i=0;i<model.len;i++)tris+=model.objects[i].len_of_connectors/3;
    printf("imported %s: %llu objects, %zu textures, %llu triangles\n",start,
           (unsigned long long)model.len,model.texture_count,(unsigned long long)tris);
    if(model.primitives_skipped)
        printf("  (%llu primitives skipped: not triangles, or morph targets)\n",
               (unsigned long long)model.primitives_skipped);
}

printf("cores: %d\n",get_number_of_cores());

// The renderer only reads the object array; ui.c owns it from here, because the
// swap has to free the old model at exactly one moment relative to set_objects.
render_init(model.objects,model.len,(uint32_t)w,(uint32_t)h);
ui_init(dir);
ui_adopt_model(model,base_name(start));

printf("controls: drag to look, WASD/arrows to move, wheel to zoom,\n"
       "          L or F1 for the model browser, C toggles forced back-face culling\n");

render();
update_win(get_frame_buffer());
set_up_event_handler();

// ui_shutdown releases the model still loaded -- never mesh_free on an object
// out of a Model, they share their textures with each other.
ui_shutdown();
renderer_shutdown();
return EXIT_SUCCESS;
}

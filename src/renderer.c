#include "renderer.h"
#include "projection.h"
#include "wireframe.h"
#include "utils.h"
#include "interpolation.h"
#include "rasterization.h"




// double camera.focal_l=30;
uint32_t screen_width; //max column on fram_buffer
uint32_t screen_hieght; // max row on frame_buffer
static Camera camera={
0.0,
0.0,
0.0,
0.0,
0.0,
15000.0, //default far plane distance
0.0,           // focal_l recomputed in setup_camera
1.047197551,   // v_fov (const): 60 deg in rad
0.0,           // h_fov recomputed in setup_camera
};



static Object* objects=NULL;
static Object* object;
_Atomic size_t triangle_tracker;
static size_t objects_len=0;
static void* frame=NULL;
static int num_core=0; // number of cores the running system has


Object* get_current_object(){
    return object;
}

static void setup_camera(){

    // v_fov fixes the vertical angle; focal_l follows from screen height,
    // and h_fov falls out of the resulting aspect ratio.
    camera.focal_l=screen_hieght/(2*tan(0.5*camera.v_fov));
    camera.h_fov=2*atan(screen_width/(2*camera.focal_l));

    // calculate x and y points of the extremes of the fov
    double x_center=camera.x+(camera.x_end-camera.x)/2;
    double x_offset=(screen_width/2)*(camera.z_end-camera.z)/camera.focal_l;
    

    double y_center=camera.y+(camera.y_end-camera.y)/2;
    double y_offset=(screen_hieght/2)*(camera.z_end-camera.z)/camera.focal_l;
   

    camera.x_end=x_center +x_offset;
    camera.x=x_center-x_offset;

    camera.y_end=y_center+y_offset;
    camera.y=y_center-y_offset;
    printf("X =%f\n",camera.x);
    printf("X end=%f\n",camera.x_end);

    printf("Y =%f\n",camera.y);
    printf("Y end=%f\n",camera.y_end);



}


static void create_frame_buffer(uint32_t win_w,uint32_t win_h){
frame= calloc((win_h*win_w),sizeof(PixelCord));
}



Camera get_camera_pos(){
return camera;
}




void* get_frame_buffer(){
    return frame;
}

// Point the renderer at the caller's object array. The renderer only reads it:
// growing, reallocating and freeing the array stay the caller's job, so after a
// realloc the caller calls this again with the new base pointer and length.
// Not safe to call concurrently with render().
void set_objects(Object* objs,uint64_t len){
objects=objs;
objects_len=objs==NULL?0:len;   // a NULL array has no elements, whatever len says
}


void render_init(Object* objs,uint64_t len,uint32_t win_w,uint32_t win_h){
// needs to be called once to initialise the renderer
num_core=get_number_of_cores(); // loading the number of core on system to know number of threads to spawn
num_core=num_core<0?1:num_core-1;
create_frame_buffer(win_w,win_h);
set_objects(objs,len);
screen_width=win_w;
screen_hieght=win_h;
setup_camera();
}


bool render(){
if(frame==NULL)return false;

PixelCord (*frame_buffer)[screen_width]= (PixelCord (*)[screen_width])frame;   


for(size_t x=0;x<objects_len;x++){
Object obj=objects[x];

// no connectors -> paint each visible vertex as one pixel
if(obj.len_of_connectors==0){
    size_t w=obj.texture_width;
    uint32_t (*texture)[w]=(uint32_t (*)[w]) (obj).texture;
    for(size_t v=0;v<obj.len_of_vertices;v++){
        Vectex pt=obj.vertices[v];
        PixelCord pc={.z=pt.z,.is_visible=false,.in_use=false,.u=pt.u,.v=pt.v};
        if(!is_vectex_visible(pt,&pc)) continue;
        pc.px=perspective_projection(pt.x,pt.z,camera.z,camera.focal_l,camera.x,camera.x_end,screen_width);
        pc.py=perspective_projection(pt.y,pt.z,camera.z,camera.focal_l,camera.y,camera.y_end,screen_hieght);
        PixelCord point0=frame_buffer[(uint64_t)pc.py][(uint64_t)pc.px];
        if(point0.in_use && point0.z<pt.z) continue;
        pc.in_use=true;
        size_t pc_row=(size_t)(obj.texture_height*pc.v);
        size_t pc_col=(size_t)(obj.texture_width*pc.u);
        pc.colour=texture[pc_row][pc_col];
        frame_buffer[(uint64_t)pc.py][(uint64_t)pc.px]=pc;
    }
    continue;
}

object=objects+x;    
atomic_store(&triangle_tracker,0);
    pthread_t threads[num_core==0?1:num_core]; // Array to hold thread IDs

    // 1. Create multiple threads in a loop
    for (long i = 0; i < num_core && num_core>1; i++) {
        // We cast 'i' to a void* directly to avoid race conditions with local memory
        if (pthread_create(&threads[i], NULL,rasterization_worker, (void*)i) != 0) {
            perror("Failed to create thread");
            return false;
        }
    }

    //using main thread too in rasterization process
    rasterization_worker(NULL);
    // 2. Join multiple threads in a separate loop
    printf("Waiting for threads which are still working to finish to finish...\n");
    for (int i = 0; i < num_core && num_core>1; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            perror("Failed to join thread");
            return false;
        }
    }
    printf("Rasterization Process Done\n");


}

return true;
}

void clear_frame_buffer(bool keep_frame){
    if(!keep_frame)free(frame);
    if(screen_hieght!=0 && screen_width!=0)create_frame_buffer(screen_width,screen_hieght);
    else frame=NULL;
}

void renderer_resize(uint32_t win_w,uint32_t win_h){
    screen_width=win_w;
    screen_hieght=win_h;
    setup_camera();
    clear_frame_buffer(false);
}

void move_camera(double unit,Movement direction){
    switch (direction)
    {
    case MOV_LEFT:
        camera.x-=unit;
        camera.x_end-=unit;
        break;
    case MOV_RIGHT:
        camera.x+=unit;
        camera.x_end+=unit;
        break;
    case MOV_UP:
        camera.y+=unit;
        camera.y_end+=unit;
        break;
    case MOV_DOWN:
        camera.y-=unit;
        camera.y_end-=unit;
        break;
    case MOV_FORWARD:
        camera.z+=unit;
        camera.z_end+=unit;
        break;
    default:
        //backewards
        camera.z-=unit;
        camera.z_end-=unit;
        break;
    }   

}
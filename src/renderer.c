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
_Atomic size_t triangle_tracker;
uint64_t* connector_pool=NULL;
size_t connector_pool_len;
uint64_t* obj_end_idx=NULL;

static size_t objects_len=0;
static void* frame=NULL;
static void* frame1=NULL;
static void* frame2=NULL;
static int num_core=0; // number of cores the running system has




Object* get_current_object(size_t index){
    return &objects[index];
}
size_t get_objects_len(){
    return objects_len;
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
// a grid of atomic PixelCord pointers. calloc zero-fills, so every cell starts
// NULL: nothing drawn there yet.
frame1= calloc((win_h*win_w),sizeof(uint64_t _Atomic));
frame2= calloc((win_h*win_w),sizeof(uint64_t _Atomic));
frame=frame1;
}

static void* clear_frame_buffer(void* args ){
   // resets the frame by setting every cell back to NULL
    if(frame!=frame1){
        //reset frame1 memory section
        memset(frame1,0,(screen_hieght*screen_width)*sizeof(uint64_t));
    }
    else{
        //reset frame2 memory section
         memset(frame2,0,(screen_hieght*screen_width)*sizeof(uint64_t));
    }

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
if(frame==frame1)frame=frame2;
else frame=frame1;
pthread_t frame_clear_thread;
if (pthread_create(&frame_clear_thread, NULL,clear_frame_buffer, NULL)) {
            perror("Failed to create thread for memory reset\n");
            return false;
        }


for (size_t i = 0; i < objects_len; i++)connector_pool_len+=objects[i].len_of_connectors;
connector_pool=calloc(connector_pool_len,sizeof(uint64_t));   
obj_end_idx=calloc(objects_len,sizeof(uint64_t));
uint64_t pool_tracker=0;   
for (size_t i = 0; i < objects_len; i++){
    Object obj=objects[i];
    for (size_t x = 0; x < obj.len_of_connectors; x++)
    {
        connector_pool[pool_tracker]=obj.connectors_sequence[x];
        pool_tracker++;
    }
    obj_end_idx[i]=pool_tracker-1;
    
}


pthread_t threads[num_core==0?1:num_core]; // Array to hold thread IDs
    // 1. Create multiple threads in a loop
    for (long i = 0; i < num_core && num_core>1; i++) {
        // We cast 'i' to a void* directly to avoid race conditions with local memory
        if (pthread_create(&threads[i], NULL,rasterization_worker, (void*)i) != 0) {
            perror("Failed to create thread");
            return false;
        }
    }


//using main thread too in rasterization process. the workers are 0..num_core-1,
//so the main thread takes index num_core.
rasterization_worker((void*)(long)num_core);

  // 2. Join multiple threads in a separate loop
    // printf("Waiting for threads which are still working to finish to finish...\n");
    for (int i = 0; i < num_core && num_core>1; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            perror("Failed to join thread");
            return false;
        }
    }
free(connector_pool);
free(obj_end_idx);
connector_pool_len=0;
triangle_tracker=0;
pthread_join(frame_clear_thread, NULL);
return true;
}



void renderer_resize(uint32_t win_w,uint32_t win_h){
    screen_width=win_w;
    screen_hieght=win_h;
    setup_camera();
    if(frame1!=NULL && frame2!=NULL)
    {
        free(frame1);
        free(frame2);
    }
    create_frame_buffer(win_w,win_h);
    
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
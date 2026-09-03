#include "renderer.h"
#include "projection.h"
#include "wireframe.h"
#include "utils.h"
#include "interpolation.h"
#include "rasterization.h"
#include "transform.h"
#include <string.h>





// double camera.focal_l=30;
// The far plane the camera starts with, and the one camera_reset() puts back.
#define CAMERA_FAR_DEFAULT 15000.0

uint32_t screen_width; //max column on fram_buffer
uint32_t screen_hieght; // max row on frame_buffer
static Camera camera={
0.0,
0.0,
0.0,
0.0,
0.0,
CAMERA_FAR_DEFAULT, //default far plane distance
0.0,           // focal_l recomputed in setup_camera
1.047197551,   // v_fov (const): 60 deg in rad
0.0,           // h_fov recomputed in setup_camera
0.0,           // yaw:   looking straight down +z
0.0,           // pitch: level
};



static Object* objects=NULL;
static Object* object;
_Atomic size_t triangle_tracker;
static size_t objects_len=0;
// Two frame buffers, allocated once per window size and owned entirely by this
// file. render() draws into the back one and swaps at the end; everyone else
// gets a const pointer to the front one and cannot allocate or release either.
//
// Before this they were freed and calloc'd again every single frame. At 1500x1000
// that is a 60MB release and a 60MB zeroed allocation per frame, and it handed
// out a pointer that the next clear silently invalidated -- anything that held
// on to the result of get_frame_buffer() across a frame was reading freed memory.
// ---- no frame buffer locks -------------------------------------------------
//
// There used to be 256 cache-line-padded spinlocks here, because work was handed
// out by TRIANGLE and two threads therefore landed on the same pixel routinely --
// and the depth test is a read-modify-write that has to be indivisible.
//
// Work is now handed out by row BAND instead (see rasterization.h), so a pixel
// has exactly one writer for the whole frame and there is nothing left to
// serialise. That deleted the locks, which were 28% of the profile, and made the
// frame bit-exact reproducible at the same time: the depth ties that used to be
// broken by arrival order are now broken by source-triangle order.

static PixelCord* buffers[2]={NULL,NULL};
static size_t buffer_cells=0;
static int back=0;                 // buffers[back] is the one being drawn into
static int num_core=0; // number of cores the running system has

static uint32_t worker_count=1;
static pthread_barrier_t frame_bar;
static bool bar_ready=false;

typedef struct WorkerArg { uint32_t tid; } WorkerArg;

static Object* frame_objects=NULL;
static size_t  frame_objects_len=0;



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


static void frame_buffers_release(void){
free(buffers[0]);
free(buffers[1]);
buffers[0]=NULL;
buffers[1]=NULL;
buffer_cells=0;
}

// The only place either buffer is ever allocated. Called at init and on resize,
// never per frame.
static bool frame_buffers_alloc(uint32_t win_w,uint32_t win_h){
frame_buffers_release();
if(win_w==0||win_h==0)return false;
size_t cells=(size_t)win_w*(size_t)win_h;
if(win_w&&cells/win_w!=win_h)return false;                 // the multiply wrapped
if(cells>SIZE_MAX/sizeof(PixelCord))return false;
buffers[0]=calloc(cells,sizeof(PixelCord));
buffers[1]=calloc(cells,sizeof(PixelCord));
if(!buffers[0]||!buffers[1]){
    // one of the two is no use on its own, and a half-allocated pair would let
    // the swap hand out NULL every other frame
    frame_buffers_release();
    return false;
}
buffer_cells=cells;
back=0;
return true;
}



Camera get_camera_pos(){
return camera;
}




const PixelCord* get_frame_buffer(){
    // the front buffer: the last frame render() finished and swapped in
    return buffers[back^1];
}

PixelCord* renderer_back_buffer(){
    return buffers[back];
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
num_core=num_core<1?1:num_core;
if(num_core>64)num_core=64;
worker_count=(uint32_t)num_core;
if(bar_ready)pthread_barrier_destroy(&frame_bar);
pthread_barrier_init(&frame_bar,NULL,worker_count);
bar_ready=true;
frame_buffers_alloc(win_w,win_h);
set_objects(objs,len);
screen_width=win_w;
screen_hieght=win_h;
setup_camera();
}


// ---- the frame worker pool -------------------------------------------------
//
// Threads are created ONCE PER FRAME rather than once per object. A 61-material
// model was paying 61 x 7 pthread_create+join every frame, which at ~25us each
// is 20ms of pure bookkeeping before a single pixel is drawn.
//
// Within a frame the workers run in lockstep through three barriers per chunk:
// setup, then fill, then reset. The barriers are what let the fill pass assume
// every setup record it might read is already written.

static void frame_work(uint32_t tid){
    uint32_t chunk=rasterization_chunk_size();
    for(size_t oi=0;oi<frame_objects_len;oi++){
        Object* obj=&frame_objects[oi];
        if(obj->len_of_connectors<3)continue;      // point clouds are drawn before the pool starts
        uint64_t ntri=obj->len_of_connectors/3;
        for(uint64_t base=0;base<ntri;base+=chunk){
            uint64_t hi=base+chunk;
            if(hi>ntri)hi=ntri;

            rasterization_setup_pass(obj,base,hi,tid);
            pthread_barrier_wait(&frame_bar);      // every record is now written

            rasterization_fill_pass(obj);
            pthread_barrier_wait(&frame_bar);      // every band is now filled

            rasterization_chunk_reset(tid);
            pthread_barrier_wait(&frame_bar);      // bins are empty for the next chunk
        }
    }
}

// Workers park here until the main thread knows how many of them actually
// started. Everything downstream -- the barrier's participant count and the
// rasterizer's triangle split -- is sized from that number, and a worker that
// began before it was known would be splitting the work N ways while only M
// threads ever arrive at the barrier.
static pthread_mutex_t gate_m=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  gate_c=PTHREAD_COND_INITIALIZER;
static bool gate_open=false;
static bool gate_run=false;      // false means "stand down, the frame is off"

static void* frame_worker(void* a){
    pthread_mutex_lock(&gate_m);
    while(!gate_open)pthread_cond_wait(&gate_c,&gate_m);
    bool run=gate_run;
    pthread_mutex_unlock(&gate_m);
    if(run)frame_work((uint32_t)(uintptr_t)a);
    return NULL;
}

bool render(){
if(buffers[0]==NULL||buffers[1]==NULL)return false;

// rebuild the camera basis once for the whole frame. every vertex is rotated
// into view space with it, and the rasterization threads only read it.
view_refresh();
rasterization_frame_begin();

// Wipe the back buffer rather than reallocating it. .in_use is the occupancy
// bit, so zeroing is what makes every cell empty again; the front buffer is
// untouched and keeps showing the last finished frame while this one is drawn.
memset(buffers[back],0,buffer_cells*sizeof(PixelCord));

PixelCord (*frame_buffer)[screen_width]=(PixelCord (*)[screen_width])buffers[back];

// point-cloud objects first, on this thread alone, before any worker exists.
for(size_t x=0;x<objects_len;x++){
    Object obj=objects[x];
    if(obj.len_of_connectors!=0)continue;
    size_t w=obj.texture_width;
    uint32_t (*texture)[w]=(uint32_t (*)[w])obj.texture;
    for(size_t v=0;v<obj.len_of_vertices;v++){
        Vectex pt=view_apply(obj.vertices[v]);
        PixelCord pc={.z=pt.z,.is_visible=false,.in_use=false,.u=pt.u,.v=pt.v};
        if(!is_vectex_visible(pt,&pc))continue;
        pc.px=perspective_projection(pt.x,pt.z,camera.z,camera.focal_l,camera.x,camera.x_end,screen_width);
        pc.py=perspective_projection(pt.y,pt.z,camera.z,camera.focal_l,camera.y,camera.y_end,screen_hieght);
        if(pc.px<0||pc.px>=screen_width||pc.py<0||pc.py>=screen_hieght)continue;
        PixelCord point0=frame_buffer[(uint64_t)pc.py][(uint64_t)pc.px];
        if(point0.in_use&&point0.z<pt.z)continue;
        pc.in_use=true;
        size_t pc_row=(size_t)(obj.texture_height*pc.v);
        size_t pc_col=(size_t)(obj.texture_width*pc.u);
        if(pc_row>=obj.texture_height)pc_row=obj.texture_height-1;
        if(pc_col>=obj.texture_width)pc_col=obj.texture_width-1;
        pc.colour=texture[pc_row][pc_col];
        frame_buffer[(uint64_t)pc.py][(uint64_t)pc.px]=pc;
    }
}

// anything with triangles goes through the pool
bool any_tris=false;
for(size_t x=0;x<objects_len;x++)if(objects[x].len_of_connectors>=3){any_tris=true;break;}
if(any_tris){
    frame_objects=objects;
    frame_objects_len=objects_len;

    // Spawn FIRST, size everything SECOND. pthread_create can fail under
    // resource pressure, and the worker count is not a preference: the
    // rasterizer splits each chunk into exactly that many contiguous ranges and
    // the barrier waits for exactly that many arrivals. Sizing from the number
    // we hoped for and then running with fewer means the missing ranges are
    // never set up at all -- their triangles simply do not appear -- and their
    // bins are never reset, so stale entries from an earlier chunk get drawn
    // again. Hence the gate: the workers do not touch either until the count
    // they were sized from is the count that exists.
    pthread_mutex_lock(&gate_m);
    gate_open=false;
    gate_run=false;
    pthread_mutex_unlock(&gate_m);

    pthread_t threads[64];
    uint32_t spawned=0;
    for(uint32_t i=1;i<worker_count;i++){
        if(pthread_create(&threads[spawned],NULL,frame_worker,(void*)(uintptr_t)(spawned+1))!=0)break;
        spawned++;
    }
    uint32_t live=spawned+1;           // the workers that exist, this thread included

    bool ready=rasterization_pool_init(live);
    if(ready){
        pthread_barrier_destroy(&frame_bar);
        pthread_barrier_init(&frame_bar,NULL,live);
    }

    // release the gate whatever happened, or the spawned threads never exit
    pthread_mutex_lock(&gate_m);
    gate_open=true;
    gate_run=ready;
    pthread_cond_broadcast(&gate_c);
    pthread_mutex_unlock(&gate_m);

    if(ready)frame_work(0);            // this thread is worker 0

    for(uint32_t i=0;i<spawned;i++)pthread_join(threads[i],NULL);

    // put the barrier back to the size render_init chose, so the next frame
    // starts from the same place this one did
    if(ready&&live!=worker_count){
        pthread_barrier_destroy(&frame_bar);
        pthread_barrier_init(&frame_bar,NULL,worker_count);
    }
    if(!ready)return false;
}

// present: the buffer just drawn becomes the one get_frame_buffer() hands out.
back^=1;
return true;
}

// Everything render_init() and renderer_resize() allocate, released. Nothing
// else in the program owns any of it, so this is the whole teardown.
void renderer_shutdown(void){
    if(bar_ready){ pthread_barrier_destroy(&frame_bar); bar_ready=false; }
    rasterization_pool_release();
    frame_buffers_release();
    objects=NULL;
    objects_len=0;
}

void renderer_resize(uint32_t win_w,uint32_t win_h){
    screen_width=win_w;
    screen_hieght=win_h;
    setup_camera();
    // a new size means new buffers; this is the only other place they are made
    frame_buffers_alloc(win_w,win_h);
    // the band count is derived from the screen height, so the bins go too
    rasterization_pool_release();
}

// Slide the whole camera box by (dx,dy,dz). Both ends of every axis move
// together, so the box keeps its shape and only the eye at its centre travels.
static void translate_camera(double dx,double dy,double dz){
    camera.x+=dx;      camera.x_end+=dx;
    camera.y+=dy;      camera.y_end+=dy;
    camera.z+=dz;      camera.z_end+=dz;
}

void move_camera(double unit,Movement direction){
    // the camera's own axes, so "forward" means where it is pointing rather
    // than +z. at zero rotation these are the world axes and this behaves
    // exactly as the old switch did.
    double fwd[3],right[3];
    camera_axes(fwd,right,NULL);

    switch (direction)
    {
    case MOV_LEFT:
        translate_camera(-right[0]*unit,-right[1]*unit,-right[2]*unit);
        break;
    case MOV_RIGHT:
        translate_camera(right[0]*unit,right[1]*unit,right[2]*unit);
        break;
    // up and down deliberately ignore the camera's own vertical and use the
    // world's. tying them to pitch means looking down and pressing up flies
    // you into the floor, which is disorienting rather than useful.
    case MOV_UP:
        translate_camera(0.0,-unit,0.0);     // +y is down
        break;
    case MOV_DOWN:
        translate_camera(0.0,unit,0.0);
        break;
    case MOV_FORWARD:
        translate_camera(fwd[0]*unit,fwd[1]*unit,fwd[2]*unit);
        break;
    default:
        //backewards
        translate_camera(-fwd[0]*unit,-fwd[1]*unit,-fwd[2]*unit);
        break;
    }   

}

// Put the camera back exactly where render_init() left it: at the world origin,
// looking straight down +z. A model swap needs this -- mesh_import fits every
// model into the same box, so the new one is always where the old one was, but
// the camera may have been flown a long way off, and arriving at a blank screen
// reads as a failed load rather than a moved viewer.
//
// The setup_camera() call is load-bearing: the fov extents are derived from the
// CENTRE of the current box, so zeroing both ends re-centres it on the origin
// and setup_camera then re-expands them to what startup produced.
void camera_reset(void){
    camera.x=0.0; camera.x_end=0.0;
    camera.y=0.0; camera.y_end=0.0;
    camera.z=0.0; camera.z_end=CAMERA_FAR_DEFAULT;
    camera.yaw=0.0; camera.pitch=0.0;
    setup_camera();
}

void rotate_camera(double d_yaw,double d_pitch){
    if(!isfinite(d_yaw)||!isfinite(d_pitch))return;
    camera.yaw+=d_yaw;
    camera.pitch+=d_pitch;

    // keep yaw in [-pi,pi) so it cannot drift into the range where a double
    // stops resolving small mouse deltas. spelled out rather than M_PI, which
    // is not defined under -std=c23.
    const double PI=3.14159265358979323846;
    const double TWO_PI=2.0*PI;
    camera.yaw=fmod(camera.yaw+PI,TWO_PI);
    if(camera.yaw<0.0)camera.yaw+=TWO_PI;
    camera.yaw-=PI;

    // stop just short of vertical. exactly straight up leaves the horizontal
    // heading undefined, and the view rolls as it passes through.
    const double LIMIT=1.55334303;   // 89 degrees
    if(camera.pitch>LIMIT)camera.pitch=LIMIT;
    if(camera.pitch<-LIMIT)camera.pitch=-LIMIT;
}
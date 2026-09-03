#include "renderer.h"
#include "projection.h"
#include "wireframe.h"
#include "utils.h"
#include "interpolation.h"
#include "rasterization.h"
#include "transform.h"
#include <string.h>




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
// ---- frame buffer row locks ----------------------------------------------
//
// Work is handed out by triangle, so two threads routinely land on the same
// pixel, and the depth test there is a read-modify-write:
//
//     existing = frame_buffer[y][x];   if(closer) frame_buffer[y][x] = pixel;
//
// Unsynchronised that loses updates -- both threads read the same `existing`,
// both conclude they are in front, and the second write clobbers the first -- and
// it tears, because a PixelCord is 40 bytes and no store that wide is atomic.
//
// A lock per pixel is impossible (1.5M of them) and one lock for the whole
// buffer would remove the point of threading. A lock per ROW BAND is the middle:
// the scanline pass writes one row at a time, so a whole triangle-row is a
// single acquire, and two threads only wait on each other when their rows
// collide modulo FRAME_LOCK_BANDS.
//
// Padded to a cache line each. Packed together, locking two different bands
// would still bounce the same line between cores and reintroduce most of the
// contention the banding exists to avoid.
#define FRAME_LOCK_BANDS 256u      // power of two, so the index is a mask
#define FRAME_LOCK_LINE  64u

typedef struct RowLock {
    atomic_flag held;
    char pad[FRAME_LOCK_LINE-sizeof(atomic_flag)];
} RowLock;

static RowLock row_locks[FRAME_LOCK_BANDS];

static PixelCord* buffers[2]={NULL,NULL};
static size_t buffer_cells=0;
static int back=0;                 // buffers[back] is the one being drawn into
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




// A spinlock, not a mutex: the critical section is one row of one triangle, so
// the wait is shorter than the round trip into the kernel that a sleeping lock
// would pay. There is never more than one thread per core here, so nobody spins
// waiting for a holder that has been descheduled.
void frame_row_lock(size_t row){
RowLock* l=&row_locks[row&(FRAME_LOCK_BANDS-1u)];
while(atomic_flag_test_and_set_explicit(&l->held,memory_order_acquire)){
#if defined(__x86_64__)||defined(__i386__)
    __builtin_ia32_pause();   // stop hammering the cache line while waiting
#endif
}
}

void frame_row_unlock(size_t row){
atomic_flag_clear_explicit(&row_locks[row&(FRAME_LOCK_BANDS-1u)].held,memory_order_release);
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
for(size_t i=0;i<FRAME_LOCK_BANDS;i++)atomic_flag_clear(&row_locks[i].held);
num_core=get_number_of_cores(); // loading the number of core on system to know number of threads to spawn
num_core=num_core<0?1:num_core-1;
frame_buffers_alloc(win_w,win_h);
set_objects(objs,len);
screen_width=win_w;
screen_hieght=win_h;
setup_camera();
}


bool render(){
if(buffers[0]==NULL||buffers[1]==NULL)return false;

// rebuild the camera basis once for the whole frame. every vertex is rotated
// into view space with it, and the rasterization threads only read it.
view_refresh();

// Wipe the back buffer rather than reallocating it. .in_use is the occupancy
// bit, so zeroing is what makes every cell empty again; the front buffer is
// untouched and keeps showing the last finished frame while this one is drawn.
memset(buffers[back],0,buffer_cells*sizeof(PixelCord));

PixelCord (*frame_buffer)[screen_width]= (PixelCord (*)[screen_width])buffers[back];   


for(size_t x=0;x<objects_len;x++){
Object obj=objects[x];

// no connectors -> paint each visible vertex as one pixel. no row lock needed:
// this path runs on the calling thread and finishes before any worker is
// spawned, so nothing else is touching the buffer while it writes.
if(obj.len_of_connectors==0){
    size_t w=obj.texture_width;
    uint32_t (*texture)[w]=(uint32_t (*)[w]) (obj).texture;
    for(size_t v=0;v<obj.len_of_vertices;v++){
        // into the camera's frame first, exactly like the triangle path. a copy,
        // never in place: obj.vertices is the caller's and is walked every frame.
        Vectex pt=view_apply(obj.vertices[v]);
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

// present: the buffer just drawn becomes the one get_frame_buffer() hands out.
// A pointer swap, so nothing is copied and the frame that was on screen a moment
// ago becomes the next back buffer.
back^=1;
return true;
}



void renderer_resize(uint32_t win_w,uint32_t win_h){
    screen_width=win_w;
    screen_hieght=win_h;
    setup_camera();
    // a new size means new buffers; this is the only other place they are made
    frame_buffers_alloc(win_w,win_h);
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
#include "renderer.h"





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



Object* objects;
static size_t objects_len=0;
static void* frame=NULL;



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

static bool is_vectex_visible(Vectex point,PixelCord* pxcord_p){
bool is_z_in_view=point.z>=camera.z && point.z<=camera.z_end;
if(!is_z_in_view)return false;

double y_center=camera.y+(camera.y_end-camera.y)/2;
double y_offset=(screen_hieght/2)*(point.z-camera.z)/camera.focal_l;


double x_center=camera.x+(camera.x_end-camera.x)/2;    
double x_offset=(screen_width/2)*(point.z-camera.z)/camera.focal_l;


bool is_x_in_view=point.x>=(x_center-x_offset) && point.x<(x_center+x_offset);
bool is_y_in_view=point.y>=(y_center-y_offset) && point.y<(y_center+y_offset);

(*pxcord_p).is_visible=is_x_in_view && is_y_in_view;


return (*pxcord_p).is_visible;
}


Camera get_camera_pos(){
return camera;   
}


void* get_frame_buffer(){
    return frame;
}

void render_init(Object* objs,uint64_t len,uint32_t win_w,uint32_t win_h,bool wirefame_mode){
// needs to be called once to initialise the renderer  
create_frame_buffer(win_w,win_h);    
objects=objs;
objects_len=len;
screen_width=win_w;
screen_hieght=win_h;
setup_camera();
}


bool render(bool wirefame_mode){
if(frame==NULL)return false;

PixelCord (*frame_buffer)[screen_width]= (PixelCord (*)[screen_width])frame;   

if(wirefame_mode){
for(size_t x=0;x<objects_len;x++){
Object obj=objects[x];

// no connectors -> paint each visible vertex as one pixel
if(obj.len_of_connectors==0){
    for(size_t v=0;v<obj.len_of_vertices;v++){
        Vectex pt=obj.vertices[v];
        PixelCord pc={.z=pt.z,.is_visible=false,.in_use=false,.colour=pt.colour};
        if(!is_vectex_visible(pt,&pc)) continue;
        pc.px=perspective_projection(pt.x,pt.z,camera.z,camera.focal_l,camera.x,camera.x_end,screen_width);
        pc.py=perspective_projection(pt.y,pt.z,camera.z,camera.focal_l,camera.y,camera.y_end,screen_hieght);
        PixelCord point0=frame_buffer[(uint64_t)pc.py][(uint64_t)pc.px];
        if(point0.in_use && point0.z<pt.z) continue;
        pc.in_use=true;
        frame_buffer[(uint64_t)pc.py][(uint64_t)pc.px]=pc;
    }
    continue;
}

for(size_t a=0;a<obj.len_of_connectors;a+=2){
    Vectex v1=obj.vertices[obj.connectors_sequence[a]];
    PixelCord p1={.z=v1.z,.is_visible=false,.in_use=false,.colour=v1.colour};

    Vectex v2=obj.vertices[obj.connectors_sequence[a+1]];
    PixelCord p2={.z=v2.z,.is_visible=false,.in_use=false,.colour=v2.colour};

    
    if(!(is_vectex_visible(v1,&p1) || is_vectex_visible(v2,&p2)))continue;

    p1.px=perspective_projection(v1.x,v1.z,camera.z,camera.focal_l,camera.x,camera.x_end,screen_width);
    p1.py=perspective_projection(v1.y,v1.z,camera.z,camera.focal_l,camera.y,camera.y_end,screen_hieght);


    p2.px=perspective_projection(v2.x,v2.z,camera.z,camera.focal_l,camera.x,camera.x_end,screen_width);
    p2.py=perspective_projection(v2.y,v2.z,camera.z,camera.focal_l,camera.y,camera.y_end,screen_hieght);

    int64_t ch_x= p1.px-p2.px;
    int64_t ch_y= p1.py-p2.py;
  
    if(ch_x<0)ch_x*=-1;
    if(ch_y<0)ch_y*=-1;

    size_t lines_len=ch_x>=ch_y?(ch_x+1):(ch_y+1);
    if(lines_len==1)continue;
    PixelCord* lines_arr=calloc(lines_len,sizeof(PixelCord)); // an array for storing vectex pointer to be coloured to form the line
    bresenhame_line_algo(p1,p2,lines_arr);


    for(size_t i=0;i<lines_len;i++){
        PixelCord point=lines_arr[i];
        if(!point.is_visible)continue;
        PixelCord point0=frame_buffer[(uint64_t)point.py][(uint64_t)point.px];

        if(point0.in_use && point0.z<point.z) continue;
        frame_buffer[(uint64_t)point.py][(uint64_t)point.px]=point;
    }
    free(lines_arr);
}

}


}

else{
// rasterization
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
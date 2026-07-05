#include "renderer.h"
uint32_t focal_len=600;
uint32_t screen_width; //max column on fram_buffer
uint32_t screen_hieght; // max row on frame_buffer
static CameraPos camera_position={
0.0,
0.0,
0.0,
0.0,
0.0,
1000.0, //default far plane distance
};

static bool is_init_called=false;
Object* objects;
static size_t objects_len;
void* frame;






static void create_frame_buffer(uint32_t win_w,uint32_t win_h){
frame= calloc((win_h*win_w),sizeof(Vectex));
}

static void calc_screen_cordinate(double win_size,double start_cord,double* end_cord_p){
    // calculates x or y cordinate values for the screen in world space(ie 3D)
    double diff=start_cord-(*end_cord_p);
    if(diff>win_size){
    *end_cord_p-=fabs(diff-win_size);
    }
    else if(diff<win_size){
    *end_cord_p+=fabs(diff-win_size);
    }
    else *end_cord_p=diff+start_cord;

}

static bool is_vectex_visible(Vectex point){
bool is_x_in_view=point.x>=camera_position.x && point.x<=camera_position.x_end;
bool is_y_in_view=point.y>=camera_position.y && point.y<=camera_position.y_end;
bool is_z_in_view=point.z>=camera_position.z && point.z<=camera_position.z_end;

if(is_x_in_view && is_y_in_view && is_z_in_view) return true;

return false;
}

void render_init(Object* objs,uint64_t len,uint32_t win_w,uint32_t win_h,bool wirefame_mode){
// needs to be called once to initialise the who renderer
if(is_init_called) return;   
create_frame_buffer(win_w,win_h);    
objects=objs;
objects_len=len;
screen_width=win_w;
screen_hieght=win_h;
calc_screen_cordinate(win_w,camera_position.x,&(camera_position.x_end));
calc_screen_cordinate(win_h,camera_position.y,&(camera_position.y_end));
is_init_called=true;
}


bool render(bool wirefame_mode){

if(!is_init_called){
 printf("Warning: render_init was never executed and is required for render func");
 return false;   
}

Vectex (*frame_buffer)[screen_width]= (Vectex (*)[screen_width])frame;   

if(wirefame_mode){
for(size_t x=0;x<objects_len;x++){
Object obj=objects[x];

// point-cloud path: no connectors -> paint each visible vertex as one pixel
if(obj.len_of_connectors==0){
    for(size_t v=0;v<obj.len_of_vertices;v++){
        Vectex pt=obj.vertices[v];
        if(!is_vectex_visible(pt)) continue;
        pt.px=perspective_projection(pt.x,pt.z,focal_len,camera_position.x,camera_position.x_end);
        pt.py=perspective_projection(pt.y,pt.z,focal_len,camera_position.y,camera_position.y_end);
        if(!(pt.py<screen_hieght && pt.px<screen_width)) continue;
        Vectex point0=frame_buffer[pt.py][pt.px];
        if(point0.in_use && point0.z<pt.z) continue;
        pt.in_use=true;
        frame_buffer[pt.py][pt.px]=pt;
    }
    continue;
}

for(size_t a=0;a<obj.len_of_connectors;a+=2){
    Vectex v1=obj.vertices[obj.connectors_sequence[a]];
    Vectex v2=obj.vertices[obj.connectors_sequence[a+1]];
    
    if(!(is_vectex_visible(v1) || is_vectex_visible(v2)))continue;

    v1.px=perspective_projection(v1.x,v1.z,focal_len,camera_position.x,camera_position.x_end);
    v1.py=perspective_projection(v1.y,v1.z,focal_len,camera_position.y,camera_position.y_end);


    v2.px=perspective_projection(v2.x,v2.z,focal_len,camera_position.x,camera_position.x_end);
    v2.py=perspective_projection(v2.y,v2.z,focal_len,camera_position.y,camera_position.y_end);

    int64_t ch_x= v1.px-v2.px;
    int64_t ch_y= v1.py-v2.py;
  
    if(ch_x<0)ch_x*=-1;
    if(ch_y<0)ch_y*=-1;

    size_t lines_len=ch_x>=ch_y?(ch_x+1):(ch_y+1);
    if(lines_len==1)continue;
    Vectex* lines_arr=calloc(lines_len,sizeof(Vectex)); // an array for storing vectex pointer to be coloured to form the line
    bresenhame_line_algo(v1.px,v1.py,v2.px,v2.py,v1.z,v2.z,lines_arr);


    for(size_t i=0;i<lines_len;i++){
        Vectex point=lines_arr[i];
        Vectex point0= point.py<screen_hieght && point.px<screen_width? frame_buffer[point.py][point.px]:(Vectex){0.0};

        if(point0.in_use && point0.z<point.z) continue;

        point.colour=v1.colour | v2.colour;
        if(point.py<screen_hieght && point.px<screen_width)frame_buffer[point.py][point.px]=point;
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

void clear_frame_buffer(){
    free(frame);
    if(screen_hieght!=0 && screen_width!=0)create_frame_buffer(screen_width,screen_hieght);
    else frame=NULL;
}

void renderer_resize(uint32_t win_w,uint32_t win_h){
    // if(!is_init_called) return;
    screen_width=win_w;
    screen_hieght=win_h;
    // recompute frustum bounds from camera + new window size.
    // this bypasses calc_screen_cordinate's accumulator, so repeated
    // resizes stay correct.
    camera_position.x_end=camera_position.x+win_w;
    camera_position.y_end=camera_position.y+win_h;
    create_frame_buffer(win_w,win_h);
}

void move_camera(double unit,Movement direction){
    switch (direction)
    {
    case MOV_LEFT:
        camera_position.x-=unit;
        camera_position.x_end-=unit;
        break;
    case MOV_RIGHT:
        camera_position.x+=unit;
        camera_position.x_end+=unit;
        break;
    case MOV_UP:
        camera_position.y+=unit;
        camera_position.y_end+=unit;
        break;
    case MOV_DOWN:
        camera_position.y-=unit;
        camera_position.y_end-=unit;
        break;
    case MOV_FORWARD:
        camera_position.z+=unit;
        camera_position.z_end+=unit;
        break;
    default:
        //backewards
        camera_position.z-=unit;
        camera_position.z_end-=unit;
        break;
    }

    // clear frame buffer
    clear_frame_buffer();

}
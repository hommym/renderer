#include "renderer.h"
uint32_t focal_len=24;
uint32_t screen_width; //max column on fram_buffer
uint32_t screen_hieght; // max row on frame_buffer
CameraPos camera_position={
0.0,
0.0,
0.0,
0.0,
0.0,
1000.0, //default far plane distance
};








static void* create_frame_buffer(uint32_t win_w,uint32_t win_h){
return calloc((win_h*win_w),4);
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


void * render(Object* objects,uint64_t len,uint32_t win_w,uint32_t win_h,bool wirefame_mode){
uint32_t (*frame_buffer)[win_w]= (uint32_t (*)[win_w]) create_frame_buffer(win_w,win_h);
calc_screen_cordinate(win_w,camera_position.x,&(camera_position.x_end));
calc_screen_cordinate(win_h,camera_position.y,&(camera_position.y_end));



// project 3D points to 2D
for(uint64_t i=0;i<len;i++){
    for(uint64_t a=0;a<objects[i].len_of_vertices;a++){
       Vectex point=objects[i].vertices[a];
       bool is_x_in_view=point.x>=camera_position.x && point.x<=camera_position.x_end;
       bool is_y_in_view=point.y>=camera_position.y && point.y<=camera_position.y_end;
       bool is_z_in_view=point.z>=camera_position.z && point.z<=camera_position.z_end;

       if(!(is_x_in_view && is_y_in_view && is_z_in_view))continue; 

        

        objects[i].vertices[a].px=perspective_projection(objects[i].vertices[a].x,objects[i].vertices[a].z,focal_len,camera_position.x,camera_position.x_end);
        objects[i].vertices[a].py=perspective_projection(objects[i].vertices[a].y,objects[i].vertices[a].z,focal_len,camera_position.y,camera_position.y_end);
        uint64_t px=objects[i].vertices[a].px;
        uint64_t py=objects[i].vertices[a].py;
        // frame_buffer[py][px]=objects[i].colour;
    }
    


}

if(wirefame_mode){
for(size_t x=0;x<len;x++){
Object obj=objects[x];

for(size_t a=0;a<obj.len_of_connectors;a+=2){
    Vectex v1=obj.vertices[obj.connectors_sequence[a]];
    Vectex v2=obj.vertices[obj.connectors_sequence[a+1]];
    if((v1.px==0 && v1.py==0) || (v2.px==0 && v2.py==0))continue;
    int64_t ch_x=v1.px-v2.px;
    int64_t ch_y=v1.py-v2.py;
  

    if(ch_x<0)ch_x*=-1;
    if(ch_y<0)ch_y*=-1;
    size_t lines_len=ch_x>=ch_y?((ch_x+1)*2):((ch_y+1)*2);
    uint64_t* lines_arr=calloc(lines_len,sizeof(uint64_t)); // an array for storing the pixel cordinates to be coloured
    bresenhame_line_algo(v1.px,v1.py,v2.px,v2.py,lines_arr,lines_len);

    for(size_t i=0;i<lines_len;i+=2){
        frame_buffer[lines_arr[i+1]][lines_arr[i]]=v1.colour|v2.colour;
    }
    free(lines_arr);
}

}


}
//code impl

return frame_buffer;
}

void release_frame_buffer(uint32_t* address){
    free(address);
}
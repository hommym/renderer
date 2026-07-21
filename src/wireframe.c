#include "wireframe.h"
#include "renderer.h"






void bresenhame_line_algo(double x1,double y1,double x2,double y2,double z1,double z2,PixelCord* lines_arr){
size_t lines_arr_pointer=0;
Camera camera=get_camera_pos();
double x,y,x0,y0;
int8_t step_x,step_y;
double slope=0.0;
double error=0.0;
double z=0.0;
double z_step=0.0;
//calculating change in x and y
int64_t ch_x=x2-x1;
int64_t ch_y=y2-y1;

if(ch_x<0)ch_x*=-1;
if(ch_y<0)ch_y*=-1;

if(ch_x>=ch_y){
// moving along x
slope=((double) ch_y)/(double) ch_x;
if(slope<0)slope*=-1;

if(x1<=x2){
x=x1;
x0=x2;
y=y1;
step_y=y1<=y2?1:-1;
// walk starts near v1, so z ramps v1.z -> v2.z
z=z1;
z_step=(z2-z1)/(double)ch_x;
}
else{
x=x2;
x0=x1;
y=y2;
step_y=y2<=y1?1:-1;
// walk starts near v2, so z ramps v2.z -> v1.z
z=z2;
z_step=(z1-z2)/(double)ch_x;
}

for(;x<=x0;x++){
//save vectex to be coloured
bool is_visible= (x>=0&&x<screen_width) && (y>=0&&y<screen_hieght) && z>=camera.z&&z<=camera.z_end;
lines_arr[lines_arr_pointer]=(PixelCord){.px=x,.py=y,.z=z,.in_use=true,.is_visible=is_visible};
lines_arr_pointer++;
error+=slope;
if(error>=1.0){
    y+=step_y;
    error-=1.0;
}
z+=z_step;
}



}



else{
// moving along y
slope=((double)ch_x)/(double)ch_y;
if(slope<0)slope*=-1;

if(y1<=y2){
y=y1;
y0=y2;
x=x1;
step_x=x1<=x2?1:-1;
// walk starts near v1, so z ramps v1.z -> v2.z
z=z1;
z_step=(z2-z1)/(double)ch_y;
}
else{
y=y2;
y0=y1;
x=x2;
step_x=x2<=x1?1:-1;
// walk starts near v2, so z ramps v2.z -> v1.z
z=z2;
z_step=(z1-z2)/(double)ch_y;
}


for(;y<=y0;y++){
//save vectex to be coloured 
bool is_visible= (x>=0&&x<screen_width) && (y>=0&&y<screen_hieght) && z>=camera.z&&z<camera.z_end;
lines_arr[lines_arr_pointer]= (PixelCord){.px=x,.py=y,.z=z,.in_use=true,.is_visible=is_visible};
lines_arr_pointer++;
error+=slope;
if(error>=1.0){
    x+=step_x;
    error-=1.0;

}
z+=z_step;
}



}


}



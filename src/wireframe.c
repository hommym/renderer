#include "wireframe.h"
#include "renderer.h"
#include "interpolation.h"






void bresenhame_line_algo(PixelCord p1,PixelCord p2,PixelCord* lines_arr){
double x1=p1.px, y1=p1.py;
double x2=p2.px, y2=p2.py; 
size_t lines_arr_pointer=0;
Camera camera=get_camera_pos();
double x,y,x0,y0;
int8_t step_x,step_y;
double slope=0.0;
// how far the true line has drifted from the row being drawn. the crossing
// below fires at half a pixel of drift, not a full one, so a step lands on
// whichever row is nearer -- rounding instead of flooring. it still subtracts
// a whole 1.0, which recentres the accumulator on the new row.
//
// firing at 1.0 instead makes the minor axis lag by up to a full pixel and
// puts the last crossing exactly on the comparison boundary, so a drift of
// one ulp eats a whole row: the strip stops short of its endpoint and the
// scanline fill loses that row.
double error=0.0;
double z_start=0.0;
double z_end=0.0;
uint32_t colour_start=0;
uint32_t colour_end=0;
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
z_start=p1.z;
z_end=p2.z;
colour_start=p1.colour;
colour_end=p2.colour;
}
else{
x=x2;
x0=x1;
y=y2;
step_y=y2<=y1?1:-1;
z_start=p2.z;
z_end=p1.z;
colour_start=p2.colour;
colour_end=p1.colour;

}

for(;x<=x0;x++){
//save vectex to be coloured
// the strip holds ch_x+1 pixels, indexed 0..ch_x, so the walked distance IS
// lines_arr_pointer. adding 1 pushed pixel 0 off the start point and ran the
// last pixel past p2 entirely.
double z=interpolate(z_start,z_end,ch_x,lines_arr_pointer);
uint32_t colour=interpolate_colour(colour_start,colour_end,ch_x,lines_arr_pointer);
bool is_visible= (x>=0&&x<screen_width) && (y>=0&&y<screen_hieght) && z>=camera.z&&z<=camera.z_end;
lines_arr[lines_arr_pointer]=(PixelCord){.px=x,.py=y,.z=z,.in_use=true,.is_visible=is_visible,.colour=colour};
lines_arr_pointer++;
error+=slope;
if(error>=0.5){
    y+=step_y;
    error-=1.0;
}
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
z_start=p1.z;
z_end=p2.z;
colour_start=p1.colour;
colour_end=p2.colour;
}
else{
y=y2;
y0=y1;
x=x2;
step_x=x2<=x1?1:-1;
z_start=p2.z;
z_end=p1.z;
colour_start=p2.colour;
colour_end=p1.colour;
}


for(;y<=y0;y++){
//save vectex to be coloured
double z=interpolate(z_start,z_end,ch_y,lines_arr_pointer);
uint32_t colour=interpolate_colour(colour_start,colour_end,ch_y,lines_arr_pointer);
bool is_visible= (x>=0&&x<screen_width) && (y>=0&&y<screen_hieght) && z>=camera.z&&z<=camera.z_end;
lines_arr[lines_arr_pointer]= (PixelCord){.px=x,.py=y,.z=z,.in_use=true,.is_visible=is_visible,.colour=colour};
lines_arr_pointer++;
error+=slope;
if(error>=0.5){
    x+=step_x;
    error-=1.0;

}
}



}


}



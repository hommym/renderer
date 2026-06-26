#include "wireframe.h"






void bresenhame_line_algo(uint64_t x1,uint64_t y1,uint64_t x2,uint64_t y2,uint64_t* lines_arr,size_t lines_len){
size_t lines_arr_pointer=0;
uint64_t x,y,x0,y0;
int8_t step_x,step_y;   
double slope=0.0;
double error=0.0;
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

}
else{
x=x2;
x0=x1;
y=y2;
step_y=y2<=y1?1:-1;
}

for(;x<=x0;x++){

//colour pixel
// if(y<=win_h && x<=win_w)frame[y][x]=color;
// else printf("3D points cannot be projected with the current screen dimensions");  
lines_arr[lines_arr_pointer]=x;
lines_arr[lines_arr_pointer+1]=y;
lines_arr_pointer+=2;    
error+=slope;
if(error>=1.0){
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
}
else{
y=y2;
y0=y1;
x=x2;
step_x=x2<=x1?1:-1;
}


for(;y<=y0;y++){
// color pixel 
// if(y<=win_h && x<=win_w)frame[y][x]=color;
// else printf("3D points cannot be projected with the current screen dimensions");    
lines_arr[lines_arr_pointer]=x;
lines_arr[lines_arr_pointer+1]=y;
lines_arr_pointer+=2;
error+=slope;    
if(error>=1.0){
    x+=step_x;
    error-=1.0;
    
} 



}



}


}



#include "projection.h"





double perspective_projection(double xy,double z,double z_start,double focal,double start_p,double end_p,uint32_t screen_s){

double centre     = start_p + (end_p - start_p)/2.0;   // world center
double pix_centre = (screen_s)/2.0;             // screen center pixel
double offset;

offset=xy-centre;

if((z-z_start) != 0){
    offset = (offset * focal) / (z-z_start);
}

double pix_point=pix_centre+offset;

return pix_point;  
}






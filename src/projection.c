#include "projection.h"





uint64_t perspective_projection(double xy,double z,int32_t focal,double start_p,double end_p){
double centre     = start_p + (end_p - start_p)/2.0;   // world center
double pix_centre = (end_p - start_p)/2.0;             // screen center pixel
double offset;

offset=xy-centre;

if(z != 0){
    offset = (offset * focal) / z;
}

double pix_point=pix_centre+offset;

// cliping to screen in pixel space
if (pix_point<0)pix_point=0.0;
else if(pix_point>(end_p-start_p)) pix_point=end_p-start_p;

return (uint64_t) pix_point;
}






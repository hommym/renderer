#include "projection.h"





uint64_t perspective_projection(double xy,double z,int32_t focal,double start_p,double end_p){
double centre     = start_p + (end_p - start_p)/2.0;   // world center
double pix_centre = (end_p - start_p)/2.0;             // screen center pixel
double offset     = xy - centre;
if(z != 0){
    offset = (offset * focal) / z;
}
return (uint64_t)(pix_centre + offset);
}






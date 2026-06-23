#include "projection.h"





uint64_t perspective_projection(int64_t xy,int64_t z,int32_t focal,bool is_x,uint32_t win_h,uint32_t win_w){
if(z){
xy=(xy*focal)/z;
}

if (is_x)xy+=(win_w/2);
else xy=(win_h/2)-xy;

return (uint64_t) xy;
}






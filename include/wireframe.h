#ifndef WIREFRAME
#define WIREFRAME
#include <stdint.h>
#include <stdlib.h>

typedef struct PixelCord PixelCord;
typedef struct Camera Camera;
void bresenhame_line_algo(double x1,double y1,double x2,double y2,double z1,double z2,PixelCord* lines_arr);
Camera get_camera_pos();

extern uint32_t screen_width;
extern uint32_t screen_hieght;


#endif
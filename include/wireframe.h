#ifndef WIREFRAME
#define WIREFRAME
#include <stdint.h>
#include <stdlib.h>

typedef struct PixelCord PixelCord;
typedef struct Camera Camera;
void bresenhame_line_algo(PixelCord p1,PixelCord p2,PixelCord* lines_arr);
Camera get_camera_pos();

extern uint32_t screen_width;
extern uint32_t screen_hieght;


#endif
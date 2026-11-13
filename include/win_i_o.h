
#ifndef WIN_I_O
#define WIN_I_O

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <SDL3/SDL.h>


extern SDL_Window* win;
// frame_buffer: w*h packed cells, high 32 bits = depth (float bits),
// low 32 bits = ARGB colour, 0 = never written
void update_win(uint64_t _Atomic *frame_buffer);
void create_window();
void set_up_event_handler();
void get_window_size(int* w,int* h);
#endif
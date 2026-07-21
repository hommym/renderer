
#ifndef WIN_I_O
#define WIN_I_O

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <SDL3/SDL.h>

typedef struct PixelCord PixelCord;

extern SDL_Window* win;
void update_win(PixelCord* frame_buffer);
void create_window();
void set_up_event_handler();
void get_window_size(int* w,int* h);
#endif
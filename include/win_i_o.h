
#ifndef WIN_I_O
#define WIN_I_O

#include <stdlib.h>
#include <stdio.h>
#include <SDL3/SDL.h>



extern SDL_Window* win;
void update_win();
void create_window();
void set_up_event_handler();
void get_window_size(int* w,int* h);
#endif
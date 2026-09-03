
#ifndef WIN_I_O
#define WIN_I_O

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <SDL3/SDL.h>

typedef struct PixelCord PixelCord;

extern SDL_Window* win;

// update_win split in two, because a UI repaint must not pay for a frame it did
// not change. win_upload_frame flattens the PixelCord grid into the streaming
// texture; win_present composites that texture plus the UI overlay and flips.
// The overlay is drawn by present, so it can repaint on its own without
// re-rasterizing -- which matters when a frame costs tens of milliseconds and a
// keystroke costs one.
void win_upload_frame(const PixelCord* frame_buffer);
void win_present(void);

// Reads the renderer's finished frame. Borrowed, never freed here.
// Equivalent to win_upload_frame() followed by win_present().
void update_win(const PixelCord* frame_buffer);
void create_window();
void set_up_event_handler();
void get_window_size(int* w,int* h);
#endif
#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <SDL3/SDL.h>
#include "mesh.h"

// An in-window model browser, drawn straight onto the SDL_Renderer AFTER the
// scene texture. It never touches the PixelCord frame buffer and never calls
// render(), so opening it, scrolling it and closing it cost one present rather
// than a whole frame.
//
// It also owns the model currently on screen. That is deliberate: the swap has
// to free the old model at exactly one moment relative to set_objects(), and
// splitting the pointer from the code that frees it is how that rule gets
// broken.

typedef enum UiAction{
    UI_NOTHING=0,    // nothing to do
    UI_REPRESENT,    // the overlay changed and the scene did not: repaint only
    UI_RERENDER      // the scene changed: rasterize a new frame
} UiAction;

// Scan `dir` for model files. Returns how many were found; 0 is not an error.
int  ui_init(const char* dir);
int  ui_rescan(void);

// Take ownership of the model main() loaded at startup, so that the first swap
// has something to free and the header can name what is on screen.
void ui_adopt_model(Model m, const char* name);

// Frees the entry list and the model still loaded.
void ui_shutdown(void);

bool ui_is_open(void);
void ui_close(void);

// True exactly once per open, so the caller can end an in-flight camera drag
// without issuing an SDL_CaptureMouse call on every event the panel consumes.
bool ui_just_opened(void);

// First refusal on every event. Returns true when it consumed the event, in
// which case the caller must not also act on it -- that is what stops an arrow
// key from both moving the list selection and turning the camera.
bool ui_handle_event(const SDL_Event* e);

// A load is a two-step so the "Loading..." notice can reach the glass before the
// blocking read starts: ui_begin_load only latches the notice, and
// ui_run_pending_load does the seconds-long mesh_import and the swap.
bool     ui_load_pending(void);
void     ui_begin_load(void);
UiAction ui_run_pending_load(void);

// Called from win_present() after the scene texture, before SDL_RenderPresent.
// A no-op when closed and idle. Leaves render scale, draw colour and blend mode
// as it found them.
void ui_draw(SDL_Renderer* r, int win_w, int win_h);

#endif

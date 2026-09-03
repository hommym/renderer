#include <stdlib.h>
#include <stdio.h>
#include <SDL3/SDL.h>
#include "renderer.h"
#include "win_i_o.h"

// this file contains code for windows,event,input/output management

// How far the camera turns per pixel of mouse movement while dragging. 0.004
// rad/px puts a full 90 degree turn at roughly 400 pixels of drag, which is
// about a third of the window -- fast enough to look around without overshooting.
#define LOOK_RADIANS_PER_PIXEL 0.004

SDL_Window* win=NULL;
static bool is_proc_running=true;
static bool is_dragging=false;      // left button held: mouse motion turns the camera
static SDL_Renderer* sdl_renderer=NULL;
static SDL_Texture* sdl_texture=NULL;
static int tex_w=0;
static int tex_h=0;
static uint32_t* pixel_buffer=NULL;
static int pb_w=0;
static int pb_h=0;

void get_window_size(int* w,int* h){
if(win!=NULL){
SDL_GetWindowSizeInPixels(win,w,h);
return;
}
printf("Failed to get window size");

}

void set_up_event_handler(){
// this method will set up your event handler 
if(win==NULL){
    printf("No windows has been created\n");
    return;
}
    while (is_proc_running)
    {
        SDL_Event event;
        // Block until something actually happens. Polling in a bare loop spins a
        // core at 100% doing nothing, which matters more now that a frame is
        // expensive enough to be worth not wasting cycles around.
        if(!SDL_WaitEvent(&event))continue;

        // Everything the batch below wants to do, accumulated rather than acted
        // on. A drag delivers mouse motion at the mouse's polling rate -- often
        // several hundred events a second -- and a frame here costs tens of
        // milliseconds, so rendering per event would queue up work faster than
        // it drains and the view would lag seconds behind the cursor. Draining
        // the queue first and drawing ONCE keeps the camera on the pointer:
        // frames are dropped, not stacked.
        bool needs_redraw=false;
        double pending_yaw=0.0,pending_pitch=0.0;

        do{
            switch (event.type)
            {
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                SDL_DestroyWindow(win);
                SDL_Quit();
                win=NULL;
                is_proc_running=false;
                printf("Window destroyed\n");
                break;
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_MAXIMIZED:
            case SDL_EVENT_WINDOW_RESTORED:
                {
                    // MAXIMIZED / RESTORED don't carry the new dims in data1/data2,
                    // and pixel size on HiDPI can differ from logical size, so ask
                    // SDL for the actual pixel size after the state change.
                    int nw, nh;
                    get_window_size(&nw, &nh);
                    printf("Window size changed to %dx%d\n", nw, nh);
                    renderer_resize((uint32_t)nw, (uint32_t)nh);
                }
                needs_redraw=true;
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if(event.button.button==SDL_BUTTON_LEFT){
                    is_dragging=true;
                    // keep receiving motion even when the pointer leaves the
                    // window, so a long drag does not stop at the edge
                    SDL_CaptureMouse(true);
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if(event.button.button==SDL_BUTTON_LEFT){
                    is_dragging=false;
                    SDL_CaptureMouse(false);
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if(is_dragging){
                    // xrel/yrel are the movement since the last motion event, so
                    // they add up across a batch exactly as one long drag would.
                    // y is negated: pushing the mouse down should look down, and
                    // positive pitch is up.
                    pending_yaw   += event.motion.xrel*LOOK_RADIANS_PER_PIXEL;
                    pending_pitch -= event.motion.yrel*LOOK_RADIANS_PER_PIXEL;
                    needs_redraw=true;
                }
                break;

            case SDL_EVENT_MOUSE_WHEEL:
                // event.wheel.y > 0 -> scroll up / away from user  -> forward
                // event.wheel.y < 0 -> scroll down / toward user   -> backward
                // forward now follows where the camera is looking, not +z.
                if(event.wheel.y > 0)move_camera(30,MOV_FORWARD);
                else if(event.wheel.y < 0)move_camera(30,MOV_BACKWARD);
                needs_redraw=true;
                break;

            case SDL_EVENT_KEY_DOWN:
                // held keys repeat, and each repeat is a step: that is what makes
                // holding an arrow glide instead of nudging once.
                switch (event.key.key)
                {
                case SDLK_LEFT:   move_camera(30,MOV_LEFT);     needs_redraw=true; break;
                case SDLK_RIGHT:  move_camera(30,MOV_RIGHT);    needs_redraw=true; break;
                case SDLK_UP:     move_camera(30,MOV_UP);       needs_redraw=true; break;
                case SDLK_DOWN:   move_camera(30,MOV_DOWN);     needs_redraw=true; break;
                case SDLK_W:      move_camera(30,MOV_FORWARD);  needs_redraw=true; break;
                case SDLK_S:      move_camera(30,MOV_BACKWARD); needs_redraw=true; break;
                case SDLK_A:      move_camera(30,MOV_LEFT);     needs_redraw=true; break;
                case SDLK_D:      move_camera(30,MOV_RIGHT);    needs_redraw=true; break;
                case SDLK_ESCAPE:
                    is_dragging=false;
                    SDL_CaptureMouse(false);
                    break;
                }
                break;
            }
        }while(is_proc_running && SDL_PollEvent(&event));

        if(is_proc_running && needs_redraw){
            if(pending_yaw!=0.0 || pending_pitch!=0.0)
                rotate_camera(pending_yaw,pending_pitch);
            clear_frame_buffer(false);
            render();
            update_win(get_frame_buffer());
        }
        
    }
    
}


// What an untouched frame-buffer cell paints as, and what a partly transparent
// one is composited against. Named because it is now needed in two places.
#define BACKGROUND_ARGB 0xFFFFFFFFu

// A texel's alpha is real data, not padding: a base colour texture uses it to cut
// a leaf out of the quad it is drawn on. 37.7% of the eco house's atlas is fully
// transparent and another 13.2% is partial, so copying the packed word straight
// through paints every one of those as solid colour.
//
// The composite is done here rather than by handing SDL a blended texture,
// because SDL would blend the whole surface -- background cells included --
// against whatever RenderClear left, which is a different operation. Here the
// background is known and only the cells that need it pay for it.
//
// Straight (non-premultiplied) source-over, in sRGB space. Blending in sRGB is
// not photometrically right, but every other colour operation in this renderer
// is sRGB-naive too and matching them beats being correct in one place only.
static uint32_t blend_over_background(uint32_t argb){
uint32_t a=argb>>24;
if(a==0xFFu)return argb;                     // the overwhelmingly common case
if(a==0u)return BACKGROUND_ARGB;

uint32_t ia=255u-a;
uint32_t br=(BACKGROUND_ARGB>>16)&0xFFu;
uint32_t bg=(BACKGROUND_ARGB>>8)&0xFFu;
uint32_t bb=BACKGROUND_ARGB&0xFFu;
// +127 rounds to nearest rather than truncating, so a fully opaque-equivalent
// blend cannot come back one level dark
uint32_t r=(((argb>>16)&0xFFu)*a+br*ia+127u)/255u;
uint32_t g=(((argb>>8)&0xFFu)*a+bg*ia+127u)/255u;
uint32_t b=((argb&0xFFu)*a+bb*ia+127u)/255u;
// the result is opaque: it has already been flattened onto the background
return 0xFF000000u|(r<<16)|(g<<8)|b;
}

void update_win(PixelCord* frame_buffer){
if(win==NULL){
    printf("No window to update\n");
    return;
}

int w,h;
get_window_size(&w,&h);

if(sdl_renderer==NULL){
    sdl_renderer=SDL_CreateRenderer(win,NULL);
    if(sdl_renderer==NULL){
        printf("Renderer creation failed: %s\n",SDL_GetError());
        abort();
    }
}

if(sdl_texture==NULL || tex_w!=w || tex_h!=h){
    if(sdl_texture!=NULL)SDL_DestroyTexture(sdl_texture);
    sdl_texture=SDL_CreateTexture(sdl_renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,w,h);
    if(sdl_texture==NULL){
        printf("Texture creation failed: %s\n",SDL_GetError());
        abort();
    }
    tex_w=w;
    tex_h=h;
}

if(pixel_buffer==NULL || pb_w!=w || pb_h!=h){
    free(pixel_buffer);
    pixel_buffer=(uint32_t*)malloc((size_t)w*(size_t)h*sizeof(uint32_t));
    if(pixel_buffer==NULL){
        printf("Pixel buffer allocation failed\n");
        abort();
    }
    pb_w=w;
    pb_h=h;
}

// flatten the PixelCord grid into ARGB pixels for SDL.
// empty cells (never written by render()) paint as the background;
// occupied cells (.in_use==true) are composited over it by their own alpha.
size_t total=(size_t)w*(size_t)h;
for(size_t i=0;i<total;i++){
    pixel_buffer[i]=frame_buffer[i].in_use ? blend_over_background(frame_buffer[i].colour)
                                           : BACKGROUND_ARGB;
}

SDL_UpdateTexture(sdl_texture,NULL,pixel_buffer,w*(int)sizeof(uint32_t));
SDL_RenderClear(sdl_renderer);
SDL_RenderTexture(sdl_renderer,sdl_texture,NULL,NULL);
SDL_RenderPresent(sdl_renderer);
}

void create_window(){    
// this method will set up your windows 
if(!SDL_WasInit(SDL_INIT_VIDEO)){
printf("Initialising subsystems from win...\n");
bool is_init= SDL_Init(SDL_INIT_VIDEO);
if(!is_init){
printf("Sub-system initialisation Failed: %s\n",SDL_GetError());    
abort();    
}

printf("subsystems Initialised\n");
}

printf("Creating a Window...\n");
win=SDL_CreateWindow("Renderer",1500,1000,SDL_WINDOW_RESIZABLE);

if(win==NULL){
    printf("Window Creation Failed: %s\n",SDL_GetError());
    abort();
}
printf("Windows Created\n");
}


#include <stdlib.h>
#include <stdio.h>
#include <SDL3/SDL.h>
#include "renderer.h"


// this file contains code for windows,event,input/output management
SDL_Window* win=NULL;
static bool is_proc_running=true;
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
        while (SDL_PollEvent(&event))
        {
            
            switch (event.type)
            {
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                SDL_DestroyWindow(win);
                SDL_Quit();
                is_proc_running=false;
                printf("Window destroyed\n");
                break;
            case SDL_EVENT_MOUSE_MOTION:
                printf("Mouse is moving\n");
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                printf("Mouse button pressed\n");
                break;    
            }

        }
        
    }
    
}


void update_win(Vectex* frame_buffer){
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

// flatten the Vectex grid into ARGB pixels for SDL.
// empty cells are zeroed by calloc, so their .colour is 0 (black).
size_t total=(size_t)w*(size_t)h;
for(size_t i=0;i<total;i++){
    pixel_buffer[i]=frame_buffer[i].colour;
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


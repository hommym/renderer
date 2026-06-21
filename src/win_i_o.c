#include <stdlib.h>
#include <stdio.h>
#include <SDL3/SDL.h>


// this file contains code for windows,event,input/output management 
SDL_Window* win=NULL;
static bool is_proc_running=true;



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


void update_win(){
//code for updating window screen.


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
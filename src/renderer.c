#include "renderer.h"


uint32_t focal_len=24;
uint32_t screen_width; //max column on fram_buffer
uint32_t screen_hieght; // max row on frame_buffer




static void init(){    
if(SDL_WasInit(SDL_INIT_VIDEO))return;
printf("Initialising subsystems from render...\n");
bool is_init= SDL_Init(SDL_INIT_VIDEO);

if(!is_init){
printf("Sub-system initialisation Failed: %s\n",SDL_GetError());    
abort();    
}
printf("subsystems Initialised\n");
}

static void get_screen_size(const SDL_DisplayMode* screen_size_p){

    SDL_DisplayID displayID = SDL_GetPrimaryDisplay();
    if (displayID == 0) {
        SDL_Log("Failed to get primary display! SDL_Error: %s\n", SDL_GetError());
        abort();
    }

    // 3. Query the current mode. Returns a pointer to a read-only struct, or NULL on error
    screen_size_p = SDL_GetCurrentDisplayMode(displayID);
    if (!screen_size_p) {
        SDL_Log("SDL_GetCurrentDisplayMode failed: %s\n", SDL_GetError());
        abort();
        
    }
}


static uint32_t* create_frame_buffer(){
init();    // initialising SDL subsystem in order to get screeen size
const SDL_DisplayMode screen_size={};

get_screen_size(&screen_size);

screen_width=(uint32_t)screen_size.w;
screen_hieght=(uint32_t)screen_size.h;

return (uint32_t*) calloc((screen_hieght*screen_width),4);
}



uint32_t*  render(Object* objects,uint64_t len){
uint32_t* frame_buffer= create_frame_buffer();
//code impl

return frame_buffer;
}

void release_frame_buffer(uint32_t* address){
    free(address);
}
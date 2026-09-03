#include "ui.h"
#include "renderer.h"
#include "win_i_o.h"   // for the window handle the hit-test needs the size of
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The model browser. SDL3 ships an 8x8 debug font (SDL_RenderDebugText) and a
// directory walker (SDL_EnumerateDirectory), so this needs no font file, no
// SDL_ttf and no dirent.h -- which is the whole reason it is drawn with the
// renderer's own primitives instead of into the PixelCord buffer.

// Where a loaded model is put, matching what main.c used to hard-code. Every
// model is fitted into the same box, which is why camera_reset() is enough to
// frame a newly swapped one.
#define MODEL_EXTENT   420.0
#define MODEL_CENTRE_X 0.0
#define MODEL_CENTRE_Y 0.0
#define MODEL_CENTRE_Z 550.0

// All layout is in "UI units" = window pixels / UI_SCALE, so the 8px font stays
// readable on a 1500x1000 window.
#define UI_SCALE      2.0f
#define UI_ROW_H      12.0f
#define UI_PAD        8.0f
#define UI_CHAR       ((float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE)
#define UI_HEADER_H   (UI_ROW_H*2.0f)
#define UI_FOOTER_H   (UI_ROW_H*2.0f)

typedef struct Entry { char* name; char* path; uint64_t size; } Entry;

static Entry*   entries=NULL;
static int      entry_count=0;
static char*    scan_dir=NULL;

static bool     open_=false;
static bool     busy=false;          // a load is latched and about to block
static int      sel=0;               // highlighted row
static int      first=0;             // first visible row
static int      pending=-1;          // entry index the user asked for, or -1
static bool     just_opened=false;   // consumed once by the event loop
static char     loading_name[128]="";
static char     status[192]="";

static Model    current={0};
static char     current_name[128]="(none)";
static uint64_t current_tris=0;

// ---- the file list ---------------------------------------------------------

static bool has_model_ext(const char* n){
    const char* dot=strrchr(n,'.');
    if(!dot)return false;
    return SDL_strcasecmp(dot,".glb")==0||SDL_strcasecmp(dot,".gltf")==0
         ||SDL_strcasecmp(dot,".obj")==0||SDL_strcasecmp(dot,".ply")==0;
}

static void entries_free(void){
    for(int i=0;i<entry_count;i++){ free(entries[i].name); free(entries[i].path); }
    free(entries);
    entries=NULL;
    entry_count=0;
}

static SDL_EnumerationResult collect(void* ud,const char* dirname,const char* fname){
    (void)ud;
    if(!has_model_ext(fname))return SDL_ENUM_CONTINUE;

    Entry* grown=realloc(entries,(size_t)(entry_count+1)*sizeof *entries);
    if(!grown)return SDL_ENUM_FAILURE;
    entries=grown;

    // SDL hands dirname back with its trailing separator already on it
    size_t dl=strlen(dirname), fl=strlen(fname);
    char* path=malloc(dl+fl+1);
    char* name=malloc(fl+1);
    if(!path||!name){ free(path); free(name); return SDL_ENUM_FAILURE; }
    memcpy(path,dirname,dl);
    memcpy(path+dl,fname,fl+1);
    memcpy(name,fname,fl+1);

    SDL_PathInfo info={0};
    uint64_t size=SDL_GetPathInfo(path,&info)?(uint64_t)info.size:0;

    entries[entry_count++]=(Entry){.name=name,.path=path,.size=size};
    return SDL_ENUM_CONTINUE;
}

static int cmp_entry(const void* a,const void* b){
    return SDL_strcasecmp(((const Entry*)a)->name,((const Entry*)b)->name);
}

int ui_rescan(void){
    entries_free();
    if(!scan_dir)return 0;
    SDL_EnumerateDirectory(scan_dir,collect,NULL);
    // enumeration order is the filesystem's, not alphabetical, so the same
    // directory would list differently on different machines
    if(entry_count>1)SDL_qsort(entries,(size_t)entry_count,sizeof *entries,cmp_entry);
    if(sel>=entry_count)sel=entry_count>0?entry_count-1:0;
    if(first>sel)first=sel;
    return entry_count;
}

int ui_init(const char* dir){
    free(scan_dir);
    scan_dir=dir?SDL_strdup(dir):NULL;
    return ui_rescan();
}

void ui_adopt_model(Model m,const char* name){
    current=m;
    current_tris=0;
    for(uint64_t i=0;i<current.len;i++)current_tris+=current.objects[i].len_of_connectors/3;
    if(name)SDL_strlcpy(current_name,name,sizeof current_name);
}

void ui_shutdown(void){
    entries_free();
    free(scan_dir);
    scan_dir=NULL;
    mesh_model_free(&current);   // safe on a zeroed Model
}

// ---- state -----------------------------------------------------------------

bool ui_is_open(void){ return open_; }
void ui_close(void){ open_=false; }
bool ui_just_opened(void){ bool j=just_opened; just_opened=false; return j; }
bool ui_load_pending(void){ return pending>=0; }

void ui_begin_load(void){
    if(pending<0)return;
    busy=true;
    SDL_strlcpy(loading_name,entries[pending].name,sizeof loading_name);
    status[0]='\0';
}

UiAction ui_run_pending_load(void){
    if(pending<0){ busy=false; return UI_NOTHING; }
    int i=pending;
    pending=-1;

    uint64_t t0=SDL_GetTicksNS();
    Model fresh={0};
    MeshResult r=mesh_import(entries[i].path,MODEL_EXTENT,
                             MODEL_CENTRE_X,MODEL_CENTRE_Y,MODEL_CENTRE_Z,&fresh);
    double secs=(double)(SDL_GetTicksNS()-t0)/1e9;
    busy=false;

    if(r!=MESH_OK){
        // mesh_import promises a zeroed Model on failure, so there is nothing to
        // free here -- and the old model has not been touched, so it is still
        // installed and still on screen.
        SDL_snprintf(status,sizeof status,"%s: %s",entries[i].name,mesh_result_string(r));
        return UI_REPRESENT;
    }

    // THE ORDER IS THE RULE, and it is these three steps in this order:
    set_objects(fresh.objects,fresh.len);   // 1. the renderer reads the new array
    mesh_model_free(&current);              // 2. only now release the old one
    current=fresh;                          // 3. take ownership
    // Reversing 1 and 2 is a use-after-free the moment render() stops joining
    // its workers before returning. It does today; do not rely on it.

    current_tris=0;
    for(uint64_t k=0;k<current.len;k++)current_tris+=current.objects[k].len_of_connectors/3;
    SDL_strlcpy(current_name,entries[i].name,sizeof current_name);

    camera_reset();
    open_=false;
    if(current.primitives_skipped)
        SDL_snprintf(status,sizeof status,"%s: %llu objects, %llu tris, %.1fs (%llu primitives skipped)",
                     current_name,(unsigned long long)current.len,
                     (unsigned long long)current_tris,secs,
                     (unsigned long long)current.primitives_skipped);
    else
        SDL_snprintf(status,sizeof status,"%s: %llu objects, %llu tris, %.1fs",
                     current_name,(unsigned long long)current.len,
                     (unsigned long long)current_tris,secs);
    return UI_RERENDER;
}

// ---- layout ----------------------------------------------------------------

static SDL_FRect panel_rect(int win_w,int win_h){
    float uw=(float)win_w/UI_SCALE, uh=(float)win_h/UI_SCALE;
    float pw=uw-2.0f*UI_PAD; if(pw>460.0f)pw=460.0f;
    float ph=uh-2.0f*UI_PAD; if(ph>340.0f)ph=340.0f;
    return (SDL_FRect){ (uw-pw)*0.5f, (uh-ph)*0.5f, pw, ph };
}

static int rows_visible(SDL_FRect p){
    int n=(int)((p.h-UI_HEADER_H-UI_FOOTER_H-2.0f*UI_PAD)/UI_ROW_H);
    return n<1?1:n;
}

static void scroll_into_view(int win_w,int win_h){
    int rv=rows_visible(panel_rect(win_w,win_h));
    if(sel<first)first=sel;
    if(sel>=first+rv)first=sel-rv+1;
    if(first<0)first=0;
}

// Mouse coordinates -> UI units, and there are TWO conversions here, not one.
//
// SDL reports a mouse event in WINDOW coordinates, but everything drawn goes
// through the renderer, whose output is measured in PIXELS -- and on a HiDPI
// display those are not the same number. SDL_RenderCoordinatesFromWindow is the
// mapping between them; skipping it puts the hit-test half a panel away from the
// highlight on exactly the displays where it is hardest to notice in a
// screenshot. Then SDL_SetRenderScale is undone on top of that: it scales what
// is drawn but not what SDL reports.
static int row_at(int win_w,int win_h,float wx,float wy){
    SDL_FRect p=panel_rect(win_w,win_h);
    float rx=wx,ry=wy;
    SDL_Renderer* rend=win?SDL_GetRenderer(win):NULL;
    if(rend)SDL_RenderCoordinatesFromWindow(rend,wx,wy,&rx,&ry);
    float ux=rx/UI_SCALE, uy=ry/UI_SCALE;
    float top=p.y+UI_PAD+UI_HEADER_H;
    if(ux<p.x||ux>p.x+p.w||uy<top||uy>p.y+p.h)return -1;
    int i=first+(int)((uy-top)/UI_ROW_H);
    if(i-first>=rows_visible(p))return -1;
    return (i>=0&&i<entry_count)?i:-1;
}

// ---- events ----------------------------------------------------------------

// Which events the panel is ever entitled to claim. Window events are NOT on the
// list: swallowing SDL_EVENT_WINDOW_CLOSE_REQUESTED while a load is latched
// would make the window unclosable for the several seconds an import takes.
static bool is_input_event(const SDL_Event* e){
    switch(e->type){
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_WHEEL:
        return true;
    default:
        return false;
    }
}

bool ui_handle_event(const SDL_Event* e){
    if(!is_input_event(e))return false;        // resize and close always get through
    if(busy)return true;                       // swallow input mid-load

    if(e->type==SDL_EVENT_KEY_DOWN&&(e->key.key==SDLK_L||e->key.key==SDLK_F1)){
        open_=!open_;
        if(open_){ ui_rescan(); status[0]='\0'; just_opened=true; }
        return true;
    }
    if(!open_)return false;                    // closed: everything falls through

    int w=0,h=0;
    if(win)SDL_GetWindowSizeInPixels(win,&w,&h);

    switch(e->type){
    case SDL_EVENT_KEY_DOWN:
        switch(e->key.key){
        case SDLK_ESCAPE: open_=false; return true;
        case SDLK_UP:     if(sel>0)sel--;             scroll_into_view(w,h); return true;
        case SDLK_DOWN:   if(sel+1<entry_count)sel++; scroll_into_view(w,h); return true;
        case SDLK_PAGEUP:
            sel-=rows_visible(panel_rect(w,h)); if(sel<0)sel=0;
            scroll_into_view(w,h); return true;
        case SDLK_PAGEDOWN:
            sel+=rows_visible(panel_rect(w,h));
            if(sel>=entry_count)sel=entry_count>0?entry_count-1:0;
            scroll_into_view(w,h); return true;
        case SDLK_HOME:   sel=0;                      scroll_into_view(w,h); return true;
        case SDLK_END:    sel=entry_count>0?entry_count-1:0; scroll_into_view(w,h); return true;
        case SDLK_R:      ui_rescan(); return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if(entry_count>0)pending=sel;
            return true;
        }
        // any other key while the panel is open is still consumed, so it cannot
        // move the camera behind the overlay
        return true;

    case SDL_EVENT_MOUSE_WHEEL:{
        int rv=rows_visible(panel_rect(w,h));
        first-=(int)e->wheel.y*3;
        if(first>entry_count-rv)first=entry_count-rv;
        if(first<0)first=0;
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:{
        int i=row_at(w,h,e->button.x,e->button.y);
        if(i>=0){
            sel=i;
            // a single click only selects. loading blocks for up to five
            // seconds, so it needs a deliberate gesture: double click, or Enter.
            if(e->button.clicks>=2)pending=i;
        }
        return true;    // never let a click through to the camera drag
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_MOTION:
        return true;
    }
    return false;       // window close / resize still reach their own handlers
}

// ---- drawing ---------------------------------------------------------------

void ui_draw(SDL_Renderer* r,int win_w,int win_h){
    if(!open_&&!busy)return;

    SDL_FRect p=panel_rect(win_w,win_h);
    SDL_SetRenderScale(r,UI_SCALE,UI_SCALE);
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);

    SDL_SetRenderDrawColor(r,18,18,24,232);    SDL_RenderFillRect(r,&p);
    SDL_SetRenderDrawColor(r,120,140,180,255); SDL_RenderRect(r,&p);

    if(busy){
        SDL_SetRenderDrawColor(r,255,235,150,255);
        SDL_RenderDebugTextFormat(r,p.x+UI_PAD,p.y+p.h*0.5f-UI_ROW_H,"Loading %s ...",loading_name);
        SDL_SetRenderDrawColor(r,170,170,190,255);
        SDL_RenderDebugText(r,p.x+UI_PAD,p.y+p.h*0.5f,"the window is blocked until it finishes");
        SDL_SetRenderScale(r,1.0f,1.0f);
        return;
    }

    SDL_SetRenderDrawColor(r,235,235,245,255);
    SDL_RenderDebugTextFormat(r,p.x+UI_PAD,p.y+UI_PAD,"LOAD MODEL   %d files",entry_count);
    SDL_SetRenderDrawColor(r,150,160,180,255);
    SDL_RenderDebugTextFormat(r,p.x+UI_PAD,p.y+UI_PAD+UI_ROW_H,"current: %s  %llu tris",
                              current_name,(unsigned long long)current_tris);

    // the debug font does no wrapping and no clipping, so a long name would draw
    // straight across the scene: truncate by hand
    int name_cols=(int)((p.w-2.0f*UI_PAD)/UI_CHAR)-2-10;
    if(name_cols<8)name_cols=8;

    int rv=rows_visible(p);
    float y=p.y+UI_PAD+UI_HEADER_H;
    for(int i=first;i<entry_count&&i-first<rv;i++,y+=UI_ROW_H){
        if(i==sel){
            SDL_FRect hl={p.x+2.0f,y-2.0f,p.w-4.0f,UI_ROW_H};
            SDL_SetRenderDrawColor(r,52,96,176,255);
            SDL_RenderFillRect(r,&hl);
        }
        SDL_SetRenderDrawColor(r,i==sel?255:200,i==sel?255:200,i==sel?255:210,255);
        SDL_RenderDebugTextFormat(r,p.x+UI_PAD,y,"%c %-*.*s %6.1f MB",i==sel?'>':' ',
                                  name_cols,name_cols,entries[i].name,
                                  (double)entries[i].size/1048576.0);
    }
    if(entry_count==0){
        SDL_SetRenderDrawColor(r,200,140,140,255);
        SDL_RenderDebugTextFormat(r,p.x+UI_PAD,y,"no .glb/.gltf/.obj/.ply in %s",
                                  scan_dir?scan_dir:"(no directory)");
    }

    SDL_SetRenderDrawColor(r,150,160,180,255);
    SDL_RenderDebugText(r,p.x+UI_PAD,p.y+p.h-UI_PAD-UI_ROW_H,
        status[0]?status:"Up/Down select   Enter load   R rescan   Esc close");

    SDL_SetRenderScale(r,1.0f,1.0f);   // always restore: the scene blit must not inherit it
}

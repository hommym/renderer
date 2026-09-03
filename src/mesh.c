#include "mesh.h"
#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

// Front desk for the loaders: sniff, dispatch, validate, and the two helpers
// (free / fit) every caller needs. The per-format parsers live in their own
// files; nothing here knows what a PLY header looks like.

// enough for the longest magic we test plus a run of leading whitespace before
// a '{'. A pretty-printed .gltf never opens with more padding than this.
#define HEAD_SNIFF 64

static bool ci_streq(const char* a,const char* b){
// tolower() is only defined for unsigned char values, and a path byte >= 0x80
// is negative in a signed char.
while(*a&&*b){
    if(tolower((unsigned char)*a)!=tolower((unsigned char)*b))return false;
    a++;b++;
}
return *a==*b;
}

// Extension of the last path component only, so a dot in a directory name
// ("v0.9/mesh") is not mistaken for one.
static const char* path_ext(const char* path){
const char* base=path;
for(const char* p=path;*p;p++)if(*p=='/'||*p=='\\')base=p+1;
const char* dot=strrchr(base,'.');
return (dot&&dot[1])?dot+1:NULL;
}

static size_t read_head(const char* path,unsigned char* buf,size_t cap){
FILE* f=fopen(path,"rb");
if(!f)return 0;
size_t n=fread(buf,1,cap,f);   // a directory or an empty file lands here as 0
fclose(f);
return n;
}

MeshFormat mesh_detect_format(const char* path){
if(!path||!*path)return MESH_FORMAT_UNKNOWN;

unsigned char head[HEAD_SNIFF];
size_t n=read_head(path,head,sizeof head);

// content wins over the name: a .glb magic is unambiguous, so an mp3 extension
// on a real GLB still loads.
if(n>=4&&memcmp(head,"glTF",4)==0)return MESH_FORMAT_GLB;
if(n>=4&&memcmp(head,"ply",3)==0&&(head[3]=='\n'||head[3]=='\r'))return MESH_FORMAT_PLY;
for(size_t i=0;i<n;i++){
    if(isspace(head[i]))continue;
    if(head[i]=='{')return MESH_FORMAT_GLTF;
    break;   // the first real byte settles it; anything else falls through
}

const char* ext=path_ext(path);
if(!ext)return MESH_FORMAT_UNKNOWN;
if(ci_streq(ext,"ply"))return MESH_FORMAT_PLY;
if(ci_streq(ext,"obj"))return MESH_FORMAT_OBJ;
if(ci_streq(ext,"gltf"))return MESH_FORMAT_GLTF;
if(ci_streq(ext,"glb"))return MESH_FORMAT_GLB;
return MESH_FORMAT_UNKNOWN;
}

// Last gate before untrusted indices reach render(), which walks
// connectors_sequence three at a time and dereferences each entry into
// vertices[] without checking. A loader bug or a hostile file must not become
// an out-of-bounds read there, so re-check the whole index list here even
// though each loader is expected to have checked it already.
static MeshResult validate(Object* o){
if(!o->vertices||o->len_of_vertices==0)return MESH_ERR_EMPTY;
if(!o->connectors_sequence||o->len_of_connectors<3)return MESH_ERR_EMPTY;
if(o->len_of_connectors%3u)return MESH_ERR_FORMAT;   // a trailing partial triangle would be read anyway
for(uint64_t i=0;i<o->len_of_connectors;i++)
    if(o->connectors_sequence[i]>=o->len_of_vertices)return MESH_ERR_FORMAT;

// the rasterizer dereferences the texture without checking it, so a loader that
// returned MESH_OK with none is a null dereference one frame later
if(!o->texture||o->texture_width==0||o->texture_height==0){
    if(!mesh_set_flat_texture(o,MESH_DEFAULT_COLOUR))return MESH_ERR_OOM;
}

// second gate on the texture coordinates, for the same reason the index list is
// re-checked above: (size_t)(texture_height*v) is used as an array subscript
// with no bounds test, so a stray 1.0 or a negative is an out-of-bounds read.
// The loaders already wrap; this catches the one that forgot.
for(uint64_t i=0;i<o->len_of_vertices;i++){
    Vectex* v=&o->vertices[i];
    if(!(v->u>=0.0f&&v->u<1.0f))v->u=mesh_wrap_uv(v->u);
    if(!(v->v>=0.0f&&v->v<1.0f))v->v=mesh_wrap_uv(v->v);
}
return MESH_OK;
}

MeshResult mesh_load(const char* path,Object* out){
// no MESH_ERR_ARGS in the enum, and a call with nothing to open is closest to
// "could not open the file".
if(!out)return MESH_ERR_OPEN;
*out=(Object){0};
if(!path)return MESH_ERR_OPEN;

MeshResult r;
switch(mesh_detect_format(path)){
    case MESH_FORMAT_PLY:  r=mesh_load_ply(path,out);  break;
    case MESH_FORMAT_OBJ:  r=mesh_load_obj(path,out);  break;
    case MESH_FORMAT_GLTF:
    case MESH_FORMAT_GLB:  r=mesh_load_gltf(path,out); break;
    default:               r=MESH_ERR_UNSUPPORTED;     break;
}

if(r==MESH_OK)r=validate(out);
// covers both the loader that failed after allocating and the one that
// succeeded into a structure validate() rejected: either way the caller is
// promised a zeroed Object with nothing left to free.
if(r!=MESH_OK)mesh_free(out);
return r;
}

// The whole pipeline behind one call: detect, parse, split by material, decode
// the textures, and place the result in front of the camera.
MeshResult mesh_import(const char* path,double target_extent,
                       double cx,double cy,double cz,Model* out){
if(!out)return MESH_ERR_OPEN;
*out=(Model){0};

MeshResult r=mesh_load_scene(path,out);
if(r!=MESH_OK)return r;

// target_extent of 0 means "leave the file's own coordinates alone". Otherwise a
// failed fit is a failed import: it means the mesh has no measurable size, and
// handing back a model that is a speck or swallows the screen is not a success.
if(target_extent>0.0&&!mesh_model_fit(out,target_extent,cx,cy,cz,true)){
    mesh_model_free(out);
    return MESH_ERR_EMPTY;
}
return MESH_OK;
}

MeshResult mesh_load_scene(const char* path,Model* out){
if(!out)return MESH_ERR_OPEN;
*out=(Model){0};
if(!path)return MESH_ERR_OPEN;

MeshFormat f=mesh_detect_format(path);
if(f==MESH_FORMAT_GLTF||f==MESH_FORMAT_GLB){
    MeshResult r=mesh_load_gltf_scene(path,out);
    if(r!=MESH_OK)return r;
    // same gate every single Object goes through: the rasterizer trusts the
    // index list and the uv range absolutely, whichever path produced them
    for(size_t i=0;i<out->len;i++){
        r=validate(&out->objects[i]);
        if(r!=MESH_OK){ mesh_model_free(out); return r; }
    }
    return MESH_OK;
}

// ply and obj are effectively single-material here, so a one-object scene is
// the honest answer rather than a special case threaded through both loaders
Object one={0};
MeshResult r=mesh_load(path,&one);
if(r!=MESH_OK)return r;
one.double_sided=true;
Object* objs=malloc(sizeof *objs);
uint32_t** texs=malloc(sizeof *texs);
if(!objs||!texs){ free(objs); free(texs); mesh_free(&one); return MESH_ERR_OOM; }
objs[0]=one;
// ply and obj carry no sidedness at all, and guessing "single sided" would
// silently delete half of any open mesh
objs[0].double_sided=true;
texs[0]=one.texture;
out->objects=objs;out->len=1;
out->textures=texs;out->texture_count=1;
return MESH_OK;
}

void mesh_model_free(Model* s){
if(!s)return;
for(size_t i=0;i<s->len;i++){
    free(s->objects[i].vertices);
    free(s->objects[i].connectors_sequence);
    // NOT the texture: several objects share one, and the scene owns them
}
for(size_t i=0;i<s->texture_count;i++)free(s->textures[i]);
free(s->objects);
free(s->textures);
*s=(Model){0};
}

void mesh_free(Object* obj){
if(!obj)return;
free(obj->vertices);
free(obj->connectors_sequence);
free(obj->texture);
*obj=(Object){0};   // zeroing is what makes a second call a no-op
}

// ---- shared loader helpers ----------------------------------------------

float mesh_wrap_uv(double t){
if(!isfinite(t))return 0.0f;
// Coordinates already inside the unit square are the common case by a wide
// margin -- an atlas is authored that way -- and there 1.0 means the far EDGE of
// the last texel, not the first texel of the next tile. Folding it to 0.0 would
// collapse every full-range quad onto texel (0,0), so an in-range value is only
// nudged below 1. The REPEAT wrap is kept for what it is actually for: a tiled
// surface authored with u running 0 to 8.
double w=(t>=0.0&&t<=1.0)?t:t-floor(t);
if(!(w>=0.0))w=0.0;           // the negated form also catches a NaN from floor
float f=(float)w;
// the narrowing can round up, and 1.0f is the one value the rasterizer's
// (size_t)(texture_height*v) cannot survive
if(f>=1.0f)f=0x1.fffffep-1f;  // largest float strictly below 1
return f;
}

bool mesh_path_is_safe(const char* rel){
if(!rel||!*rel)return false;
if(rel[0]=='/'||rel[0]=='\\')return false;
if(rel[1]==':')return false;                      // c:\... and other drive paths
for(const char* q=rel;*q;){
    const char* e=q;
    while(*e&&*e!='/'&&*e!='\\')e++;
    if(e-q==2&&q[0]=='.'&&q[1]=='.')return false;
    q=*e?e+1:e;
}
return true;
}

char* mesh_path_sibling(const char* base_path,const char* rel){
if(!base_path||!mesh_path_is_safe(rel))return NULL;
const char* slash=NULL;
for(const char* p=base_path;*p;p++)if(*p=='/'||*p=='\\')slash=p;
size_t dir=slash?(size_t)(slash-base_path)+1:0;
size_t n=strlen(rel);
if(n>SIZE_MAX-dir-1)return NULL;
char* full=malloc(dir+n+1);
if(!full)return NULL;
memcpy(full,base_path,dir);
memcpy(full+dir,rel,n+1);
// windows exporters write "textures\\wall.jpg"; normalise so the rest of the
// program only ever sees '/'
for(size_t i=dir;i<dir+n;i++)if(full[i]=='\\')full[i]='/';
return full;
}

bool mesh_set_flat_texture(Object* obj,uint32_t argb){
if(!obj)return false;
Image img={0};
if(!image_solid(argb,&img))return false;
free(obj->texture);
obj->texture=img.pixels;
obj->texture_width=img.width;
obj->texture_height=img.height;
return true;
}

const char* mesh_result_string(MeshResult r){
switch(r){
    case MESH_OK:              return "ok";
    case MESH_ERR_OPEN:        return "could not open file";
    case MESH_ERR_READ:        return "truncated or unreadable file";
    case MESH_ERR_FORMAT:      return "malformed file";
    case MESH_ERR_UNSUPPORTED: return "unsupported format or feature";
    case MESH_ERR_OOM:         return "out of memory";
    case MESH_ERR_EMPTY:       return "no triangles in file";
}
return "unknown mesh error";   // a value outside the enum still gets a string
}

// ---- fitting ---------------------------------------------------------------

// Folds one object's vertices into a running bounding box. `seen` carries across
// calls so a whole scene shares one box.
static void bbox_fold(const Object* o,double mn[3],double mx[3],uint64_t* seen){
if(!o||!o->vertices)return;
for(uint64_t i=0;i<o->len_of_vertices;i++){
    Vectex v=o->vertices[i];
    // one NaN vertex poisons every later comparison (NaN compares false both
    // ways, so the box stops growing), so skip it rather than fold it in.
    if(!isfinite(v.x)||!isfinite(v.y)||!isfinite(v.z))continue;
    if(*seen==0){
        mn[0]=mx[0]=v.x; mn[1]=mx[1]=v.y; mn[2]=mx[2]=v.z;
    }else{
        if(v.x<mn[0])mn[0]=v.x; else if(v.x>mx[0])mx[0]=v.x;
        if(v.y<mn[1])mn[1]=v.y; else if(v.y>mx[1])mx[1]=v.y;
        if(v.z<mn[2])mn[2]=v.z; else if(v.z>mx[2])mx[2]=v.z;
    }
    (*seen)++;
}
}

// scale/centre worked out once from the shared box, then applied everywhere
static bool fit_params(const double mn[3],const double mx[3],uint64_t seen,
                       double target_extent,double* scale,double mid[3]){
if(seen==0)return false;                          // every vertex was garbage
double sx=mx[0]-mn[0],sy=mx[1]-mn[1],sz=mx[2]-mn[2];
double extent=sx;
if(sy>extent)extent=sy;
if(sz>extent)extent=sz;
// spans of finite values can still overflow to inf (DBL_MAX to -DBL_MAX), and a
// single-point mesh has extent 0: both make the scale meaningless.
if(!isfinite(extent)||extent<=0.0)return false;
*scale=target_extent/extent;
if(!isfinite(*scale))return false;
mid[0]=(mn[0]+mx[0])*0.5;
mid[1]=(mn[1]+mx[1])*0.5;
mid[2]=(mn[2]+mx[2])*0.5;
return true;
}

static void apply_fit(Object* o,double scale,const double mid[3],
                      double cx,double cy,double cz,bool flip_y){
if(!o||!o->vertices)return;
for(uint64_t i=0;i<o->len_of_vertices;i++){
    Vectex* v=&o->vertices[i];
    v->x=(v->x-mid[0])*scale+cx;
    // model formats are +y up, this renderer is +y down; mirroring about the
    // box centre is the same as negating after the recentre.
    v->y=(flip_y?(mid[1]-v->y):(v->y-mid[1]))*scale+cy;
    v->z=(v->z-mid[2])*scale+cz;
}
}

bool mesh_fit_to_view(Object* obj,double target_extent,
                      double cx,double cy,double cz,bool flip_y){
if(!obj||!obj->vertices||obj->len_of_vertices==0)return false;
// a non-finite target or centre would turn every vertex into NaN, which the
// projection then silently drops. Refuse instead of quietly emptying the mesh.
if(!isfinite(target_extent)||!isfinite(cx)||!isfinite(cy)||!isfinite(cz))return false;

double mn[3]={0,0,0},mx[3]={0,0,0},mid[3],scale;
uint64_t seen=0;
bbox_fold(obj,mn,mx,&seen);
if(!fit_params(mn,mx,seen,target_extent,&scale,mid))return false;
apply_fit(obj,scale,mid,cx,cy,cz,flip_y);
return true;
}

bool mesh_model_fit(Model* s,double target_extent,
                            double cx,double cy,double cz,bool flip_y){
if(!s||!s->objects||s->len==0)return false;
if(!isfinite(target_extent)||!isfinite(cx)||!isfinite(cy)||!isfinite(cz))return false;

// ONE box over every object. Fitting them one at a time would scale each
// material's share to the same extent and stack them all on the same centre,
// which turns a model into a heap.
double mn[3]={0,0,0},mx[3]={0,0,0},mid[3],scale;
uint64_t seen=0;
for(size_t i=0;i<s->len;i++)bbox_fold(&s->objects[i],mn,mx,&seen);
if(!fit_params(mn,mx,seen,target_extent,&scale,mid))return false;
for(size_t i=0;i<s->len;i++)apply_fit(&s->objects[i],scale,mid,cx,cy,cz,flip_y);
return true;
}

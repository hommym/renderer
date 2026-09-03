// PLY (Stanford polygon) loader.
//
// Nothing about the layout is hardcoded: the header declares every element and
// every property, and real exporters differ wildly in order, in extra vertex
// fields and in extra elements. Unknown elements are stepped over from their
// declared stride -- and for list properties by reading each row's count --
// because a wrong skip does not fail loudly, it silently reinterprets every
// byte after it as the wrong thing.
//
// The whole file is slurped into one buffer first, so every field access is a
// bounds check against a known length instead of a stream of fread() returns
// that are easy to get half right on a truncated file.

#include "mesh.h"
#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLY_NAME_MAX  64
#define PLY_TOKEN_MAX 128
#define NO_PROP ((size_t)-1)

typedef enum PlyFormat{ PLY_ASCII=0, PLY_BIN_LE, PLY_BIN_BE } PlyFormat;

typedef enum PlyType{
    PLY_NONE=0,PLY_I8,PLY_U8,PLY_I16,PLY_U16,PLY_I32,PLY_U32,PLY_F32,PLY_F64
} PlyType;

typedef struct PlyProp{
    char name[PLY_NAME_MAX];
    PlyType type;        // the item type when this is a list
    PlyType count_type;  // PLY_NONE unless this is a list
} PlyProp;

typedef struct PlyElem{
    char name[PLY_NAME_MAX];
    uint64_t count;
    PlyProp* props;
    size_t len_of_props;
    size_t cap_of_props;
} PlyElem;

// a cursor over borrowed bytes. n is the end of what may be touched, so the
// same struct bounds a whole file or a single header line.
typedef struct Buf{ const uint8_t* p; size_t n; size_t at; } Buf;

// bad_token separates "the file ran out" from "the file said something that is
// not a number", so a truncated file and a corrupt one do not report the same
typedef struct Reader{ Buf b; PlyFormat fmt; bool bad_token; } Reader;

// which property index carries what. NO_PROP when the file does not have it.
typedef struct VertexMap{ size_t x,y,z,r,g,b,a,u,v; } VertexMap;


// ---- small helpers -------------------------------------------------------

static bool host_is_big(void){
uint16_t one=1;
uint8_t probe[2];
memcpy(probe,&one,sizeof probe);
return probe[0]==0;
}

// ascii-only, because ctype.h is locale-dependent and a mesh file is not text
// in the user's locale
static bool is_ws(uint8_t c){ return c==' '||c=='\t'||c=='\r'||c=='\n'||c=='\v'||c=='\f'; }

static bool ieq(const char* a,const char* b){
for(;*a&&*b;a++,b++){
    int ca=*a,cb=*b;
    if(ca>='A'&&ca<='Z')ca+=32;
    if(cb>='A'&&cb<='Z')cb+=32;
    if(ca!=cb)return false;
}
return *a==*b;
}

static void copy_name(char* dst,size_t cap,const char* src){
size_t i=0;
for(;src[i]&&i+1<cap;i++)dst[i]=src[i];
dst[i]='\0';
}

static size_t ply_type_size(PlyType t){
switch(t){
case PLY_I8: case PLY_U8: return 1;
case PLY_I16: case PLY_U16: return 2;
case PLY_I32: case PLY_U32: case PLY_F32: return 4;
case PLY_F64: return 8;
default: return 0;
}
}

// both spellings of every type turn up in the wild, often mixed in one file
static PlyType ply_type_from_name(const char* s){
if(ieq(s,"char")||ieq(s,"int8"))return PLY_I8;
if(ieq(s,"uchar")||ieq(s,"uint8"))return PLY_U8;
if(ieq(s,"short")||ieq(s,"int16"))return PLY_I16;
if(ieq(s,"ushort")||ieq(s,"uint16"))return PLY_U16;
if(ieq(s,"int")||ieq(s,"int32"))return PLY_I32;
if(ieq(s,"uint")||ieq(s,"uint32"))return PLY_U32;
if(ieq(s,"float")||ieq(s,"float32"))return PLY_F32;
if(ieq(s,"double")||ieq(s,"float64"))return PLY_F64;
return PLY_NONE;
}

// whitespace-delimited word out of b. an over-long word is reported as
// malformed rather than truncated: a silently cut number would parse as a
// different value.
static bool tok_next(Buf* b,char* out,size_t cap,bool* too_long){
if(too_long)*too_long=false;
while(b->at<b->n&&is_ws(b->p[b->at]))b->at++;
if(b->at>=b->n)return false;
size_t k=0;
while(b->at<b->n&&!is_ws(b->p[b->at])){
    if(k+1>=cap){ if(too_long)*too_long=true; return false; }
    out[k++]=(char)b->p[b->at++];
}
out[k]='\0';
return true;
}

// header lines only. bounding the tokenizer to one line stops a line with a
// missing field from quietly eating the next line's words.
static bool next_line(Buf* b,Buf* line){
if(b->at>=b->n)return false;
size_t s=b->at,e=s;
while(e<b->n&&b->p[e]!='\n')e++;
size_t stop=e;
if(stop>s&&b->p[stop-1]=='\r')stop--;   // \r\n, as windows exporters write
b->at=(e<b->n)?e+1:e;
line->p=b->p; line->n=stop; line->at=s;
return true;
}

// decimal only, and no sign: a negative or wrapped count is garbage we must
// not turn into a huge unsigned
static bool parse_u64(const char* s,uint64_t* out){
if(!*s)return false;
uint64_t v=0;
for(const char* c=s;*c;c++){
    if(*c<'0'||*c>'9')return false;
    uint64_t d=(uint64_t)(*c-'0');
    if(v>(UINT64_MAX-d)/10)return false;
    v=v*10+d;
}
*out=v;
return true;
}


// ---- value reading -------------------------------------------------------

// one scalar in whatever the header declared. an ascii row is just numbers, so
// there the declared type only matters later, for colour scaling.
static bool read_value(Reader* rd,PlyType t,double* out){
if(rd->fmt==PLY_ASCII){
    char tok[PLY_TOKEN_MAX];
    bool too_long=false;
    if(!tok_next(&rd->b,tok,sizeof tok,&too_long)){ rd->bad_token=rd->bad_token||too_long; return false; }
    char* end=NULL;
    double v=strtod(tok,&end);
    if(end==tok||*end!='\0'){ rd->bad_token=true; return false; }   // words where a number belongs
    *out=v;
    return true;
}
size_t sz=ply_type_size(t);
uint8_t raw[8];
if(sz==0||rd->b.n-rd->b.at<sz)return false;
memcpy(raw,rd->b.p+rd->b.at,sz);
rd->b.at+=sz;
// swap only on a mismatch: the host is not assumed little-endian either, so
// the declared format is compared against a measured host order
if((rd->fmt==PLY_BIN_BE)!=host_is_big())
    for(size_t i=0;i<sz/2;i++){
        uint8_t tmp=raw[i];
        raw[i]=raw[sz-1-i];
        raw[sz-1-i]=tmp;
    }
switch(t){
case PLY_I8:{ int8_t v; memcpy(&v,raw,1); *out=(double)v; break; }
case PLY_U8:{ uint8_t v; memcpy(&v,raw,1); *out=(double)v; break; }
case PLY_I16:{ int16_t v; memcpy(&v,raw,2); *out=(double)v; break; }
case PLY_U16:{ uint16_t v; memcpy(&v,raw,2); *out=(double)v; break; }
case PLY_I32:{ int32_t v; memcpy(&v,raw,4); *out=(double)v; break; }
case PLY_U32:{ uint32_t v; memcpy(&v,raw,4); *out=(double)v; break; }
case PLY_F32:{ float v; memcpy(&v,raw,4); *out=(double)v; break; }
case PLY_F64:{ double v; memcpy(&v,raw,8); *out=v; break; }
default: return false;
}
return true;
}

// a list count is file data like any other: garbage here is exactly how a
// corrupt file walks the cursor into the middle of the next element
static bool read_list_count(Reader* rd,const PlyProp* p,uint64_t* out){
double v;
if(!read_value(rd,p->count_type,&v))return false;
if(!(v>=0.0)||v>4294967295.0){ rd->bad_token=true; return false; }   // !(>=) also rejects nan
*out=(uint64_t)v;
return true;
}

// a value we could not read is either the end of a truncated file or nonsense
// written where a number belongs; the caller should not have to guess which
static MeshResult rd_err(const Reader* rd){ return rd->bad_token?MESH_ERR_FORMAT:MESH_ERR_READ; }

static bool skip_values(Reader* rd,PlyType t,uint64_t n){
if(rd->fmt!=PLY_ASCII){
    size_t sz=ply_type_size(t);
    if(sz==0)return false;
    if(n>(uint64_t)((rd->b.n-rd->b.at)/sz))return false;   // truncated
    rd->b.at+=(size_t)n*sz;
    return true;
}
double v;
for(uint64_t i=0;i<n;i++)if(!read_value(rd,t,&v))return false;
return true;
}

// false when any list property makes the row width vary
static bool elem_fixed_stride(const PlyElem* e,size_t* out){
size_t s=0;
for(size_t i=0;i<e->len_of_props;i++){
    if(e->props[i].count_type!=PLY_NONE)return false;
    size_t sz=ply_type_size(e->props[i].type);
    if(sz==0||s>SIZE_MAX-sz)return false;
    s+=sz;
}
*out=s;
return true;
}

// elements we do not care about still have to be stepped over exactly
static MeshResult skip_element(Reader* rd,const PlyElem* e){
size_t stride=0;
if(rd->fmt!=PLY_ASCII&&elem_fixed_stride(e,&stride)){
    if(stride!=0){
        if(e->count>(uint64_t)((rd->b.n-rd->b.at)/stride))return MESH_ERR_READ;   // truncated
        rd->b.at+=(size_t)e->count*stride;
    }
    return MESH_OK;
}
for(uint64_t i=0;i<e->count;i++)
    for(size_t j=0;j<e->len_of_props;j++){
        const PlyProp* p=&e->props[j];
        if(p->count_type!=PLY_NONE){
            uint64_t c;
            if(!read_list_count(rd,p,&c)||!skip_values(rd,p->type,c))return rd_err(rd);
        }else{
            double v;
            if(!read_value(rd,p->type,&v))return rd_err(rd);
        }
    }
return MESH_OK;
}


// ---- header --------------------------------------------------------------

static void free_elems(PlyElem* e,size_t n){
for(size_t i=0;i<n;i++)free(e[i].props);
free(e);
}

static bool push_elem(PlyElem** arr,size_t* len,size_t* cap,const PlyElem* e){
if(*len==*cap){
    size_t want=*cap?*cap*2:8;
    if(want<=*cap||want>SIZE_MAX/sizeof(PlyElem))return false;
    PlyElem* grown=realloc(*arr,want*sizeof(PlyElem));
    if(!grown)return false;
    *arr=grown;
    *cap=want;
}
(*arr)[(*len)++]=*e;
return true;
}

static bool push_prop(PlyElem* e,const PlyProp* p){
if(e->len_of_props==e->cap_of_props){
    size_t want=e->cap_of_props?e->cap_of_props*2:8;
    if(want<=e->cap_of_props||want>SIZE_MAX/sizeof(PlyProp))return false;
    PlyProp* grown=realloc(e->props,want*sizeof(PlyProp));
    if(!grown)return false;
    e->props=grown;
    e->cap_of_props=want;
}
e->props[e->len_of_props++]=*p;
return true;
}

// consumes b up to and including the end_header line, so b is left sitting on
// the first byte of data
static MeshResult parse_header(Buf* b,PlyElem** out_elems,size_t* out_n,PlyFormat* out_fmt){
PlyElem* elems=NULL;
size_t n=0,cap=0,cur=NO_PROP;
bool have_format=false,ended=false;
char tok[PLY_TOKEN_MAX];
MeshResult r=MESH_ERR_FORMAT;
Buf line;

if(!next_line(b,&line)||!tok_next(&line,tok,sizeof tok,NULL)||!ieq(tok,"ply"))goto fail;

while(next_line(b,&line)){
    if(!tok_next(&line,tok,sizeof tok,NULL))continue;      // blank lines are harmless
    if(ieq(tok,"comment")||ieq(tok,"obj_info"))continue;
    if(ieq(tok,"end_header")){ ended=true; break; }
    if(ieq(tok,"format")){
        if(!tok_next(&line,tok,sizeof tok,NULL))goto fail;
        if(ieq(tok,"ascii"))*out_fmt=PLY_ASCII;
        else if(ieq(tok,"binary_little_endian"))*out_fmt=PLY_BIN_LE;
        else if(ieq(tok,"binary_big_endian"))*out_fmt=PLY_BIN_BE;
        else{ r=MESH_ERR_UNSUPPORTED; goto fail; }
        have_format=true;
        continue;
    }
    if(ieq(tok,"element")){
        PlyElem e={0};
        if(!tok_next(&line,tok,sizeof tok,NULL))goto fail;
        copy_name(e.name,sizeof e.name,tok);
        if(!tok_next(&line,tok,sizeof tok,NULL)||!parse_u64(tok,&e.count))goto fail;
        if(!push_elem(&elems,&n,&cap,&e)){ r=MESH_ERR_OOM; goto fail; }
        cur=n-1;
        continue;
    }
    if(ieq(tok,"property")){
        if(cur==NO_PROP)goto fail;                    // a property before any element
        PlyProp p={0};
        if(!tok_next(&line,tok,sizeof tok,NULL))goto fail;
        if(ieq(tok,"list")){
            if(!tok_next(&line,tok,sizeof tok,NULL))goto fail;
            p.count_type=ply_type_from_name(tok);
            if(!tok_next(&line,tok,sizeof tok,NULL))goto fail;
            p.type=ply_type_from_name(tok);
            if(p.count_type==PLY_NONE||p.type==PLY_NONE){ r=MESH_ERR_UNSUPPORTED; goto fail; }
        }else{
            p.type=ply_type_from_name(tok);
            p.count_type=PLY_NONE;
            if(p.type==PLY_NONE){ r=MESH_ERR_UNSUPPORTED; goto fail; }
        }
        if(!tok_next(&line,tok,sizeof tok,NULL))goto fail;
        copy_name(p.name,sizeof p.name,tok);
        if(!push_prop(&elems[cur],&p)){ r=MESH_ERR_OOM; goto fail; }
        continue;
    }
    // anything else is ignored: ply defines no other keywords, and rejecting
    // an unknown one would throw away files that are otherwise readable
}
if(!ended||!have_format)goto fail;
*out_elems=elems;
*out_n=n;
return MESH_OK;

fail:
free_elems(elems,n);
*out_elems=NULL;
*out_n=0;
return r;
}


// ---- element bodies ------------------------------------------------------

static void build_vertex_map(const PlyElem* e,VertexMap* m){
m->x=m->y=m->z=m->r=m->g=m->b=m->a=m->u=m->v=NO_PROP;
for(size_t i=0;i<e->len_of_props;i++){
    if(e->props[i].count_type!=PLY_NONE)continue;     // a coordinate list is nonsense
    const char* nm=e->props[i].name;
    if(m->x==NO_PROP&&ieq(nm,"x"))m->x=i;
    else if(m->y==NO_PROP&&ieq(nm,"y"))m->y=i;
    else if(m->z==NO_PROP&&ieq(nm,"z"))m->z=i;
    else if(m->r==NO_PROP&&(ieq(nm,"red")||ieq(nm,"r")))m->r=i;
    else if(m->g==NO_PROP&&(ieq(nm,"green")||ieq(nm,"g")))m->g=i;
    else if(m->b==NO_PROP&&(ieq(nm,"blue")||ieq(nm,"b")))m->b=i;
    else if(m->a==NO_PROP&&(ieq(nm,"alpha")||ieq(nm,"a")))m->a=i;
    // ply has no standard name for texture coordinates; these are the four
    // spellings the common exporters use. "u"/"v" are tested last so they cannot
    // shadow anything above.
    else if(m->u==NO_PROP&&(ieq(nm,"s")||ieq(nm,"texture_u")||ieq(nm,"texture_s")||ieq(nm,"u")))m->u=i;
    else if(m->v==NO_PROP&&(ieq(nm,"t")||ieq(nm,"texture_v")||ieq(nm,"texture_t")||ieq(nm,"v")))m->v=i;
}
}

// colour is either 0-255 integers or 0-1 floats, and the declared type is the
// only thing in the file that says which
static uint32_t chan_to_byte(double v,PlyType t){
if(t==PLY_F32||t==PLY_F64)v*=255.0;
if(!(v>0.0))return 0;          // the negated form also catches nan
if(v>255.0)return 255;
return (uint32_t)(v+0.5);
}

static MeshResult read_vertices(Reader* rd,const PlyElem* e,const VertexMap* m,Vectex* v){
bool has_colour=(m->r!=NO_PROP&&m->g!=NO_PROP&&m->b!=NO_PROP);
bool has_uv=(m->u!=NO_PROP&&m->v!=NO_PROP);
for(size_t i=0;i<(size_t)e->count;i++){
    double x=0.0,y=0.0,z=0.0,tu=0.0,tv=0.0;
    uint32_t ch[4]={0,0,0,255};                       // r,g,b,a
    for(size_t j=0;j<e->len_of_props;j++){
        const PlyProp* p=&e->props[j];
        if(p->count_type!=PLY_NONE){                  // exporters do hang lists off vertices
            uint64_t c;
            if(!read_list_count(rd,p,&c)||!skip_values(rd,p->type,c))return rd_err(rd);
            continue;
        }
        double val;
        if(!read_value(rd,p->type,&val))return rd_err(rd);
        if(j==m->x)x=val;
        else if(j==m->y)y=val;
        else if(j==m->z)z=val;
        else if(j==m->r)ch[0]=chan_to_byte(val,p->type);
        else if(j==m->g)ch[1]=chan_to_byte(val,p->type);
        else if(j==m->b)ch[2]=chan_to_byte(val,p->type);
        else if(j==m->a)ch[3]=chan_to_byte(val,p->type);
        else if(j==m->u)tu=val;
        else if(j==m->v)tv=val;
    }
    v[i].x=x;
    v[i].y=y;
    v[i].z=z;
    v[i].u=has_uv?mesh_wrap_uv(tu):0.0f;
    // ply inherits obj's bottom-left texture origin, so v is mirrored the same
    // way. If a ply ever turns up textured upside down, this is the line.
    v[i].v=has_uv?mesh_wrap_uv(1.0-tv):0.0f;
    v[i].colour=has_colour?((ch[3]<<24)|(ch[0]<<16)|(ch[1]<<8)|ch[2]):MESH_DEFAULT_COLOUR;
}
return MESH_OK;
}

// ply has no material block, so exporters record the model's image as a header
// comment: "comment TextureFile wall.png". The header parser ignores comments
// -- rightly, they carry no structure -- so it is picked out separately here.
// Returns a malloc'd name, or NULL when the header has no such comment.
static char* texture_comment(const uint8_t* data,size_t header_len){
Buf hb={data,header_len,0};
Buf line;
char tok[PLY_TOKEN_MAX];
while(next_line(&hb,&line)){
    if(!tok_next(&line,tok,sizeof tok,NULL))continue;
    if(!ieq(tok,"comment"))continue;
    if(!tok_next(&line,tok,sizeof tok,NULL))continue;
    if(!ieq(tok,"texturefile")&&!ieq(tok,"texturefile:"))continue;
    if(!tok_next(&line,tok,sizeof tok,NULL))continue;
    size_t n=strlen(tok);
    char* name=malloc(n+1);
    if(!name)return NULL;
    memcpy(name,tok,n+1);
    return name;
}
return NULL;
}

// Every vertex colour averaged into one. A ply usually carries colour per vertex
// and no image at all, and the rasterizer only reads the texture -- so the flat
// texture a colour-only ply falls back to is at least the right overall shade
// rather than a default grey.
static uint32_t average_colour(const Vectex* v,uint64_t n){
if(!v||n==0)return MESH_DEFAULT_COLOUR;
uint64_t r=0,g=0,b=0;
for(uint64_t i=0;i<n;i++){
    r+=(v[i].colour>>16)&0xFFu;
    g+=(v[i].colour>>8)&0xFFu;
    b+=v[i].colour&0xFFu;
}
return 0xFF000000u|((uint32_t)(r/n)<<16)|((uint32_t)(g/n)<<8)|(uint32_t)(b/n);
}

static bool push_tri(uint64_t** arr,size_t* len,size_t* cap,uint64_t a,uint64_t b,uint64_t c){
if(*len+3>*cap){
    size_t want=*cap?*cap*2:192;
    if(want<*len+3)want=*len+3;
    if(want<*len||want>SIZE_MAX/sizeof(uint64_t))return false;
    uint64_t* grown=realloc(*arr,want*sizeof(uint64_t));
    if(!grown)return false;
    *arr=grown;
    *cap=want;
}
(*arr)[(*len)++]=a;
(*arr)[(*len)++]=b;
(*arr)[(*len)++]=c;
return true;
}

// an index the vertex array cannot answer is not something to emit and hope:
// render() would read past the end of the array
static bool index_from(double v,uint64_t vcount,uint64_t* out){
if(!(v>=0.0)||!(v<(double)vcount))return false;
*out=(uint64_t)v;
return true;
}

static MeshResult read_faces(Reader* rd,const PlyElem* e,size_t list_prop,uint64_t vcount,
                             uint64_t** tris,size_t* len,size_t* cap){
for(uint64_t i=0;i<e->count;i++)
    for(size_t j=0;j<e->len_of_props;j++){
        const PlyProp* p=&e->props[j];
        if(j!=list_prop){                              // per-face normals, material ids, ...
            if(p->count_type!=PLY_NONE){
                uint64_t c;
                if(!read_list_count(rd,p,&c)||!skip_values(rd,p->type,c))return rd_err(rd);
            }else{
                double v;
                if(!read_value(rd,p->type,&v))return rd_err(rd);
            }
            continue;
        }
        uint64_t c;
        if(!read_list_count(rd,p,&c))return rd_err(rd);
        // fan triangulation, emitted as we walk: (i0,i1,i2),(i0,i2,i3),...
        // a face with fewer than 3 indices is a point or an edge, so it is
        // skipped rather than treated as a broken file
        uint64_t first=0,prev=0;
        for(uint64_t k=0;k<c;k++){
            double val;
            if(!read_value(rd,p->type,&val))return rd_err(rd);
            uint64_t idx;
            if(!index_from(val,vcount,&idx))return MESH_ERR_FORMAT;
            if(k==0)first=idx;
            else if(k>=2&&!push_tri(tris,len,cap,first,prev,idx))return MESH_ERR_OOM;
            prev=idx;
        }
    }
return MESH_OK;
}

static MeshResult slurp(const char* path,uint8_t** out_data,size_t* out_size){
FILE* f=fopen(path,"rb");
if(!f)return MESH_ERR_OPEN;
if(fseek(f,0,SEEK_END)!=0){ fclose(f); return MESH_ERR_READ; }
long end=ftell(f);
if(end<0||fseek(f,0,SEEK_SET)!=0){ fclose(f); return MESH_ERR_READ; }
size_t size=(size_t)end;
if(size<4){ fclose(f); return MESH_ERR_FORMAT; }      // shorter than "ply\n"
uint8_t* data=malloc(size);
if(!data){ fclose(f); return MESH_ERR_OOM; }
size_t got=fread(data,1,size,f);
fclose(f);
if(got!=size){ free(data); return MESH_ERR_READ; }
*out_data=data;
*out_size=size;
return MESH_OK;
}


MeshResult mesh_load_ply(const char* path,Object* out){
if(!path||!out)return MESH_ERR_OPEN;
*out=(Object){0};

uint8_t* data=NULL;
size_t size=0;
PlyElem* elems=NULL;
size_t nelems=0;
Vectex* verts=NULL;
uint64_t* tris=NULL;
size_t n_conn=0,conn_cap=0;

MeshResult r=slurp(path,&data,&size);
if(r!=MESH_OK)return r;

PlyFormat fmt=PLY_ASCII;
Buf b={data,size,0};
r=parse_header(&b,&elems,&nelems,&fmt);
if(r!=MESH_OK)goto done;

// every element instance costs at least one byte in either encoding, so a
// count bigger than what is left of the file is garbage -- caught here, before
// it reaches malloc as a multi-gigabyte request
size_t left=size-b.at;
for(size_t i=0;i<nelems;i++)
    if(elems[i].count>(uint64_t)left){ r=MESH_ERR_FORMAT; goto done; }

size_t vi=NO_PROP,fi=NO_PROP,flist=NO_PROP;
for(size_t i=0;i<nelems;i++){
    if(vi==NO_PROP&&ieq(elems[i].name,"vertex"))vi=i;
    if(fi==NO_PROP&&ieq(elems[i].name,"face"))fi=i;
}
if(vi==NO_PROP){ r=MESH_ERR_FORMAT; goto done; }

VertexMap map;
build_vertex_map(&elems[vi],&map);
if(map.x==NO_PROP||map.y==NO_PROP||map.z==NO_PROP){ r=MESH_ERR_FORMAT; goto done; }

if(fi!=NO_PROP)
    for(size_t i=0;i<elems[fi].len_of_props;i++){
        const PlyProp* p=&elems[fi].props[i];
        if(p->count_type!=PLY_NONE&&(ieq(p->name,"vertex_indices")||ieq(p->name,"vertex_index"))){
            flist=i;
            break;
        }
    }
// a face element with no index list carries no geometry we can use; it gets
// skipped like any other element and the load ends as EMPTY
if(flist==NO_PROP)fi=NO_PROP;

uint64_t vcount=elems[vi].count;
if(vcount>SIZE_MAX/sizeof(Vectex)){ r=MESH_ERR_OOM; goto done; }
if(vcount){
    verts=malloc((size_t)vcount*sizeof(Vectex));
    if(!verts){ r=MESH_ERR_OOM; goto done; }
}

Reader rd={.b=b,.fmt=fmt};
for(size_t i=0;i<nelems;i++){
    if(i==vi)r=read_vertices(&rd,&elems[i],&map,verts);
    else if(i==fi)r=read_faces(&rd,&elems[i],flist,vcount,&tris,&n_conn,&conn_cap);
    else r=skip_element(&rd,&elems[i]);
    if(r!=MESH_OK)goto done;
}
if(n_conn==0){ r=MESH_ERR_EMPTY; goto done; }

out->vertices=verts;
out->len_of_vertices=vcount;
out->connectors_sequence=tris;
out->len_of_connectors=(uint64_t)n_conn;

// b.at is where the header ended, so data[0..b.at) is exactly the header text
char* texname=texture_comment(data,b.at);
char* texpath=texname?mesh_path_sibling(path,texname):NULL;
free(texname);
Image img={0};
bool textured=texpath&&image_decode_file(texpath,MESH_TEXTURE_MAX_DIM,&img,NULL,0);
free(texpath);
if(textured){
    out->texture=img.pixels;
    out->texture_width=img.width;
    out->texture_height=img.height;
}else if(!mesh_set_flat_texture(out,map.r!=NO_PROP?average_colour(verts,vcount):MESH_DEFAULT_COLOUR)){
    *out=(Object){0};
    r=MESH_ERR_OOM;
    goto done;
}
verts=NULL;
tris=NULL;
r=MESH_OK;

done:
free(verts);
free(tris);
free_elems(elems,nelems);
free(data);
if(r!=MESH_OK){
    free(out->texture);
    *out=(Object){0};
}
return r;
}

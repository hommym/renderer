#include "mesh.h"
#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

// wavefront obj, text. v, vt and f carry the geometry; mtllib/usemtl carry the
// diffuse map. vn/o/g/s are read past -- there is no lighting downstream of
// Object, so a normal has nowhere to go.
//
// The awkward part of obj is that positions and texture coordinates are indexed
// SEPARATELY: "f 4/1413/4" means position 4 with uv 1413, and the same position
// appears again on another face with a different uv wherever the model is cut
// open for its atlas. Object has one array with one uv per vertex, so a corner
// is keyed on the (position, uv) PAIR and a position used with three different
// uvs becomes three vertices. Nothing is de-duplicated by position alone: that
// would pick one of the three uvs arbitrarily and smear the texture.
//
// the file is untrusted. every count, index and size computation below is
// bounds-checked against what was actually allocated before it is used, and
// every failure path frees the partial arrays and leaves *out zeroed.

#define OBJ_CHUNK 8192u      // fread granularity, nothing to do with line length
#define OBJ_MAX_V_FLOATS 8u  // x y z w r g b + slack; extra tokens on a v are junk

typedef struct Reader{
FILE* f;
char chunk[OBJ_CHUNK];
size_t pos,fill;        // unconsumed window is chunk[pos..fill)
char* line;             // grown, never truncated: obj lines have no length limit
size_t line_len,line_cap;
bool eof;
} Reader;

typedef struct VertexBuf{ Vectex* data; size_t len,cap; } VertexBuf;
typedef struct IndexBuf{ uint64_t* data; size_t len,cap; } IndexBuf;
typedef struct Uv{ float u,v; } Uv;
typedef struct UvBuf{ Uv* data; size_t len,cap; } UvBuf;

// One entry per emitted vertex, chained off the position it came from. Lookup
// walks the chain for a position -- typically one or two links, since a corner
// is only split where the atlas seam runs -- so no hash table is needed and the
// nodes cost exactly as much as the output they describe.
typedef struct CornerNode{ uint64_t uv; uint64_t out; uint64_t next; } CornerNode;
typedef struct CornerMap{
    CornerNode* nodes; size_t len,cap;
    uint64_t*   head;  size_t head_len,head_cap;   // per position: node index + 1, 0 = none
} CornerMap;

// A material as far as this renderer is concerned: a name to match usemtl
// against, a diffuse map to sample and a flat colour to fall back on.
typedef struct Material{
    char* name;
    char* map_kd;      // path as written in the mtl, relative to the mtl's directory
    uint32_t kd;
    size_t tris;       // how much of the mesh is drawn with it
} Material;
typedef struct MatBuf{ Material* data; size_t len,cap; } MatBuf;

// geometric growth. a realloc per element makes a million-vertex load a million
// copies; doubling keeps it amortised. returns NULL and leaves the old block
// untouched on failure, so the caller can still free what it has.
static void* grow_array(void* data,size_t* cap,size_t elem){
size_t want=*cap? *cap*2 : 256;
if(want<*cap||want>SIZE_MAX/elem)return NULL; // want*elem must not wrap
void* p=realloc(data,want*elem);
if(!p)return NULL;
*cap=want;
return p;
}

static bool push_vertex(VertexBuf* b,Vectex v){
if(b->len==b->cap){
    void* p=grow_array(b->data,&b->cap,sizeof *b->data);
    if(!p)return false;
    b->data=p;
}
b->data[b->len++]=v;
return true;
}

static bool push_uv(UvBuf* b,Uv v){
if(b->len==b->cap){
    void* p=grow_array(b->data,&b->cap,sizeof *b->data);
    if(!p)return false;
    b->data=p;
}
b->data[b->len++]=v;
return true;
}

static bool push_index(IndexBuf* b,uint64_t i){
if(b->len==b->cap){
    void* p=grow_array(b->data,&b->cap,sizeof *b->data);
    if(!p)return false;
    b->data=p;
}
b->data[b->len++]=i;
return true;
}

static bool line_append(Reader* r,const char* src,size_t n){
if(n>SIZE_MAX-1-r->line_len)return false;
size_t need=r->line_len+n+1; // +1: strtod/strtoll need the line NUL-terminated
while(need>r->line_cap){
    void* p=grow_array(r->line,&r->line_cap,1);
    if(!p)return false;
    r->line=p;
}
memcpy(r->line+r->line_len,src,n);
r->line_len+=n;
r->line[r->line_len]='\0';
return true;
}

// 1 = line ready, 0 = clean end of file, -1 = read error, -2 = out of memory.
// a line is accumulated across chunk boundaries instead of into a fixed buffer:
// a truncated f line would silently become a different, valid-looking face.
static int next_line(Reader* r){
r->line_len=0;
if(r->line)r->line[0]='\0';
for(;;){
    if(r->pos==r->fill){
        if(r->eof)return r->line_len?1:0;
        r->fill=fread(r->chunk,1,sizeof r->chunk,r->f);
        r->pos=0;
        if(r->fill==0){
            r->eof=true;
            if(ferror(r->f))return -1;
            return r->line_len?1:0; // last line may lack a trailing newline
        }
    }
    const char* start=r->chunk+r->pos;
    size_t left=r->fill-r->pos;
    const char* nl=memchr(start,'\n',left);
    size_t seg=nl?(size_t)(nl-start):left;
    if(!line_append(r,start,seg))return -2;
    r->pos+=nl?seg+1:seg;
    if(!nl)continue;
    // a trailing backslash continues the statement onto the next physical line
    // (obj spec, general statement syntax). exporters use it to wrap long v and
    // f lines, so treating it as a terminator rejects conformant files. count
    // the run so an escaped backslash is not mistaken for a continuation.
    size_t bs=r->line_len;
    while(bs>0&&r->line[bs-1]=='\\')bs--;
    if((r->line_len-bs)%2==1){
        r->line[r->line_len-1]=' ';   // keep the joined tokens separated
        continue;
    }
    return 1;
}
}

// '\r' counts as whitespace so crlf files need no separate stripping pass
static const char* skip_ws(const char* p){
while(*p==' '||*p=='\t'||*p=='\r')p++;
return p;
}

static const char* skip_token(const char* p){
while(*p&&*p!=' '&&*p!='\t'&&*p!='\r')p++;
return p;
}

static uint32_t chan(double v,double scale){
double s=v*scale;
if(!(s>0.0))return 0; // negatives and NaN both floor here
if(s>255.0)return 255;
return (uint32_t)(s+0.5);
}

static uint32_t pack_colour(double r,double g,double b){
// writers disagree on range: meshlab emits 0..1, a few emit 0..255. a channel
// above 1.0 can only mean the byte range, and the triple shares one range.
double scale=(r>1.0||g>1.0||b>1.0)?1.0:255.0;
return 0xFF000000u|(chan(r,scale)<<16)|(chan(g,scale)<<8)|chan(b,scale);
}

// p points just past the 'v'. false means the line is malformed, not empty.
static bool parse_vertex(const char* p,Vectex* v){
double f[OBJ_MAX_V_FLOATS];
size_t n=0;
while(n<OBJ_MAX_V_FLOATS){
    p=skip_ws(p);
    if(*p=='\0')break;
    char* end;
    double d=strtod(p,&end);
    if(end==p)return false; // a non-numeric token in a position that must be a float
    f[n++]=d;
    p=end;
}
if(n<3)return false;
v->x=f[0]; v->y=f[1]; v->z=f[2];
// non-standard vertex colour. six floats is "x y z r g b"; seven is "x y z w
// r g b", so the colour is the trailing triple in either case.
v->colour = n>=6 ? pack_colour(f[n-3],f[n-2],f[n-1]) : MESH_DEFAULT_COLOUR;
return true;
}

// ---- (position, uv) -> vertex ---------------------------------------------

// Grows the per-position chain heads to cover position `i`. Called as positions
// are read, so the table is always exactly as long as the position list.
static bool map_reserve(CornerMap* m,size_t i){
while(i>=m->head_cap){
    void* p=grow_array(m->head,&m->head_cap,sizeof *m->head);
    if(!p)return false;
    m->head=p;
}
while(m->head_len<=i)m->head[m->head_len++]=0;
return true;
}

// Returns the output vertex index for (pos, uv), emitting a new one the first
// time that pair is seen. uv is SIZE_MAX for a face reference with no vt.
static bool map_get(CornerMap* m,VertexBuf* out,const VertexBuf* pos,const UvBuf* uvs,
                    size_t pi,size_t ti,uint64_t* out_i){
if(!map_reserve(m,pi))return false;
uint64_t key=(ti==SIZE_MAX)?UINT64_MAX:(uint64_t)ti;
for(uint64_t n=m->head[pi];n;n=m->nodes[n-1].next){
    if(m->nodes[n-1].uv==key){ *out_i=m->nodes[n-1].out; return true; }
}

Vectex v=pos->data[pi];
if(ti==SIZE_MAX){
    v.u=0.0f;v.v=0.0f;
}else{
    v.u=mesh_wrap_uv(uvs->data[ti].u);
    // obj puts the uv origin at the BOTTOM-left and the image (and this
    // renderer, which indexes texture rows from the top) puts it at the top, so
    // v is mirrored. glTF needs no such flip: it is already top-left.
    v.v=mesh_wrap_uv(1.0-(double)uvs->data[ti].v);
}
if(!push_vertex(out,v))return false;

if(m->len==m->cap){
    void* p=grow_array(m->nodes,&m->cap,sizeof *m->nodes);
    if(!p)return false;
    m->nodes=p;
}
m->nodes[m->len]=(CornerNode){.uv=key,.out=(uint64_t)(out->len-1),.next=m->head[pi]};
m->head[pi]=(uint64_t)(++m->len);   // stored +1 so 0 can mean "empty"
*out_i=(uint64_t)(out->len-1);
return true;
}

// ---- materials -------------------------------------------------------------

static char* dup_str(const char* s,size_t n){
char* p=malloc(n+1);
if(!p)return NULL;
memcpy(p,s,n);
p[n]='\0';
return p;
}

// Everything up to the end of the line, trailing whitespace trimmed. A map_Kd
// filename may contain spaces, so it cannot be taken as a single token.
static char* rest_of_line(const char* p){
const char* e=p+strlen(p);
while(e>p&&(e[-1]==' '||e[-1]=='\t'||e[-1]=='\r'))e--;
return e>p?dup_str(p,(size_t)(e-p)):NULL;
}

static Material* mat_find(MatBuf* b,const char* name){
for(size_t i=0;i<b->len;i++)
    if(b->data[i].name&&strcmp(b->data[i].name,name)==0)return &b->data[i];
return NULL;
}

static Material* mat_add(MatBuf* b,const char* name){
Material* e=mat_find(b,name);
if(e)return e;
if(b->len==b->cap){
    void* p=grow_array(b->data,&b->cap,sizeof *b->data);
    if(!p)return NULL;
    b->data=p;
}
Material* m=&b->data[b->len];
*m=(Material){0};
m->name=dup_str(name,strlen(name));
m->kd=MESH_DEFAULT_COLOUR;
if(!m->name)return NULL;
b->len++;
return m;
}

static void mat_free(MatBuf* b){
for(size_t i=0;i<b->len;i++){ free(b->data[i].name); free(b->data[i].map_kd); }
free(b->data);
*b=(MatBuf){0};
}

// map_Kd takes texture options before the filename ("-s 1 1 1 wall.jpg"), each a
// '-' token followed by its arguments. The arguments are numbers or on/off, so
// skipping every leading token that is one of those lands on the filename --
// which then runs to the end of the line, spaces included.
static char* parse_map_kd(const char* p){
for(;;){
    p=skip_ws(p);
    if(*p!='-')break;
    p=skip_token(p);            // the option itself
    for(;;){
        const char* q=skip_ws(p);
        if(*q=='\0'||*q=='-')break;
        char* end;
        strtod(q,&end);
        bool numeric=(end!=q&&(*end==' '||*end=='\t'||*end=='\r'||*end=='\0'));
        bool flag=(strncmp(q,"on",2)==0||strncmp(q,"off",3)==0);
        if(!numeric&&!flag)break;   // not an argument: this is the filename
        p=skip_token(q);
    }
}
return rest_of_line(p);
}

// Reads a .mtl into b. A missing or unreadable mtl is not a load failure: the
// geometry is still good and the mesh falls back to a flat colour.
static void load_mtl(const char* path,MatBuf* b){
FILE* f=fopen(path,"rb");
if(!f)return;
Reader r={.f=f};
Material* cur=NULL;
for(;;){
    int st=next_line(&r);
    if(st<=0)break;
    if(!r.line)break;
    char* hash=memchr(r.line,'#',r.line_len);
    if(hash)*hash='\0';
    const char* p=skip_ws(r.line);
    if(*p=='\0')continue;

    if(strncmp(p,"newmtl",6)==0&&(p[6]==' '||p[6]=='\t')){
        char* name=rest_of_line(skip_ws(p+6));
        if(!name)continue;
        cur=mat_add(b,name);
        free(name);
    }else if(cur&&strncmp(p,"map_Kd",6)==0&&(p[6]==' '||p[6]=='\t')){
        // first map_Kd wins: a material that names two is malformed, and picking
        // the later one silently would depend on file order
        if(!cur->map_kd){
            char* rel=parse_map_kd(p+6);
            // a texture path is resolved against the MTL's directory, not the
            // obj's -- they are usually the same but nothing requires it
            if(rel)cur->map_kd=mesh_path_sibling(path,rel);
            free(rel);
        }
    }else if(cur&&p[0]=='K'&&p[1]=='d'&&(p[2]==' '||p[2]=='\t')){
        double c[3]={1.0,1.0,1.0};
        const char* q=p+2;
        size_t n=0;
        while(n<3){
            q=skip_ws(q);
            char* end;
            double d=strtod(q,&end);
            if(end==q)break;
            c[n++]=d;
            q=end;
        }
        if(n==3)cur->kd=pack_colour(c[0],c[1],c[2]);
    }
}
fclose(f);
free(r.line);
}

// obj indices are 1-based, and a negative one counts back from the end of the
// vertex list AS IT STANDS on this line -- so it is resolved against the live
// count, never the final one.
static bool resolve_index(long long idx,size_t vcount,uint64_t* out_i){
if(idx==0)return false; // there is no vertex 0
if(idx>0){
    unsigned long long i=(unsigned long long)idx-1ull;
    if(i>=(unsigned long long)vcount)return false;
    *out_i=(uint64_t)i;
    return true;
}
// -(idx+1)+1 rather than -idx: LLONG_MIN has no positive counterpart
unsigned long long back=(unsigned long long)(-(idx+1))+1ull;
if(back>(unsigned long long)vcount)return false;
*out_i=(uint64_t)((unsigned long long)vcount-back);
return true;
}

// One "v", "v/vt", "v//vn" or "v/vt/vn" reference. Returns the end of the token,
// or NULL if it is not one of those four shapes -- a half-written reference is a
// malformed file, not a face to guess at.
static const char* parse_face_ref(const char* q,long long* vi,long long* ti,bool* have_t){
char* end;
*have_t=false;
*ti=0;
*vi=strtoll(q,&end,10);
if(end==q)return NULL;
if(*end!='/')return end;

const char* r=end+1;
if(*r!='/'){
    char* e2;
    long long t=strtoll(r,&e2,10);
    if(e2==r)return NULL;
    *ti=t;*have_t=true;
    end=e2;
}else end=(char*)r;              // "v//vn": the uv slot is empty

if(*end=='/'){
    const char* r2=end+1;
    char* e3;
    strtoll(r2,&e3,10);          // the normal index is parsed only to skip it
    if(e3==r2)return NULL;
    end=e3;
}
return end;
}

MeshResult mesh_load_obj(const char* path, Object* out){
if(!out)return MESH_ERR_FORMAT;
*out=(Object){0};
if(!path)return MESH_ERR_OPEN;
// "rb": the tokeniser already treats '\r' as whitespace, so text mode would
// only add a platform-dependent second stripping rule
FILE* f=fopen(path,"rb");
if(!f)return MESH_ERR_OPEN;

Reader r={.f=f};
VertexBuf pos={0};   // "v" lines, in file order
UvBuf     uvs={0};   // "vt" lines, in file order
VertexBuf vb={0};    // the vertices actually emitted, one per (pos,uv) pair
IndexBuf ib={0};
IndexBuf refs={0};   // one face's corner indices, reused so an n-gon costs no churn
CornerMap map={0};
MatBuf mats={0};
// an INDEX, not a pointer: a later mtllib can grow the material array and move
// it, which would leave a pointer taken before it dangling
size_t cur_mat=SIZE_MAX;
MeshResult res=MESH_OK;

for(;;){
    int s=next_line(&r);
    if(s==0)break;
    if(s==-1){ res=MESH_ERR_READ; goto done; }
    if(s==-2){ res=MESH_ERR_OOM; goto done; }
    if(r.line_len==0||!r.line)continue;
    char* hash=memchr(r.line,'#',r.line_len);
    if(hash)*hash='\0'; // '#' opens a comment anywhere on the line
    const char* p=skip_ws(r.line);
    if(*p=='\0')continue;

    if(p[0]=='v'&&(p[1]==' '||p[1]=='\t')){
        Vectex v;
        if(!parse_vertex(p+1,&v)){ res=MESH_ERR_FORMAT; goto done; }
        v.u=0.0f;v.v=0.0f;
        if(!push_vertex(&pos,v)){ res=MESH_ERR_OOM; goto done; }
    }
    else if(p[0]=='v'&&p[1]=='t'&&(p[2]==' '||p[2]=='\t')){
        const char* q=skip_ws(p+2);
        char* end;
        double u=strtod(q,&end);
        if(end==q){ res=MESH_ERR_FORMAT; goto done; }
        q=skip_ws(end);
        double v=strtod(q,&end);
        if(end==q)v=0.0;                  // "vt u" is legal: a 1-D texture coord
        if(!push_uv(&uvs,(Uv){(float)u,(float)v})){ res=MESH_ERR_OOM; goto done; }
    }
    else if(p[0]=='f'&&(p[1]==' '||p[1]=='\t')){
        refs.len=0;
        const char* q=p+1;
        for(;;){
            q=skip_ws(q);
            if(*q=='\0')break;
            long long vidx,tidx;
            bool have_t;
            const char* end=parse_face_ref(q,&vidx,&tidx,&have_t);
            // out-of-range digits saturate at LLONG_MIN/MAX, which resolve_index
            // then rejects, so no separate errno check is needed
            if(!end){ res=MESH_ERR_FORMAT; goto done; }
            uint64_t pi;
            if(!resolve_index(vidx,pos.len,&pi)){ res=MESH_ERR_FORMAT; goto done; }
            size_t ti=SIZE_MAX;
            if(have_t){
                uint64_t t;
                if(!resolve_index(tidx,uvs.len,&t)){ res=MESH_ERR_FORMAT; goto done; }
                ti=(size_t)t;
            }
            uint64_t vi;
            if(!map_get(&map,&vb,&pos,&uvs,(size_t)pi,ti,&vi)){ res=MESH_ERR_OOM; goto done; }
            if(!push_index(&refs,vi)){ res=MESH_ERR_OOM; goto done; }
            q=end;
        }
        if(refs.len<3)continue; // a point or a dangling edge, not a broken file
        // fan triangulation: render() reads connectors three at a time, so an
        // n-gon has to arrive already split
        for(size_t i=1;i+1<refs.len;i++){
            if(!push_index(&ib,refs.data[0])||!push_index(&ib,refs.data[i])
             ||!push_index(&ib,refs.data[i+1])){ res=MESH_ERR_OOM; goto done; }
            if(cur_mat<mats.len)mats.data[cur_mat].tris++;
        }
    }
    else if(strncmp(p,"mtllib",6)==0&&(p[6]==' '||p[6]=='\t')){
        // one statement may name several libraries, space separated
        const char* q=skip_ws(p+6);
        while(*q){
            const char* e=skip_token(q);
            char* name=dup_str(q,(size_t)(e-q));
            char* full=name?mesh_path_sibling(path,name):NULL;
            if(full)load_mtl(full,&mats);
            free(full);
            free(name);
            q=skip_ws(e);
        }
    }
    else if(strncmp(p,"usemtl",6)==0&&(p[6]==' '||p[6]=='\t')){
        char* name=rest_of_line(skip_ws(p+6));
        // a usemtl naming a material no mtllib defined still has to switch away
        // from the previous one, so an unknown name is added rather than ignored
        cur_mat=SIZE_MAX;
        if(name){
            Material* m=mat_add(&mats,name);
            if(m)cur_mat=(size_t)(m-mats.data);
        }
        free(name);
    }
    // anything else is vn/o/g/s or an unknown keyword: skipped
}

done:
fclose(f);
free(r.line);
free(refs.data);
free(map.nodes);
free(map.head);
free(pos.data);
free(uvs.data);
if(res==MESH_OK&&ib.len==0)res=MESH_ERR_EMPTY;
if(res!=MESH_OK){
    mat_free(&mats);
    free(vb.data);
    free(ib.data);
    *out=(Object){0};
    return res;
}
// give back the doubling slack; it would otherwise sit held for the lifetime
// of the object. a failed shrink is harmless, keep the larger block.
Vectex* vfit=realloc(vb.data,vb.len*sizeof *vb.data);
if(vfit)vb.data=vfit;
uint64_t* ifit=realloc(ib.data,ib.len*sizeof *ib.data);
if(ifit)ib.data=ifit;
out->vertices=vb.data;
out->len_of_vertices=(uint64_t)vb.len;
out->connectors_sequence=ib.data;
out->len_of_connectors=(uint64_t)ib.len;

// Object holds one texture, so the material covering the most triangles wins.
// With no usemtl anywhere the tallies are all zero and the first material in the
// mtl is used, which is what a single-material export means.
Material* best=NULL;
for(size_t i=0;i<mats.len;i++)
    if(!best||mats.data[i].tris>best->tris)best=&mats.data[i];

Image img={0};
if(best&&best->map_kd&&image_decode_file(best->map_kd,MESH_TEXTURE_MAX_DIM,&img,NULL,0)){
    out->texture=img.pixels;
    out->texture_width=img.width;
    out->texture_height=img.height;
}else if(!mesh_set_flat_texture(out,best?best->kd:MESH_DEFAULT_COLOUR)){
    mat_free(&mats);
    free(vb.data);
    free(ib.data);
    *out=(Object){0};
    return MESH_ERR_OOM;
}
mat_free(&mats);
return MESH_OK;
}

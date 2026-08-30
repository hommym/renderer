#include "mesh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// wavefront obj, text. only v and f carry anything this renderer can draw, so
// vn/vt/o/g/s/usemtl/mtllib are read past rather than stored -- there are no
// texture units, no normals and no materials downstream of Object.
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

MeshResult mesh_load_obj(const char* path, Object* out){
if(!out)return MESH_ERR_FORMAT;
*out=(Object){0};
if(!path)return MESH_ERR_OPEN;
// "rb": the tokeniser already treats '\r' as whitespace, so text mode would
// only add a platform-dependent second stripping rule
FILE* f=fopen(path,"rb");
if(!f)return MESH_ERR_OPEN;

Reader r={.f=f};
VertexBuf vb={0};
IndexBuf ib={0};
IndexBuf refs={0}; // one face's vertex refs, reused so an n-gon costs no churn
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
        if(!push_vertex(&vb,v)){ res=MESH_ERR_OOM; goto done; }
    }
    else if(p[0]=='f'&&(p[1]==' '||p[1]=='\t')){
        refs.len=0;
        const char* q=p+1;
        for(;;){
            q=skip_ws(q);
            if(*q=='\0')break;
            char* end;
            long long idx=strtoll(q,&end,10);
            // out-of-range digits saturate at LLONG_MIN/MAX, which resolve_index
            // then rejects, so no separate errno check is needed
            if(end==q){ res=MESH_ERR_FORMAT; goto done; }
            uint64_t vi;
            if(!resolve_index(idx,vb.len,&vi)){ res=MESH_ERR_FORMAT; goto done; }
            if(!push_index(&refs,vi)){ res=MESH_ERR_OOM; goto done; }
            q=skip_token(end); // drop the /vt/vn tail of this reference
        }
        if(refs.len<3)continue; // a point or a dangling edge, not a broken file
        // fan triangulation: render() reads connectors three at a time, so an
        // n-gon has to arrive already split
        for(size_t i=1;i+1<refs.len;i++){
            if(!push_index(&ib,refs.data[0])||!push_index(&ib,refs.data[i])
             ||!push_index(&ib,refs.data[i+1])){ res=MESH_ERR_OOM; goto done; }
        }
    }
    // anything else is vn/vt/o/g/s/usemtl/mtllib or an unknown keyword: skipped
}

done:
fclose(f);
free(r.line);
free(refs.data);
if(res==MESH_OK&&ib.len==0)res=MESH_ERR_EMPTY;
if(res!=MESH_OK){
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
return MESH_OK;
}

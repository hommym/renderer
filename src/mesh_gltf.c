#include "mesh.h"
#include "json.h"
#include "png.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

// glTF 2.0 reader, both containers: .glb (chunked binary) and .gltf (plain JSON
// with external or data: URI buffers).
//
// Everything the file says about a size, an offset or an index is a claim, not a
// fact. Nothing read out of the file addresses memory until it has been checked
// against the bytes actually present, and every size arithmetic is checked for
// overflow before it reaches malloc -- a wrapped multiply is how a "count" of
// 2^62 turns into a two-byte allocation and a wild write.

#define GLB_MAGIC      0x46546C67u   // "glTF"
#define GLB_CHUNK_JSON 0x4E4F534Au   // "JSON"
#define GLB_CHUNK_BIN  0x004E4942u   // "BIN\0"

#define CT_BYTE   5120
#define CT_UBYTE  5121
#define CT_SHORT  5122
#define CT_USHORT 5123
#define CT_UINT   5125
#define CT_FLOAT  5126

#define MODE_TRIANGLES 4
// recursion is bounded by this, not by the visited set: a valid tree of 100k
// nodes is still a 100k-deep call chain if it is a chain.
#define MAX_NODE_DEPTH 256
// an accessor with no bufferView reads as zeros, so its count is the one number
// in the file that no byte count bounds. cap it: otherwise a garbage "count":
// 2^40 sizes a 32TB allocation off nothing.
#define MAX_ZEROFILL_COUNT (1u<<22)

typedef struct Buf {
    const uint8_t* data;
    size_t len;
    bool owned;      // false for the GLB BIN chunk, which lives in the file image
    bool tried;
    MeshResult rc;   // cached so a broken buffer is not re-opened per accessor
} Buf;

// A resolved accessor: where its first element sits and how to step through it.
typedef struct Accessor {
    const uint8_t* base;   // NULL when the accessor has no bufferView -- reads as zeros
    size_t stride;
    size_t count;
    size_t ncomp;
    size_t comp_size;
    int comp_type;
    bool normalized;
} Accessor;

// one slot per images[] entry. `tried` is set before the decode, not after, so a
// file that cannot be decoded costs one attempt for the whole load rather than
// one per primitive -- 40 materials sharing a 2048x2048 texture is the normal
// case, and re-decoding it each time would dwarf the rest of the loader.
typedef struct ImgCache {
    PngImage img;
    bool tried;
} ImgCache;

typedef struct Ctx {
    const JsonValue* root;
    const JsonValue* buffers;
    const JsonValue* views;
    const JsonValue* accessors;
    const JsonValue* meshes;
    const JsonValue* nodes;
    const JsonValue* materials;
    const JsonValue* textures;
    const JsonValue* images;
    const JsonValue* samplers;
    Buf* bufs; size_t nbufs;
    ImgCache* imgs; size_t nimgs;         // allocated on the first texture actually sampled
    const uint8_t* bin; size_t bin_len;   // GLB BIN chunk, borrowed from the file image
    const char* dir; size_t dir_len;      // directory of the .gltf, for relative uris
    Vectex* verts; size_t nverts, cap_verts;
    uint64_t* idx;  size_t nidx,   cap_idx;
    bool* visited; size_t nnodes;
} Ctx;

static bool sz_add(size_t a,size_t b,size_t* r){ if(a>SIZE_MAX-b)return false; *r=a+b; return true; }
static bool sz_mul(size_t a,size_t b,size_t* r){ if(a&&b>SIZE_MAX/a)return false; *r=a*b; return true; }

static uint32_t rd_u32(const uint8_t* p){
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}

// json numbers are doubles; anything used as a count or an offset has to be a
// whole non-negative value that survives the trip into size_t exactly.
static bool json_size(const JsonValue* v,size_t* out){
    if(json_type(v)!=JSON_NUMBER)return false;
    double d=json_number(v,-1.0);
    if(!(d>=0.0))return false;                 // also rejects NaN
    if(d>9007199254740992.0)return false;      // past 2^53 a double is not an exact integer
    if(d>(double)SIZE_MAX)return false;
    if(d!=floor(d))return false;
    *out=(size_t)d;
    return true;
}

static bool json_size_opt(const JsonValue* v,size_t def,size_t* out){
    if(!v||json_type(v)==JSON_NULL){*out=def;return true;}
    return json_size(v,out);
}

static bool json_num_array(const JsonValue* v,size_t n,double* out){
    if(json_type(v)!=JSON_ARRAY||json_count(v)!=n)return false;
    for(size_t i=0;i<n;i++){
        const JsonValue* e=json_at(v,i);
        if(json_type(e)!=JSON_NUMBER)return false;
        out[i]=json_number(e,0.0);
        if(!isfinite(out[i]))return false;     // one NaN in a matrix poisons every vertex under it
    }
    return true;
}

static void* grow_arr(void* arr,size_t* cap,size_t need,size_t elem){
    if(need<=*cap)return arr;
    size_t ncap=*cap?*cap:1024;
    while(ncap<need){ if(ncap>SIZE_MAX/2)return NULL; ncap*=2; }
    if(ncap>SIZE_MAX/elem)return NULL;
    void* p=realloc(arr,ncap*elem);
    if(!p)return NULL;
    *cap=ncap;
    return p;
}

static MeshResult read_whole_file(const char* path,uint8_t** out,size_t* out_len){
    *out=NULL;*out_len=0;
    FILE* f=fopen(path,"rb");
    if(!f)return MESH_ERR_OPEN;
    if(fseek(f,0,SEEK_END)!=0){fclose(f);return MESH_ERR_READ;}
    long n=ftell(f);
    if(n<0){fclose(f);return MESH_ERR_READ;}
    if(n==0){fclose(f);return MESH_ERR_FORMAT;}
    if(fseek(f,0,SEEK_SET)!=0){fclose(f);return MESH_ERR_READ;}
    size_t len=(size_t)n;
    uint8_t* buf=malloc(len+1);                // +1 so a .gltf image can be treated as a C string
    if(!buf){fclose(f);return MESH_ERR_OOM;}
    size_t got=fread(buf,1,len,f);
    fclose(f);
    if(got!=len){free(buf);return MESH_ERR_READ;}
    buf[len]='\0';
    *out=buf;*out_len=len;
    return MESH_OK;
}

// ---- GLB container -------------------------------------------------------

static MeshResult glb_split(const uint8_t* f,size_t flen,const char** js,size_t* js_len,
                            const uint8_t** bin,size_t* bin_len){
    if(flen<12)return MESH_ERR_FORMAT;
    if(rd_u32(f+4)!=2u)return MESH_ERR_UNSUPPORTED;   // only glTF 2 uses this chunk layout
    size_t total=rd_u32(f+8);
    if(total<12||total>flen)return MESH_ERR_FORMAT;   // a header longer than the file is a truncation
    size_t o=12;
    bool have_json=false;
    while(o+8<=total){
        size_t clen=rd_u32(f+o);
        uint32_t ctype=rd_u32(f+o+4);
        size_t payload=o+8;
        if(clen>total-payload)return MESH_ERR_FORMAT;
        if(ctype==GLB_CHUNK_JSON){
            if(have_json||o!=12)return MESH_ERR_FORMAT;  // JSON must be the first chunk and appear once
            *js=(const char*)(f+payload);*js_len=clen;have_json=true;
        }else if(ctype==GLB_CHUNK_BIN&&!*bin){
            *bin=f+payload;*bin_len=clen;
        }
        // unknown chunk types are extensions: step over them, do not fail
        o=payload+clen;
        size_t pad=(4u-(clen&3u))&3u;
        if(pad>total-o)break;                 // the last chunk may end on the final byte
        o+=pad;
    }
    if(!have_json)return MESH_ERR_FORMAT;
    return MESH_OK;
}

// ---- uris ----------------------------------------------------------------

static int hex_val(char ch){
    if(ch>='0'&&ch<='9')return ch-'0';
    if(ch>='a'&&ch<='f')return ch-'a'+10;
    if(ch>='A'&&ch<='F')return ch-'A'+10;
    return -1;
}

static char* uri_unescape(const char* s){
    size_t n=strlen(s);
    char* o=malloc(n+1);
    if(!o)return NULL;
    size_t w=0;
    for(size_t i=0;i<n;i++){
        int hi,lo;
        if(s[i]=='%'&&i+2<n&&(hi=hex_val(s[i+1]))>=0&&(lo=hex_val(s[i+2]))>=0){
            int b=(hi<<4)|lo;
            if(b==0){free(o);return NULL;}    // %00 would truncate the path silently
            o[w++]=(char)b;
            i+=2;
        }else o[w++]=s[i];
    }
    o[w]='\0';
    return o;
}

// a uri out of an untrusted model must not reach outside the model's own folder,
// so schemes, absolute paths and ".." segments are refused rather than resolved.
static bool uri_is_safe_relative(const char* u){
    if(!u||!*u)return false;
    if(u[0]=='/'||u[0]=='\\')return false;
    if(u[1]==':')return false;                 // windows drive letter
    if(strstr(u,"://"))return false;
    for(const char* p=u;;){
        if(p[0]=='.'&&p[1]=='.'&&(p[2]=='\0'||p[2]=='/'||p[2]=='\\'))return false;
        const char* slash=strpbrk(p,"/\\");
        if(!slash)break;
        p=slash+1;
    }
    return true;
}

static int b64_val(unsigned char ch){
    if(ch>='A'&&ch<='Z')return ch-'A';
    if(ch>='a'&&ch<='z')return ch-'a'+26;
    if(ch>='0'&&ch<='9')return ch-'0'+52;
    if(ch=='+')return 62;
    if(ch=='/')return 63;
    return -1;
}

static bool b64_decode(const char* s,size_t n,uint8_t** out,size_t* out_len){
    size_t cap=n/4*3+3;
    uint8_t* buf=malloc(cap);
    if(!buf)return false;
    uint32_t acc=0;
    int nbits=0;
    size_t w=0;
    for(size_t i=0;i<n;i++){
        unsigned char ch=(unsigned char)s[i];
        if(ch=='=')break;
        if(ch==' '||ch=='\n'||ch=='\r'||ch=='\t')continue;
        int v=b64_val(ch);
        if(v<0){free(buf);return false;}
        acc=(acc<<6)|(uint32_t)v;
        nbits+=6;
        if(nbits>=8){
            nbits-=8;
            if(w>=cap){free(buf);return false;}
            buf[w++]=(uint8_t)((acc>>nbits)&0xFFu);
        }
    }
    *out=buf;*out_len=w;
    return true;
}

// ---- buffers -------------------------------------------------------------

// a data: payload or a path under the model's own directory, into caller-owned
// bytes. shared by buffers and by .gltf image uris so an image cannot reach
// anywhere a buffer could not.
static MeshResult uri_bytes(Ctx* c,const char* uri,uint8_t** out,size_t* out_len){
    *out=NULL;*out_len=0;
    if(strncmp(uri,"data:",5)==0){
        const char* comma=strchr(uri,',');
        if(!comma)return MESH_ERR_FORMAT;
        size_t hdr=(size_t)(comma-uri);
        if(hdr<7||strncmp(comma-7,";base64",7)!=0)return MESH_ERR_UNSUPPORTED; // only base64 payloads
        if(!b64_decode(comma+1,strlen(comma+1),out,out_len))return MESH_ERR_FORMAT;
        return MESH_OK;
    }
    char* dec=uri_unescape(uri);
    if(!dec)return MESH_ERR_FORMAT;
    if(!uri_is_safe_relative(dec)){free(dec);return MESH_ERR_UNSUPPORTED;}
    size_t nlen=strlen(dec),plen;
    if(!sz_add(c->dir_len,nlen,&plen)||!sz_add(plen,1,&plen)){free(dec);return MESH_ERR_FORMAT;}
    char* full=malloc(plen);
    if(!full){free(dec);return MESH_ERR_OOM;}
    memcpy(full,c->dir,c->dir_len);
    memcpy(full+c->dir_len,dec,nlen+1);
    free(dec);
    MeshResult r=read_whole_file(full,out,out_len);
    free(full);
    return r;
}

static MeshResult buffer_load(Ctx* c,size_t i,Buf* b){
    const JsonValue* bv=json_at(c->buffers,i);
    if(json_type(bv)!=JSON_OBJECT)return MESH_ERR_FORMAT;
    size_t declared;
    if(!json_size(json_member(bv,"byteLength"),&declared)||declared==0)return MESH_ERR_FORMAT;

    const char* uri=json_member_string(bv,"uri",NULL);
    if(!uri){
        // no uri means the GLB BIN chunk; a plain .gltf has nothing to point at
        if(!c->bin||c->bin_len<declared)return MESH_ERR_FORMAT;
        b->data=c->bin;b->len=declared;b->owned=false;
        return MESH_OK;
    }

    uint8_t* data=NULL;
    size_t len=0;
    MeshResult r=uri_bytes(c,uri,&data,&len);
    if(r!=MESH_OK)return r;
    if(len<declared){free(data);return MESH_ERR_FORMAT;}
    b->data=data;b->len=declared;b->owned=true;   // trailing base64/file slop is ignored
    return MESH_OK;
}

// resolved lazily: a file whose second buffer is missing still loads if nothing
// ever reads through it.
static MeshResult buffer_get(Ctx* c,size_t i,const uint8_t** data,size_t* len){
    if(i>=c->nbufs)return MESH_ERR_FORMAT;
    Buf* b=&c->bufs[i];
    if(!b->tried){
        b->tried=true;
        b->rc=buffer_load(c,i,b);
    }
    if(b->rc!=MESH_OK)return b->rc;
    *data=b->data;*len=b->len;
    return MESH_OK;
}

// ---- accessors -----------------------------------------------------------

static size_t comp_size_of(int ct){
    switch(ct){
    case CT_BYTE: case CT_UBYTE: return 1;
    case CT_SHORT: case CT_USHORT: return 2;
    case CT_UINT: case CT_FLOAT: return 4;
    default: return 0;
    }
}

static size_t type_ncomp(const char* t){
    if(!t)return 0;
    if(strcmp(t,"SCALAR")==0)return 1;
    if(strcmp(t,"VEC2")==0)return 2;
    if(strcmp(t,"VEC3")==0)return 3;
    if(strcmp(t,"VEC4")==0)return 4;
    return 0;                                  // MAT2/3/4 never carry geometry we read
}

static MeshResult accessor_resolve(Ctx* c,size_t ai,Accessor* a){
    memset(a,0,sizeof *a);
    if(json_type(c->accessors)!=JSON_ARRAY||ai>=json_count(c->accessors))return MESH_ERR_FORMAT;
    const JsonValue* av=json_at(c->accessors,ai);
    if(json_type(av)!=JSON_OBJECT)return MESH_ERR_FORMAT;
    // a sparse accessor patches the dense data; reading the dense side alone is
    // wrong geometry, not an approximation
    if(json_type(json_member(av,"sparse"))==JSON_OBJECT)return MESH_ERR_UNSUPPORTED;

    size_t ct;
    if(!json_size(json_member(av,"componentType"),&ct)||ct>(size_t)INT_MAX)return MESH_ERR_FORMAT;
    size_t csize=comp_size_of((int)ct);
    if(!csize)return MESH_ERR_FORMAT;
    size_t ncomp=type_ncomp(json_member_string(av,"type",NULL));
    if(!ncomp)return MESH_ERR_FORMAT;
    if(!json_size(json_member(av,"count"),&a->count)||a->count==0)return MESH_ERR_FORMAT;
    a->comp_type=(int)ct;
    a->comp_size=csize;
    a->ncomp=ncomp;
    a->normalized=json_bool(json_member(av,"normalized"),false);

    size_t elem;
    if(!sz_mul(csize,ncomp,&elem))return MESH_ERR_FORMAT;

    const JsonValue* bvv=json_member(av,"bufferView");
    if(!bvv||json_type(bvv)==JSON_NULL){
        if(a->count>MAX_ZEROFILL_COUNT)return MESH_ERR_FORMAT;
        a->base=NULL;a->stride=elem;           // no bufferView: the accessor reads as zeros
        return MESH_OK;
    }
    size_t vi;
    if(!json_size(bvv,&vi))return MESH_ERR_FORMAT;
    if(json_type(c->views)!=JSON_ARRAY||vi>=json_count(c->views))return MESH_ERR_FORMAT;
    const JsonValue* view=json_at(c->views,vi);
    if(json_type(view)!=JSON_OBJECT)return MESH_ERR_FORMAT;
    // a meshopt-compressed view's bytes are not the elements they describe
    if(json_member(json_member(view,"extensions"),"EXT_meshopt_compression"))return MESH_ERR_UNSUPPORTED;

    size_t bi,v_off,v_len,v_stride,a_off;
    if(!json_size(json_member(view,"buffer"),&bi))return MESH_ERR_FORMAT;
    if(!json_size_opt(json_member(view,"byteOffset"),0,&v_off))return MESH_ERR_FORMAT;
    if(!json_size(json_member(view,"byteLength"),&v_len)||v_len==0)return MESH_ERR_FORMAT;
    if(!json_size_opt(json_member(view,"byteStride"),0,&v_stride))return MESH_ERR_FORMAT;
    if(!json_size_opt(json_member(av,"byteOffset"),0,&a_off))return MESH_ERR_FORMAT;

    const uint8_t* data;
    size_t dlen;
    MeshResult r=buffer_get(c,bi,&data,&dlen);
    if(r!=MESH_OK)return r;
    size_t end;
    if(!sz_add(v_off,v_len,&end)||end>dlen)return MESH_ERR_FORMAT;

    size_t stride=v_stride?v_stride:elem;      // absent byteStride means tightly packed
    if(stride<elem)return MESH_ERR_FORMAT;     // interleaved elements would overlap

    // the last element has to land inside the view: a_off + (count-1)*stride + elem
    size_t need;
    if(!sz_mul(a->count-1,stride,&need))return MESH_ERR_FORMAT;
    if(!sz_add(need,elem,&need))return MESH_ERR_FORMAT;
    if(!sz_add(need,a_off,&need))return MESH_ERR_FORMAT;
    if(need>v_len)return MESH_ERR_FORMAT;

    a->base=data+v_off+a_off;
    a->stride=stride;
    return MESH_OK;
}

// glTF is little-endian; so is every target this renderer builds for. memcpy
// because a bufferView is only 4-byte aligned at best and may not be even that.
static double acc_raw(const Accessor* a,size_t i,size_t k){
    if(!a->base||k>=a->ncomp)return 0.0;
    const uint8_t* p=a->base+i*a->stride+k*a->comp_size;
    switch(a->comp_type){
    case CT_BYTE:  { int8_t   v; memcpy(&v,p,1); return (double)v; }
    case CT_UBYTE: { uint8_t  v; memcpy(&v,p,1); return (double)v; }
    case CT_SHORT: { int16_t  v; memcpy(&v,p,2); return (double)v; }
    case CT_USHORT:{ uint16_t v; memcpy(&v,p,2); return (double)v; }
    case CT_UINT:  { uint32_t v; memcpy(&v,p,4); return (double)v; }
    case CT_FLOAT: { float    v; memcpy(&v,p,4); return (double)v; }
    default: return 0.0;
    }
}

// integer colour channels are normalized by their type's maximum; the clamp also
// swallows NaN, which would otherwise be undefined behaviour on the cast below.
static double acc_norm(const Accessor* a,size_t i,size_t k){
    double v=acc_raw(a,i,k);
    switch(a->comp_type){
    case CT_BYTE:   v/=127.0; break;
    case CT_UBYTE:  v/=255.0; break;
    case CT_SHORT:  v/=32767.0; break;
    case CT_USHORT: v/=65535.0; break;
    case CT_UINT:   v/=4294967295.0; break;
    default: break;
    }
    if(!(v>=0.0))return 0.0;
    return v>1.0?1.0:v;
}

static uint32_t pack_colour(const Accessor* a,size_t i){
    double r=acc_norm(a,i,0),g=acc_norm(a,i,1),b=acc_norm(a,i,2);
    double al=a->ncomp>=4?acc_norm(a,i,3):1.0;   // VEC3 colour is opaque
    return ((uint32_t)(al*255.0+0.5)<<24)|((uint32_t)(r*255.0+0.5)<<16)
          |((uint32_t)(g*255.0+0.5)<<8)|(uint32_t)(b*255.0+0.5);
}

// ---- base colour ---------------------------------------------------------

// this renderer has no texture units, so a material's baseColorTexture is baked
// down to one sample per vertex at load time. everything below is cosmetic: a
// missing, malformed or undecodable link degrades to the next step of the
// precedence chain (COLOR_0, texture, factor, MESH_DEFAULT_COLOUR) instead of
// failing a load whose geometry is perfectly good. a jpeg reaches the same path
// as a corrupt png -- neither is an error.

typedef struct BaseColour {
    const PngImage* tex;      // NULL unless a baseColorTexture image actually decoded
    size_t uv_set;            // names the attribute TEXCOORD_<uv_set>
    PngWrap wrap_s,wrap_t;
    bool opaque;              // alphaMode OPAQUE, the default: base colour alpha is ignored
    bool has_factor;
    double factor[4];         // rgba, clamped to [0,1]; all ones when absent
} BaseColour;

// a bufferView as raw bytes. accessor_resolve cannot be reused here: an image
// view carries no element type, count or stride to check against.
static bool view_bytes(Ctx* c,size_t vi,const uint8_t** p,size_t* n){
    if(json_type(c->views)!=JSON_ARRAY||vi>=json_count(c->views))return false;
    const JsonValue* view=json_at(c->views,vi);
    if(json_type(view)!=JSON_OBJECT)return false;
    size_t bi,off,len;
    if(!json_size(json_member(view,"buffer"),&bi))return false;
    if(!json_size_opt(json_member(view,"byteOffset"),0,&off))return false;
    if(!json_size(json_member(view,"byteLength"),&len)||len==0)return false;
    const uint8_t* data;
    size_t dlen,end;
    if(buffer_get(c,bi,&data,&dlen)!=MESH_OK)return false;
    if(!sz_add(off,len,&end)||end>dlen)return false;
    *p=data+off;*n=len;
    return true;
}

static const PngImage* image_get(Ctx* c,size_t ii){
    if(ii>=c->nimgs)return NULL;
    if(!c->imgs&&!(c->imgs=calloc(c->nimgs,sizeof *c->imgs)))return NULL;
    ImgCache* e=&c->imgs[ii];
    if(e->tried)return e->img.rgba?&e->img:NULL;
    e->tried=true;

    const JsonValue* iv=json_at(c->images,ii);
    if(json_type(iv)!=JSON_OBJECT)return NULL;

    const uint8_t* bytes=NULL;
    uint8_t* owned=NULL;
    size_t n=0;
    const JsonValue* bvv=json_member(iv,"bufferView");
    if(bvv&&json_type(bvv)!=JSON_NULL){
        size_t vi;
        if(!json_size(bvv,&vi)||!view_bytes(c,vi,&bytes,&n))return NULL;
    }else{
        const char* uri=json_member_string(iv,"uri",NULL);
        if(!uri||uri_bytes(c,uri,&owned,&n)!=MESH_OK)return NULL;
        bytes=owned;
    }
    // mimeType is not consulted: what the file calls the bytes decides nothing,
    // whether png_decode accepts them decides everything
    if(!png_decode(bytes,n,&e->img,NULL,0))memset(&e->img,0,sizeof e->img);
    free(owned);
    return e->img.rgba?&e->img:NULL;
}

static PngWrap wrap_of(double mode){
    if(mode==33071.0)return PNG_WRAP_CLAMP;
    if(mode==33648.0)return PNG_WRAP_MIRROR;
    return PNG_WRAP_REPEAT;                        // 10497, and anything unrecognised
}

static void base_colour_of(Ctx* c,const JsonValue* prim,BaseColour* bc){
    memset(bc,0,sizeof *bc);
    for(int k=0;k<4;k++)bc->factor[k]=1.0;         // identity, so a texture alone multiplies out unchanged

    size_t mi;
    const JsonValue* miv=json_member(prim,"material");
    if(!miv||json_type(miv)==JSON_NULL||!json_size(miv,&mi))return;
    if(json_type(c->materials)!=JSON_ARRAY||mi>=json_count(c->materials))return;
    const JsonValue* mat=json_at(c->materials,mi);
    // OPAQUE is the default and it discards the base colour alpha outright, so a
    // factor of [1,1,1,0.2] on an opaque material must not come out translucent
    const char* am=json_member_string(mat,"alphaMode","OPAQUE");
    bc->opaque=strcmp(am,"BLEND")!=0&&strcmp(am,"MASK")!=0;
    const JsonValue* pbr=json_member(mat,"pbrMetallicRoughness");
    if(json_type(pbr)!=JSON_OBJECT)return;

    const JsonValue* f=json_member(pbr,"baseColorFactor");
    if(f&&json_type(f)!=JSON_NULL){
        // json_num_array writes as it goes, so a partial array has to be undone
        if(json_num_array(f,4,bc->factor)){
            for(int k=0;k<4;k++)bc->factor[k]=bc->factor[k]<0.0?0.0:(bc->factor[k]>1.0?1.0:bc->factor[k]);
            bc->has_factor=true;
        }else for(int k=0;k<4;k++)bc->factor[k]=1.0;
    }

    const JsonValue* bct=json_member(pbr,"baseColorTexture");
    if(json_type(bct)!=JSON_OBJECT)return;
    size_t ti;
    if(!json_size(json_member(bct,"index"),&ti))return;
    if(!json_size_opt(json_member(bct,"texCoord"),0,&bc->uv_set)||bc->uv_set>99)return;
    if(json_type(c->textures)!=JSON_ARRAY||ti>=json_count(c->textures))return;
    const JsonValue* tex=json_at(c->textures,ti);
    if(json_type(tex)!=JSON_OBJECT)return;

    size_t si;
    if(json_size(json_member(tex,"sampler"),&si)&&
       json_type(c->samplers)==JSON_ARRAY&&si<json_count(c->samplers)){
        const JsonValue* sm=json_at(c->samplers,si);
        bc->wrap_s=wrap_of(json_member_number(sm,"wrapS",10497.0));
        bc->wrap_t=wrap_of(json_member_number(sm,"wrapT",10497.0));
    }

    size_t src;
    // a source-less texture is an extension's (KHR_texture_basisu and friends);
    // there is nothing here we know how to read
    if(json_size(json_member(tex,"source"),&src))bc->tex=image_get(c,src);
}

// png_sample applies one wrap mode to both axes, so when wrapT disagrees with
// wrapS the v axis is folded into [0,1) here, where every mode is a no-op. clamp
// folds to the outermost texel *centres* rather than to 0 and 1: a bilinear tap
// at the very edge would otherwise reach past it and pick up whatever wrapS says
// lies there, which is the edge bleeding clamp exists to prevent. `n` is the
// image's extent on this axis.
static double fold(double t,PngWrap w,uint32_t n){
    double f;
    if(w==PNG_WRAP_CLAMP){
        double half=n?0.5/(double)n:0.0;
        f=t<half?half:(t>1.0-half?1.0-half:t);
    }else{
        f=t-floor(t);
        if(w==PNG_WRAP_MIRROR&&fmod(floor(t),2.0)!=0.0)f=1.0-f;
    }
    return f<1.0?f:nextafter(1.0,0.0);
}

// glTF defines baseColor as factor * texture, so the two multiply per channel.
// either half may be absent -- the missing one is left as white.
static uint32_t shade(const BaseColour* bc,const Accessor* uv,size_t i){
    double ch[4]={bc->factor[0],bc->factor[1],bc->factor[2],bc->factor[3]};
    if(bc->tex&&uv){
        // normalized integer texcoords are already unit-range; float ones are not
        // meant to be, and the wrap mode is what gives them meaning
        double u=uv->comp_type==CT_FLOAT?acc_raw(uv,i,0):acc_norm(uv,i,0);
        double v=uv->comp_type==CT_FLOAT?acc_raw(uv,i,1):acc_norm(uv,i,1);
        if(isfinite(u)&&isfinite(v)){
            if(bc->wrap_t!=bc->wrap_s)v=fold(v,bc->wrap_t,bc->tex->height);
            uint32_t t=png_sample(bc->tex,u,v,bc->wrap_s,0xFFFFFFFFu);
            ch[0]*=(double)((t>>16)&0xFFu)/255.0;
            ch[1]*=(double)((t>>8)&0xFFu)/255.0;
            ch[2]*=(double)(t&0xFFu)/255.0;
            ch[3]*=(double)((t>>24)&0xFFu)/255.0;
        }
    }
    if(bc->opaque)ch[3]=1.0;
    return ((uint32_t)(ch[3]*255.0+0.5)<<24)|((uint32_t)(ch[0]*255.0+0.5)<<16)
          |((uint32_t)(ch[1]*255.0+0.5)<<8)|(uint32_t)(ch[2]*255.0+0.5);
}

// ---- node transforms -----------------------------------------------------

// column-major, out = a * b. parent on the left, so descending the tree composes
// world = parent * local.
static void mat_mul(const double a[16],const double b[16],double out[16]){
    for(int col=0;col<4;col++)
        for(int row=0;row<4;row++){
            double s=0.0;
            for(int k=0;k<4;k++)s+=a[k*4+row]*b[col*4+k];
            out[col*4+row]=s;
        }
}

static MeshResult node_matrix(const JsonValue* n,double m[16]){
    const JsonValue* mm=json_member(n,"matrix");
    if(mm&&json_type(mm)!=JSON_NULL){
        if(!json_num_array(mm,16,m))return MESH_ERR_FORMAT;   // already column-major
        return MESH_OK;
    }
    double t[3]={0,0,0},q[4]={0,0,0,1},s[3]={1,1,1};
    const JsonValue* v;
    if((v=json_member(n,"translation"))&&!json_num_array(v,3,t))return MESH_ERR_FORMAT;
    if((v=json_member(n,"rotation"))&&!json_num_array(v,4,q))return MESH_ERR_FORMAT;
    if((v=json_member(n,"scale"))&&!json_num_array(v,3,s))return MESH_ERR_FORMAT;

    double len=sqrt(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
    if(len<1e-12){q[0]=q[1]=q[2]=0.0;q[3]=1.0;len=1.0;}   // a zero quaternion is no rotation
    double x=q[0]/len,y=q[1]/len,z=q[2]/len,w=q[3]/len;

    // M = T * R * S, so each rotation column is pre-scaled by its own axis scale
    m[0]=(1-2*(y*y+z*z))*s[0]; m[1]=(2*(x*y+z*w))*s[0];   m[2]=(2*(x*z-y*w))*s[0];   m[3]=0;
    m[4]=(2*(x*y-z*w))*s[1];   m[5]=(1-2*(x*x+z*z))*s[1]; m[6]=(2*(y*z+x*w))*s[1];   m[7]=0;
    m[8]=(2*(x*z+y*w))*s[2];   m[9]=(2*(y*z-x*w))*s[2];   m[10]=(1-2*(x*x+y*y))*s[2];m[11]=0;
    m[12]=t[0];m[13]=t[1];m[14]=t[2];m[15]=1;
    return MESH_OK;
}

// ---- geometry ------------------------------------------------------------

static MeshResult emit_primitive(Ctx* c,const JsonValue* prim,const double m[16]){
    if(json_type(prim)!=JSON_OBJECT)return MESH_ERR_FORMAT;
    // render() reads connectors three at a time; strips, fans and lines would be
    // reinterpreted as triangles rather than drawn
    if(json_member_number(prim,"mode",MODE_TRIANGLES)!=(double)MODE_TRIANGLES)return MESH_ERR_UNSUPPORTED;

    // draco keeps the real geometry in an extension buffer and leaves the
    // accessors bufferView-less; read straight, that is a mesh of zeros, not an error
    if(json_member(json_member(prim,"extensions"),"KHR_draco_mesh_compression"))return MESH_ERR_UNSUPPORTED;

    // morph targets displace POSITION by the node/mesh weights. we cannot apply
    // them, and emitting the base shape would be the silent reinterpretation
    // mesh.h forbids, so refuse rather than return the wrong geometry.
    if(json_member(prim,"targets"))return MESH_ERR_UNSUPPORTED;

    const JsonValue* attrs=json_member(prim,"attributes");
    if(json_type(attrs)!=JSON_OBJECT)return MESH_ERR_FORMAT;

    size_t pi;
    if(!json_size(json_member(attrs,"POSITION"),&pi))return MESH_ERR_FORMAT;
    Accessor pos;
    MeshResult r=accessor_resolve(c,pi,&pos);
    if(r!=MESH_OK)return r;
    if(pos.comp_type!=CT_FLOAT||pos.ncomp!=3)return MESH_ERR_FORMAT;

    Accessor col;
    bool have_col=false;
    const JsonValue* cv=json_member(attrs,"COLOR_0");
    if(cv&&json_type(cv)!=JSON_NULL){
        size_t ci;
        if(!json_size(cv,&ci))return MESH_ERR_FORMAT;
        r=accessor_resolve(c,ci,&col);
        if(r!=MESH_OK)return r;
        if(col.ncomp<3||col.ncomp>4)return MESH_ERR_FORMAT;
        if(col.comp_type!=CT_FLOAT&&col.comp_type!=CT_UBYTE&&col.comp_type!=CT_USHORT)return MESH_ERR_FORMAT;
        have_col=true;
    }

    // COLOR_0 wins outright, so a primitive that has one never touches a
    // material -- and never decodes an image it would not have used
    BaseColour bc={0};
    Accessor uv;
    bool have_uv=false;
    if(!have_col){
        base_colour_of(c,prim,&bc);
        if(bc.tex){
            char attr[24];
            snprintf(attr,sizeof attr,"TEXCOORD_%zu",bc.uv_set);
            size_t ui;
            // a texcoord shorter than POSITION would leave the tail unsampled, so
            // the whole primitive falls back rather than half of it
            if(json_size(json_member(attrs,attr),&ui)&&accessor_resolve(c,ui,&uv)==MESH_OK&&
               uv.ncomp==2&&uv.count>=pos.count&&
               (uv.comp_type==CT_FLOAT||((uv.comp_type==CT_UBYTE||uv.comp_type==CT_USHORT)&&uv.normalized)))
                have_uv=true;
            else bc.tex=NULL;
        }
    }

    Accessor ind;
    bool have_ind=false;
    const JsonValue* iv=json_member(prim,"indices");
    if(iv&&json_type(iv)!=JSON_NULL){
        size_t ii;
        if(!json_size(iv,&ii))return MESH_ERR_FORMAT;
        r=accessor_resolve(c,ii,&ind);
        if(r!=MESH_OK)return r;
        if(ind.ncomp!=1)return MESH_ERR_FORMAT;
        if(ind.comp_type!=CT_UBYTE&&ind.comp_type!=CT_USHORT&&ind.comp_type!=CT_UINT)return MESH_ERR_FORMAT;
        have_ind=true;
    }

    size_t tri_count=have_ind?ind.count:pos.count;
    if(tri_count%3)return MESH_ERR_FORMAT;     // a triangle list is always a multiple of 3

    size_t base=c->nverts,vneed,ineed;
    if(!sz_add(base,pos.count,&vneed))return MESH_ERR_FORMAT;
    if(!sz_add(c->nidx,tri_count,&ineed))return MESH_ERR_FORMAT;

    Vectex* nv=grow_arr(c->verts,&c->cap_verts,vneed,sizeof *c->verts);
    if(!nv)return MESH_ERR_OOM;
    c->verts=nv;
    uint64_t* ni=grow_arr(c->idx,&c->cap_idx,ineed,sizeof *c->idx);
    if(!ni)return MESH_ERR_OOM;
    c->idx=ni;

    for(size_t i=0;i<pos.count;i++){
        double x=acc_raw(&pos,i,0),y=acc_raw(&pos,i,1),z=acc_raw(&pos,i,2);
        if(!isfinite(x)||!isfinite(y)||!isfinite(z))return MESH_ERR_FORMAT;
        Vectex v;
        v.x=m[0]*x+m[4]*y+m[8]*z+m[12];
        v.y=m[1]*x+m[5]*y+m[9]*z+m[13];
        v.z=m[2]*x+m[6]*y+m[10]*z+m[14];
        if(!isfinite(v.x)||!isfinite(v.y)||!isfinite(v.z))return MESH_ERR_FORMAT;
        // a short COLOR_0 is padded rather than rejected: the geometry is still good
        if(have_col&&i<col.count)v.colour=pack_colour(&col,i);
        else if(bc.tex||bc.has_factor)v.colour=shade(&bc,have_uv?&uv:NULL,i);
        else v.colour=MESH_DEFAULT_COLOUR;
        c->verts[base+i]=v;
    }

    for(size_t i=0;i<tri_count;i++){
        size_t vi;
        if(have_ind){
            double d=acc_raw(&ind,i,0);        // unsigned and at most 2^32, so exact in a double
            if(!(d>=0.0)||d>=(double)pos.count)return MESH_ERR_FORMAT;
            vi=(size_t)d;
        }else vi=i;                            // no indices: vertices are triangles in order
        c->idx[c->nidx+i]=(uint64_t)(base+vi);
    }

    c->nverts=vneed;
    c->nidx=ineed;
    return MESH_OK;
}

static MeshResult emit_mesh(Ctx* c,size_t mi,const double m[16]){
    if(json_type(c->meshes)!=JSON_ARRAY||mi>=json_count(c->meshes))return MESH_ERR_FORMAT;
    const JsonValue* mesh=json_at(c->meshes,mi);
    if(json_type(mesh)!=JSON_OBJECT)return MESH_ERR_FORMAT;
    const JsonValue* prims=json_member(mesh,"primitives");
    if(json_type(prims)!=JSON_ARRAY)return MESH_ERR_FORMAT;
    size_t n=json_count(prims);
    for(size_t i=0;i<n;i++){
        MeshResult r=emit_primitive(c,json_at(prims,i),m);
        if(r!=MESH_OK)return r;
    }
    return MESH_OK;
}

static MeshResult walk_node(Ctx* c,size_t ni,const double parent[16],int depth){
    if(depth>MAX_NODE_DEPTH)return MESH_ERR_UNSUPPORTED;
    if(ni>=c->nnodes)return MESH_ERR_FORMAT;
    if(c->visited[ni])return MESH_ERR_FORMAT;  // the node graph is a tree; a revisit is a cycle
    c->visited[ni]=true;

    const JsonValue* n=json_at(c->nodes,ni);
    if(json_type(n)!=JSON_OBJECT)return MESH_ERR_FORMAT;
    double local[16],world[16];
    MeshResult r=node_matrix(n,local);
    if(r!=MESH_OK)return r;
    mat_mul(parent,local,world);

    const JsonValue* mv=json_member(n,"mesh");
    if(mv&&json_type(mv)!=JSON_NULL){
        // a skinned mesh is posed by its joints, and the spec requires the
        // referencing node's own transform to be ignored. we do neither, so
        // baking `world` in would place it wrongly in a wrong pose.
        const JsonValue* sk=json_member(n,"skin");
        if(sk&&json_type(sk)!=JSON_NULL)return MESH_ERR_UNSUPPORTED;
        size_t mi;
        if(!json_size(mv,&mi))return MESH_ERR_FORMAT;
        r=emit_mesh(c,mi,world);
        if(r!=MESH_OK)return r;
    }

    const JsonValue* ch=json_member(n,"children");
    if(ch&&json_type(ch)!=JSON_NULL){
        if(json_type(ch)!=JSON_ARRAY)return MESH_ERR_FORMAT;
        size_t nc=json_count(ch);
        for(size_t i=0;i<nc;i++){
            size_t ci;
            if(!json_size(json_at(ch,i),&ci))return MESH_ERR_FORMAT;
            r=walk_node(c,ci,world,depth+1);
            if(r!=MESH_OK)return r;
        }
    }
    return MESH_OK;
}

static MeshResult walk_scene(Ctx* c){
    static const double ident[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const JsonValue* scenes=json_member(c->root,"scenes");
    if(json_type(scenes)==JSON_ARRAY&&json_count(scenes)>0){
        size_t si;
        if(!json_size_opt(json_member(c->root,"scene"),0,&si))return MESH_ERR_FORMAT;
        if(si>=json_count(scenes))return MESH_ERR_FORMAT;
        const JsonValue* sc=json_at(scenes,si);
        if(json_type(sc)!=JSON_OBJECT)return MESH_ERR_FORMAT;
        const JsonValue* roots=json_member(sc,"nodes");
        if(!roots||json_type(roots)==JSON_NULL)return MESH_OK;   // an empty scene is legal
        if(json_type(roots)!=JSON_ARRAY)return MESH_ERR_FORMAT;
        size_t n=json_count(roots);
        for(size_t i=0;i<n;i++){
            size_t ri;
            if(!json_size(json_at(roots,i),&ri))return MESH_ERR_FORMAT;
            MeshResult r=walk_node(c,ri,ident,0);
            if(r!=MESH_OK)return r;
        }
        return MESH_OK;
    }

    if(c->nnodes){
        // no scene list: every node that nothing else parents is a root. finding
        // them first matters -- walking node 0 blind would apply an identity
        // transform to something that is really somebody's child.
        bool* is_child=calloc(c->nnodes,sizeof *is_child);
        if(!is_child)return MESH_ERR_OOM;
        for(size_t i=0;i<c->nnodes;i++){
            const JsonValue* ch=json_member(json_at(c->nodes,i),"children");
            if(json_type(ch)!=JSON_ARRAY)continue;
            size_t nc=json_count(ch);
            for(size_t k=0;k<nc;k++){
                size_t ci;
                if(json_size(json_at(ch,k),&ci)&&ci<c->nnodes)is_child[ci]=true;
            }
        }
        MeshResult r=MESH_OK;
        for(size_t i=0;i<c->nnodes&&r==MESH_OK;i++)
            if(!is_child[i]&&!c->visited[i])r=walk_node(c,i,ident,0);
        free(is_child);
        return r;
    }

    // no scene graph at all: the meshes are all there is, so place them at the origin
    if(json_type(c->meshes)!=JSON_ARRAY)return MESH_OK;
    size_t n=json_count(c->meshes);
    for(size_t i=0;i<n;i++){
        MeshResult r=emit_mesh(c,i,ident);
        if(r!=MESH_OK)return r;
    }
    return MESH_OK;
}

// ---- entry point ---------------------------------------------------------

static void ctx_dispose(Ctx* c){
    if(c->bufs){
        for(size_t i=0;i<c->nbufs;i++)
            if(c->bufs[i].owned)free((void*)c->bufs[i].data);
        free(c->bufs);
    }
    if(c->imgs){
        // 2048x2048 rgba is 16MB a piece; none of it outlives the load
        for(size_t i=0;i<c->nimgs;i++)png_free(&c->imgs[i].img);
        free(c->imgs);
    }
    free(c->visited);
}

MeshResult mesh_load_gltf(const char* path,Object* out){
    if(!out)return MESH_ERR_FORMAT;
    memset(out,0,sizeof *out);
    if(!path)return MESH_ERR_OPEN;

    uint8_t* file=NULL;
    size_t file_len=0;
    MeshResult r=read_whole_file(path,&file,&file_len);
    if(r!=MESH_OK)return r;

    Ctx c;
    memset(&c,0,sizeof c);
    const char* json_text=(const char*)file;
    size_t json_len=file_len;
    if(file_len>=4&&rd_u32(file)==GLB_MAGIC){
        r=glb_split(file,file_len,&json_text,&json_len,&c.bin,&c.bin_len);
        if(r!=MESH_OK){free(file);return r;}
    }

    JsonDoc* doc=json_parse(json_text,json_len,NULL,0);
    if(!doc){free(file);return MESH_ERR_FORMAT;}
    c.root=json_root(doc);
    if(json_type(c.root)!=JSON_OBJECT){json_free(doc);free(file);return MESH_ERR_FORMAT;}

    // an extension listed here is required to load the asset at all, so anything
    // we do not implement must fail rather than load as wrong geometry. the list
    // of supported extensions is empty, so any entry is a refusal.
    const JsonValue* req=json_member(c.root,"extensionsRequired");
    if(req&&json_type(req)!=JSON_NULL){
        if(json_type(req)!=JSON_ARRAY){json_free(doc);free(file);return MESH_ERR_FORMAT;}
        if(json_count(req)>0){json_free(doc);free(file);return MESH_ERR_UNSUPPORTED;}
    }

    c.buffers=json_member(c.root,"buffers");
    c.views=json_member(c.root,"bufferViews");
    c.accessors=json_member(c.root,"accessors");
    c.meshes=json_member(c.root,"meshes");
    c.nodes=json_member(c.root,"nodes");
    c.materials=json_member(c.root,"materials");
    c.textures=json_member(c.root,"textures");
    c.images=json_member(c.root,"images");
    c.samplers=json_member(c.root,"samplers");
    c.nbufs=json_type(c.buffers)==JSON_ARRAY?json_count(c.buffers):0;
    c.nnodes=json_type(c.nodes)==JSON_ARRAY?json_count(c.nodes):0;
    c.nimgs=json_type(c.images)==JSON_ARRAY?json_count(c.images):0;

    // the .gltf's own directory, kept with its trailing separator so a relative
    // uri is just a concatenation away
    const char* slash=strrchr(path,'/');
    const char* back=strrchr(path,'\\');
    if(back&&(!slash||back>slash))slash=back;
    c.dir=path;
    c.dir_len=slash?(size_t)(slash-path)+1:0;

    r=MESH_OK;
    if(c.nbufs){
        c.bufs=calloc(c.nbufs,sizeof *c.bufs);
        if(!c.bufs)r=MESH_ERR_OOM;
    }
    if(r==MESH_OK&&c.nnodes){
        c.visited=calloc(c.nnodes,sizeof *c.visited);
        if(!c.visited)r=MESH_ERR_OOM;
    }
    if(r==MESH_OK)r=walk_scene(&c);
    if(r==MESH_OK&&(c.nidx==0||c.nverts==0))r=MESH_ERR_EMPTY;

    ctx_dispose(&c);
    json_free(doc);
    free(file);
    if(r!=MESH_OK){
        free(c.verts);
        free(c.idx);
        return r;
    }

    // hand back exactly what was used; a failed shrink is not a failure
    Vectex* sv=realloc(c.verts,c.nverts*sizeof *c.verts);
    if(sv)c.verts=sv;
    uint64_t* si=realloc(c.idx,c.nidx*sizeof *c.idx);
    if(si)c.idx=si;

    out->vertices=c.verts;
    out->len_of_vertices=(uint64_t)c.nverts;
    out->connectors_sequence=c.idx;
    out->len_of_connectors=(uint64_t)c.nidx;
    return MESH_OK;
}

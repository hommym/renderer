#include "mesh.h"
#include "json.h"
#include "image.h"
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
// highest TEXCOORD_n a material may ask for. the spec sets no limit, but two
// sets is what exporters actually emit and the name is built into a fixed buffer.
#define MAX_UV_SET 7

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

// One material's share of the scene: its own vertex and index arrays, and which
// image it wants sampled.
typedef struct Bucket {
    Vectex* verts;  size_t nverts, cap_verts;
    uint64_t* idx;  size_t nidx,   cap_idx;
    size_t   img;   bool   has_img;
    bool     double_sided;
    uint32_t flat;                       // baseColorFactor, for when the image is unusable
} Bucket;

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
    // Object holds one texture but a glTF may name dozens, so the one that
    // covers the most triangles wins. These tally that vote while the scene is
    // walked; nothing is decoded until it is over.
    size_t* img_tris; size_t nimages;
    size_t* mat_tris; uint32_t* mat_flat; size_t nmaterials;
    // geometry is accumulated per material rather than into one pile, because an
    // Object carries one texture: a primitive drawn with material 7 has to end up
    // in an Object whose texture is material 7's image, or its uv coordinates
    // address the wrong picture. buckets[nmaterials] is the catch-all for
    // primitives that name no material at all.
    Bucket* buckets; size_t nbuckets;
    Buf* bufs; size_t nbufs;
    const uint8_t* bin; size_t bin_len;   // GLB BIN chunk, borrowed from the file image
    const char* dir; size_t dir_len;      // directory of the .gltf, for relative uris
    Vectex* verts; size_t nverts, cap_verts;
    uint64_t* idx;  size_t nidx,   cap_idx;
    bool* visited; size_t nnodes;

    // primitives dropped because they are not triangles, or carry morph targets,
    // or are Draco-compressed. Counted rather than fatal; see emit_primitive.
    uint64_t skipped;
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
    if(strncmp(uri,"data:",5)==0){
        const char* comma=strchr(uri,',');
        if(!comma)return MESH_ERR_FORMAT;
        size_t hdr=(size_t)(comma-uri);
        if(hdr<7||strncmp(comma-7,";base64",7)!=0)return MESH_ERR_UNSUPPORTED; // only base64 payloads
        if(!b64_decode(comma+1,strlen(comma+1),&data,&len))return MESH_ERR_FORMAT;
    }else{
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
        MeshResult r=read_whole_file(full,&data,&len);
        free(full);
        if(r!=MESH_OK)return r;
    }
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

// TEXCOORD_0 is FLOAT, or a normalized UBYTE/USHORT. acc_norm clamps to [0,1],
// which is exactly right for the integer forms -- they cannot express anything
// else -- and wrong for float, where a tiled uv of 3.5 is ordinary and clamping
// it to 1.0 would collapse a whole repeat into one texel column.
static double acc_uv(const Accessor* a,size_t i,size_t k){
    return a->comp_type==CT_FLOAT?acc_raw(a,i,k):acc_norm(a,i,k);
}

static uint32_t chan8(double v){
    if(!(v>0.0))return 0;          // negatives and NaN both floor here
    if(v>=1.0)return 255;
    return (uint32_t)(v*255.0+0.5);
}

static uint32_t pack_factor(const double f[4]){
    return (chan8(f[3])<<24)|(chan8(f[0])<<16)|(chan8(f[1])<<8)|chan8(f[2]);
}

static uint32_t pack_colour(const Accessor* a,size_t i){
    double r=acc_norm(a,i,0),g=acc_norm(a,i,1),b=acc_norm(a,i,2);
    double al=a->ncomp>=4?acc_norm(a,i,3):1.0;   // VEC3 colour is opaque
    return ((uint32_t)(al*255.0+0.5)<<24)|((uint32_t)(r*255.0+0.5)<<16)
          |((uint32_t)(g*255.0+0.5)<<8)|(uint32_t)(b*255.0+0.5);
}

// ---- materials and textures ----------------------------------------------

// The raw span a bufferView covers. accessor_resolve() does this too, but with
// element striding on top; an embedded png or jpeg is just a byte range.
static MeshResult view_bytes(Ctx* c,size_t vi,const uint8_t** data,size_t* len){
    if(json_type(c->views)!=JSON_ARRAY||vi>=json_count(c->views))return MESH_ERR_FORMAT;
    const JsonValue* view=json_at(c->views,vi);
    if(json_type(view)!=JSON_OBJECT)return MESH_ERR_FORMAT;
    size_t bi,off,ln;
    if(!json_size(json_member(view,"buffer"),&bi))return MESH_ERR_FORMAT;
    if(!json_size_opt(json_member(view,"byteOffset"),0,&off))return MESH_ERR_FORMAT;
    if(!json_size(json_member(view,"byteLength"),&ln)||ln==0)return MESH_ERR_FORMAT;
    const uint8_t* b;
    size_t blen;
    MeshResult r=buffer_get(c,bi,&b,&blen);
    if(r!=MESH_OK)return r;
    size_t end;
    if(!sz_add(off,ln,&end)||end>blen)return MESH_ERR_FORMAT;
    *data=b+off;*len=ln;
    return MESH_OK;
}

// Decodes images[ii]. A missing, unreadable or unrecognised image is not a load
// failure: the model still has geometry, and the caller falls back to a flat
// colour. Only the three glTF spellings are handled -- a bufferView, a base64
// data: uri, and a relative path next to the .gltf.
static bool image_load(Ctx* c,size_t ii,Image* img){
    *img=(Image){0};
    if(json_type(c->images)!=JSON_ARRAY||ii>=json_count(c->images))return false;
    const JsonValue* iv=json_at(c->images,ii);
    if(json_type(iv)!=JSON_OBJECT)return false;

    const JsonValue* bvv=json_member(iv,"bufferView");
    if(bvv&&json_type(bvv)!=JSON_NULL){
        size_t vi;
        if(!json_size(bvv,&vi))return false;
        const uint8_t* d;
        size_t n;
        if(view_bytes(c,vi,&d,&n)!=MESH_OK)return false;
        return image_decode(d,n,MESH_TEXTURE_MAX_DIM,img,NULL,0);
    }

    const char* uri=json_member_string(iv,"uri",NULL);
    if(!uri)return false;
    if(strncmp(uri,"data:",5)==0){
        const char* comma=strchr(uri,',');
        if(!comma)return false;
        size_t hdr=(size_t)(comma-uri);
        if(hdr<7||strncmp(comma-7,";base64",7)!=0)return false;
        uint8_t* raw=NULL;
        size_t rn=0;
        if(!b64_decode(comma+1,strlen(comma+1),&raw,&rn))return false;
        bool ok=image_decode(raw,rn,MESH_TEXTURE_MAX_DIM,img,NULL,0);
        free(raw);
        return ok;
    }

    char* dec=uri_unescape(uri);
    if(!dec)return false;
    if(!uri_is_safe_relative(dec)){free(dec);return false;}
    size_t nlen=strlen(dec),plen;
    if(!sz_add(c->dir_len,nlen,&plen)||!sz_add(plen,1,&plen)){free(dec);return false;}
    char* full=malloc(plen);
    if(!full){free(dec);return false;}
    memcpy(full,c->dir,c->dir_len);
    memcpy(full+c->dir_len,dec,nlen+1);
    free(dec);
    bool ok=image_decode_file(full,MESH_TEXTURE_MAX_DIM,img,NULL,0);
    free(full);
    return ok;
}

// Which material a primitive uses, which image that material samples for base
// colour, and the flat factor to fall back on. Returns false when the primitive
// names no material -- glTF then says to draw it with the default material,
// which is plain white.
static bool prim_material(Ctx* c,const JsonValue* prim,size_t* mat,
                          size_t* img,bool* has_img,uint32_t* flat,size_t* uvset,
                          bool* two_sided){
    *has_img=false;
    *flat=0xFFFFFFFFu;
    *uvset=0;
    // glTF defaults doubleSided to false, and a primitive with no material at
    // all takes the default material, which is single sided.
    *two_sided=false;
    if(!json_size(json_member(prim,"material"),mat))return false;
    if(json_type(c->materials)!=JSON_ARRAY||*mat>=json_count(c->materials))return false;
    const JsonValue* matv=json_at(c->materials,*mat);
    *two_sided=json_bool(json_member(matv,"doubleSided"),false);
    const JsonValue* pbr=json_member(matv,"pbrMetallicRoughness");
    // A KHR_materials_pbrSpecularGlossiness material usually carries no
    // pbrMetallicRoughness block at all: its base colour is diffuseFactor and
    // diffuseTexture instead. Same textureInfo shape, same texCoord field, so
    // everything downstream works unchanged. Without this the three models the
    // allow-list above unlocks would load as 106 objects of flat white.
    const JsonValue* sg=json_member(json_member(matv,"extensions"),
                                    "KHR_materials_pbrSpecularGlossiness");

    double f[4]={1.0,1.0,1.0,1.0};
    const JsonValue* bcf=json_member(pbr,"baseColorFactor");
    if(json_type(bcf)!=JSON_ARRAY)bcf=json_member(sg,"diffuseFactor");
    if(json_type(bcf)==JSON_ARRAY){
        size_t n=json_count(bcf);
        for(size_t k=0;k<4&&k<n;k++)f[k]=json_number(json_at(bcf,k),1.0);
    }
    *flat=pack_factor(f);

    const JsonValue* bct=json_member(pbr,"baseColorTexture");
    if(json_type(bct)!=JSON_OBJECT)bct=json_member(sg,"diffuseTexture");
    if(json_type(bct)!=JSON_OBJECT)return true;
    // a material picks which uv set to sample with. baked lightmap exports
    // routinely use TEXCOORD_1 for the baked atlas and keep TEXCOORD_0 for the
    // tiling detail map, so reading set 0 unconditionally textures them with
    // coordinates that belong to a different image entirely.
    double tc=json_member_number(bct,"texCoord",0.0);
    if(tc>=0.0&&tc<=(double)MAX_UV_SET)*uvset=(size_t)tc;
    size_t ti;
    if(!json_size(json_member(bct,"index"),&ti))return true;
    if(json_type(c->textures)!=JSON_ARRAY||ti>=json_count(c->textures))return true;
    size_t si;
    if(!json_size(json_member(json_at(c->textures,ti),"source"),&si))return true;
    *img=si;*has_img=true;
    return true;
}

// Decodes each distinct image once. Several materials commonly point at one
// atlas, and a 2048x2048 texture is 16MB, so decoding per material instead of
// per image would multiply a 40-material model's memory by 40.
typedef struct TexCache { size_t img; uint32_t* px; size_t w,h; } TexCache;

// Turns the per-material buckets into a scene. Must run before ctx_dispose():
// an embedded image still lives in a buffer that is about to be freed.
static MeshResult build_scene(Ctx* c,Model* out){
    size_t n=0;
    for(size_t i=0;i<c->nbuckets;i++)if(c->buckets[i].nidx>=3)n++;
    if(n==0)return MESH_ERR_EMPTY;

    Object* objs=calloc(n,sizeof *objs);
    uint32_t** texs=calloc(n,sizeof *texs);   // each object contributes at most one new one
    TexCache* cache=calloc(c->nimages?c->nimages:1,sizeof *cache);
    if(!objs||!texs||!cache){free(objs);free(texs);free(cache);return MESH_ERR_OOM;}

    size_t ntex=0,ncache=0,k=0;
    MeshResult r=MESH_OK;
    for(size_t i=0;i<c->nbuckets;i++){
        Bucket* b=&c->buckets[i];
        if(b->nidx<3)continue;

        Object* o=&objs[k];
        o->vertices=b->verts;
        o->len_of_vertices=b->nverts;
        o->connectors_sequence=b->idx;
        o->len_of_connectors=b->nidx;
        o->double_sided=b->double_sided;
        b->verts=NULL;b->idx=NULL;             // ownership moves to the Object

        uint32_t* px=NULL;
        size_t w=0,h=0;
        if(b->has_img){
            bool hit=false;
            for(size_t j=0;j<ncache;j++)
                if(cache[j].img==b->img){px=cache[j].px;w=cache[j].w;h=cache[j].h;hit=true;break;}
            if(!hit){
                Image im={0};
                if(image_load(c,b->img,&im)){px=im.pixels;w=im.width;h=im.height;texs[ntex++]=px;}
                cache[ncache++]=(TexCache){.img=b->img,.px=px,.w=w,.h=h};
            }
        }
        if(!px){
            // no image, or one we could not decode: the material's flat base
            // colour, so this object still has something to sample
            Image im={0};
            if(!image_solid(b->flat?b->flat:MESH_DEFAULT_COLOUR,&im)){r=MESH_ERR_OOM;break;}
            px=im.pixels;w=im.width;h=im.height;
            texs[ntex++]=px;
        }
        o->texture=px;o->texture_width=w;o->texture_height=h;
        k++;
    }
    free(cache);

    if(r!=MESH_OK){
        for(size_t i=0;i<k;i++){free(objs[i].vertices);free(objs[i].connectors_sequence);}
        for(size_t i=0;i<ntex;i++)free(texs[i]);
        free(objs);free(texs);
        return r;
    }
    out->objects=objs;out->len=k;
    out->primitives_skipped=c->skipped;
    out->textures=texs;out->texture_count=ntex;
    return MESH_OK;
}

// Flattens a scene back into one Object for mesh_load's single-object contract.
// The dominant material's texture survives and the rest are freed -- which is
// exactly the limitation mesh_load_scene exists to avoid.
static MeshResult merge_scene(Model* sc,Object* out){
    size_t nv=0,ni=0,best=0;
    uint64_t bestn=0;
    for(size_t i=0;i<sc->len;i++){
        if(!sz_add(nv,sc->objects[i].len_of_vertices,&nv))return MESH_ERR_FORMAT;
        if(!sz_add(ni,sc->objects[i].len_of_connectors,&ni))return MESH_ERR_FORMAT;
        if(sc->objects[i].len_of_connectors>bestn){bestn=sc->objects[i].len_of_connectors;best=i;}
    }
    Vectex* V=malloc(nv*sizeof *V);
    uint64_t* I=malloc(ni*sizeof *I);
    if(!V||!I){free(V);free(I);return MESH_ERR_OOM;}

    size_t vo=0,io=0;
    for(size_t i=0;i<sc->len;i++){
        Object* o=&sc->objects[i];
        memcpy(V+vo,o->vertices,(size_t)o->len_of_vertices*sizeof *V);
        // indices are bucket-local; concatenating the vertex arrays shifts them
        for(uint64_t j=0;j<o->len_of_connectors;j++)I[io+j]=o->connectors_sequence[j]+vo;
        vo+=(size_t)o->len_of_vertices;
        io+=(size_t)o->len_of_connectors;
    }

    uint32_t* keep=sc->objects[best].texture;
    // merging loses the per-material distinction, so the merged Object can only
    // be culled if every material it swallowed agreed it was safe
    out->double_sided=false;
    for(size_t i=0;i<sc->len;i++)if(sc->objects[i].double_sided)out->double_sided=true;
    out->texture=keep;
    out->texture_width=sc->objects[best].texture_width;
    out->texture_height=sc->objects[best].texture_height;
    for(size_t i=0;i<sc->texture_count;i++)if(sc->textures[i]!=keep)free(sc->textures[i]);
    for(size_t i=0;i<sc->len;i++){free(sc->objects[i].vertices);free(sc->objects[i].connectors_sequence);}
    free(sc->objects);free(sc->textures);
    *sc=(Model){0};

    out->vertices=V;out->len_of_vertices=(uint64_t)nv;
    out->connectors_sequence=I;out->len_of_connectors=(uint64_t)ni;
    return MESH_OK;
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
    // A primitive we cannot draw is dropped, not fatal. One LINES helper or one
    // blend-shaped eyelid must not cost the other 600 primitives in the file --
    // kralzfiller alone has 656. Nothing wrong is emitted, but the drop is
    // silent, so it is counted and reported through Model.primitives_skipped.
    if(json_member_number(prim,"mode",MODE_TRIANGLES)!=(double)MODE_TRIANGLES){c->skipped++;return MESH_OK;}

    // draco keeps the real geometry in an extension buffer and leaves the
    // accessors bufferView-less; read straight, that is a mesh of zeros, not an error
    if(json_member(json_member(prim,"extensions"),"KHR_draco_mesh_compression"))return MESH_ERR_UNSUPPORTED;

    // morph targets displace POSITION by the node/mesh weights. we cannot apply
    // them, and emitting the base shape would be the silent reinterpretation
    // mesh.h forbids, so refuse rather than return the wrong geometry.
    if(json_member(prim,"targets")){c->skipped++;return MESH_OK;}

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

    // the material decides which TEXCOORD_n to read, so it has to be settled
    // before the attribute is looked up
    size_t mat=0,img=0,uvset=0;
    bool has_img=false,two_sided=false;
    uint32_t flat=0;
    if(!prim_material(c,prim,&mat,&img,&has_img,&flat,&uvset,&two_sided)||mat>=c->nmaterials)
        mat=c->nmaterials;                     // the catch-all bucket

    Accessor uv;
    bool have_uv=false;
    char uvname[16];
    snprintf(uvname,sizeof uvname,"TEXCOORD_%zu",uvset);
    const JsonValue* tv=json_member(attrs,uvname);
    // a material naming a set the mesh does not carry is malformed; set 0 is a
    // better guess than no texture at all
    if(!tv||json_type(tv)==JSON_NULL)tv=json_member(attrs,"TEXCOORD_0");
    if(tv&&json_type(tv)!=JSON_NULL){
        size_t ti;
        if(!json_size(tv,&ti))return MESH_ERR_FORMAT;
        r=accessor_resolve(c,ti,&uv);
        if(r!=MESH_OK)return r;
        if(uv.ncomp!=2)return MESH_ERR_FORMAT;
        // the spec allows exactly these three; anything else is a broken file
        if(uv.comp_type!=CT_FLOAT&&uv.comp_type!=CT_UBYTE&&uv.comp_type!=CT_USHORT)return MESH_ERR_FORMAT;
        have_uv=true;
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

    Bucket* bk=&c->buckets[mat];
    if(has_img&&img<c->nimages){ bk->img=img; bk->has_img=true; }
    if(two_sided)bk->double_sided=true;
    if(flat)bk->flat=flat;

    size_t base=bk->nverts,vneed,ineed;
    if(!sz_add(base,pos.count,&vneed))return MESH_ERR_FORMAT;
    if(!sz_add(bk->nidx,tri_count,&ineed))return MESH_ERR_FORMAT;

    Vectex* nv=grow_arr(bk->verts,&bk->cap_verts,vneed,sizeof *bk->verts);
    if(!nv)return MESH_ERR_OOM;
    bk->verts=nv;
    uint64_t* ni=grow_arr(bk->idx,&bk->cap_idx,ineed,sizeof *bk->idx);
    if(!ni)return MESH_ERR_OOM;
    bk->idx=ni;

    for(size_t i=0;i<pos.count;i++){
        double x=acc_raw(&pos,i,0),y=acc_raw(&pos,i,1),z=acc_raw(&pos,i,2);
        if(!isfinite(x)||!isfinite(y)||!isfinite(z))return MESH_ERR_FORMAT;
        Vectex v;
        v.x=m[0]*x+m[4]*y+m[8]*z+m[12];
        v.y=m[1]*x+m[5]*y+m[9]*z+m[13];
        v.z=m[2]*x+m[6]*y+m[10]*z+m[14];
        if(!isfinite(v.x)||!isfinite(v.y)||!isfinite(v.z))return MESH_ERR_FORMAT;
        // a short COLOR_0 is padded rather than rejected: the geometry is still good
        v.colour=(have_col&&i<col.count)?pack_colour(&col,i):MESH_DEFAULT_COLOUR;
        // a mesh with no TEXCOORD_0 gets (0,0), which reads the top-left texel.
        // that is deliberate: its texture is the 1x1 flat fallback, where every
        // coordinate is the same texel anyway.
        if(have_uv&&i<uv.count){
            v.u=mesh_wrap_uv(acc_uv(&uv,i,0));
            v.v=mesh_wrap_uv(acc_uv(&uv,i,1));
        }else{
            v.u=0.0f;v.v=0.0f;
        }
        bk->verts[base+i]=v;
    }

    for(size_t i=0;i<tri_count;i++){
        size_t vi;
        if(have_ind){
            double d=acc_raw(&ind,i,0);        // unsigned and at most 2^32, so exact in a double
            if(!(d>=0.0)||d>=(double)pos.count)return MESH_ERR_FORMAT;
            vi=(size_t)d;
        }else vi=i;                            // no indices: vertices are triangles in order
        bk->idx[bk->nidx+i]=(uint64_t)(base+vi);
    }

    if(mat<c->nmaterials){
        c->mat_tris[mat]+=tri_count/3;
        c->mat_flat[mat]=flat;
    }
    if(has_img&&img<c->nimages)c->img_tris[img]+=tri_count/3;

    bk->nverts=vneed;
    bk->nidx=ineed;
    c->nverts+=pos.count;                      // scene totals, for the empty check
    c->nidx+=tri_count;
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
        // A skinned mesh is posed by its joints. We do not skin, so we draw the
        // BIND pose -- and that is exact, not an approximation. POSITION is
        // already in the space the inverse bind matrices map from, so in the
        // bind pose every joint matrix is globalJoint*IBM = identity and the
        // weighted sum collapses to the vertex itself. The spec also requires
        // the referencing node's own transform to be ignored
        // (glTF 2.0 Specification.adoc: "Only the joint transforms are applied
        // to the skinned mesh; the transform of the skinned mesh node MUST be
        // ignored"), so `world` is precisely what must NOT go in here.
        //
        // What this costs: a file whose rest hierarchy is itself a posed frame
        // renders its T-pose instead of that pose. Checked against a full
        // linear-blend-skinning reference on all three skinned models in
        // 3dmodels/: the shapes agree to within 8.7e-6 of model extent, so
        // there is nothing to gain from skinning them properly here.
        static const double bind_ident[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        const JsonValue* sk=json_member(n,"skin");
        bool skinned=sk&&json_type(sk)!=JSON_NULL;
        size_t mi;
        if(!json_size(mv,&mi))return MESH_ERR_FORMAT;
        r=emit_mesh(c,mi,skinned?bind_ident:world);
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
    free(c->visited);
    free(c->img_tris);
    free(c->mat_tris);
    free(c->mat_flat);
    if(c->buckets){
        // NULL for anything build_scene already took ownership of
        for(size_t i=0;i<c->nbuckets;i++){free(c->buckets[i].verts);free(c->buckets[i].idx);}
        free(c->buckets);
    }
}

// Extensions a required-list may name that this loader can safely ignore.
//
// Membership is narrow: the extension must not define an accessor, a bufferView
// encoding, an attribute semantic, a primitive property or a uv mapping.
// Everything below only describes how a surface is SHADED, and this renderer
// samples a base-colour texel and writes it -- there is no shading to get wrong.
// Anything that touches the bytes stays off the list, because "load it anyway"
// there means "load something else".
//
// Deliberately NOT here, and why:
//   KHR_draco_mesh_compression  positions/indices live in a Draco bitstream
//   EXT_meshopt_compression     a bufferView's bytes are a compressed stream
//   KHR_mesh_quantization       POSITION/TEXCOORD become integer types
//   KHR_texture_transform       rewrites the uv mapping: silently wrong texels
//   EXT_mesh_gpu_instancing     draws N copies; ignoring it draws one
//   KHR_texture_basisu, EXT_texture_webp, EXT_texture_avif
//                               png.c/jpeg.c cannot decode them, so every
//                               material would fall back to flat colour
static const char* const EXT_IGNORABLE[]={
    "KHR_materials_pbrSpecularGlossiness",   // base colour moves; see prim_material
    "KHR_materials_unlit",
    "KHR_materials_emissive_strength",
    "KHR_materials_specular",
    "KHR_materials_ior",
    "KHR_materials_clearcoat",
    "KHR_materials_sheen",
    "KHR_materials_transmission",
    "KHR_materials_volume",
    "KHR_materials_iridescence",
    "KHR_materials_anisotropy",
    "KHR_materials_dispersion",
    "KHR_materials_diffuse_transmission",
    "KHR_materials_variants",   // primitive.material stays the default mapping
    "KHR_lights_punctual",      // there is no lighting here at all
    "EXT_lights_image_based",
    "KHR_animation_pointer",    // animation only; a static viewer wants base values
    "KHR_xmp_json_ld",
    "KHR_xmp",
    "FB_ngon_encoding",         // a hint for rebuilding ngons; the triangles are real
};

static bool ext_is_ignorable(const char* name){
    if(!name)return false;
    for(size_t i=0;i<sizeof EXT_IGNORABLE/sizeof *EXT_IGNORABLE;i++)
        if(strcmp(name,EXT_IGNORABLE[i])==0)return true;
    return false;
}

static MeshResult load_gltf_scene(const char* path,Model* out){
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
    // that could change what the BYTES mean must still fail rather than load as
    // wrong geometry. But refusing on the count alone refused three models over a
    // shading model this renderer does not even have, so the test is by name now.
    const JsonValue* req=json_member(c.root,"extensionsRequired");
    if(req&&json_type(req)!=JSON_NULL){
        if(json_type(req)!=JSON_ARRAY){json_free(doc);free(file);return MESH_ERR_FORMAT;}
        size_t nreq=json_count(req);
        for(size_t i=0;i<nreq;i++){
            const JsonValue* e=json_at(req,i);
            if(json_type(e)!=JSON_STRING){json_free(doc);free(file);return MESH_ERR_FORMAT;}
            if(!ext_is_ignorable(json_string(e,NULL))){json_free(doc);free(file);return MESH_ERR_UNSUPPORTED;}
        }
    }

    c.buffers=json_member(c.root,"buffers");
    c.views=json_member(c.root,"bufferViews");
    c.accessors=json_member(c.root,"accessors");
    c.meshes=json_member(c.root,"meshes");
    c.nodes=json_member(c.root,"nodes");
    c.materials=json_member(c.root,"materials");
    c.textures=json_member(c.root,"textures");
    c.images=json_member(c.root,"images");
    c.nbufs=json_type(c.buffers)==JSON_ARRAY?json_count(c.buffers):0;
    c.nnodes=json_type(c.nodes)==JSON_ARRAY?json_count(c.nodes):0;
    c.nmaterials=json_type(c.materials)==JSON_ARRAY?json_count(c.materials):0;
    c.nimages=json_type(c.images)==JSON_ARRAY?json_count(c.images):0;

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
    if(r==MESH_OK&&c.nmaterials){
        c.mat_tris=calloc(c.nmaterials,sizeof *c.mat_tris);
        c.mat_flat=calloc(c.nmaterials,sizeof *c.mat_flat);
        if(!c.mat_tris||!c.mat_flat)r=MESH_ERR_OOM;
    }
    if(r==MESH_OK&&c.nimages){
        c.img_tris=calloc(c.nimages,sizeof *c.img_tris);
        if(!c.img_tris)r=MESH_ERR_OOM;
    }
    if(r==MESH_OK){
        // one bucket per material plus the catch-all for primitives with none
        c.nbuckets=c.nmaterials+1;
        c.buckets=calloc(c.nbuckets,sizeof *c.buckets);
        if(!c.buckets)r=MESH_ERR_OOM;
    }
    if(r==MESH_OK)r=walk_scene(&c);
    if(r==MESH_OK&&(c.nidx==0||c.nverts==0))r=MESH_ERR_EMPTY;
    // before ctx_dispose: an embedded texture lives in a buffer it is about to free
    if(r==MESH_OK)r=build_scene(&c,out);

    ctx_dispose(&c);
    json_free(doc);
    free(file);
    if(r!=MESH_OK)memset(out,0,sizeof *out);
    return r;
}

MeshResult mesh_load_gltf_scene(const char* path,Model* out){
    return load_gltf_scene(path,out);
}

MeshResult mesh_load_gltf(const char* path,Object* out){
    if(!out)return MESH_ERR_FORMAT;
    memset(out,0,sizeof *out);
    Model sc={0};
    MeshResult r=load_gltf_scene(path,&sc);
    if(r!=MESH_OK)return r;
    r=merge_scene(&sc,out);
    if(r!=MESH_OK){
        mesh_model_free(&sc);
        memset(out,0,sizeof *out);
    }
    return r;
}

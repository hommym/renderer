#include "png.h"
#include "inflate.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// PNG reader. Every number in the file is a claim, not a fact: a length is
// checked against the bytes actually present before anything is read through it,
// every size arithmetic is checked for overflow before it reaches malloc, and
// every chunk's CRC is verified -- a flipped bit has to be an error, because the
// alternative is a field of garbage pixels that looks like a renderer bug.
//
// Output is always straight 8-bit RGBA. All the colour-type and bit-depth
// branching happens once here, in expand(), so no caller ever sees it.

// the spec's own dimension limit. it bounds nothing on its own -- every size
// derived from it is still checked with sz_mul -- it just rejects the absurd early.
#define PNG_MAX_DIM 0x7FFFFFFFu

[[gnu::format(printf,3,4)]]
static void seterr(char* err,size_t n,const char* fmt,...){
    if(!err||!n)return;
    va_list ap; va_start(ap,fmt); vsnprintf(err,n,fmt,ap); va_end(ap);
}

static bool sz_add(size_t a,size_t b,size_t* r){ if(a>SIZE_MAX-b)return false; *r=a+b; return true; }
static bool sz_mul(size_t a,size_t b,size_t* r){ if(a&&b>SIZE_MAX/a)return false; *r=a*b; return true; }

// ---- crc32 (reflected 0xEDB88320) ----------------------------------------

// a nibble at a time: sixteen entries are const, so there is no lazy build and
// no shared mutable state for two decoding threads to race on. entry i is the
// polynomial folded through four zero bits.
static const uint32_t crc_nib[16]={
    0x00000000u,0x1DB71064u,0x3B6E20C8u,0x26D930ACu,
    0x76DC4190u,0x6B6B51F4u,0x4DB26158u,0x5005713Cu,
    0xEDB88320u,0xF00F9344u,0xD6D6A3E8u,0xCB61B38Cu,
    0x9B64C2B0u,0x86D3D2D4u,0xA00AE278u,0xBDBDF21Cu
};

static uint32_t crc32_of(const uint8_t* p,size_t n){
    uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++){
        uint32_t b=p[i];
        c=crc_nib[(c^b)&0xFu]^(c>>4);
        c=crc_nib[(c^(b>>4))&0xFu]^(c>>4);
    }
    return c^0xFFFFFFFFu;
}

// ---- byte order ----------------------------------------------------------

static uint32_t rd_be32(const uint8_t* p){
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3];
}
static uint32_t rd_be16(const uint8_t* p){ return ((uint32_t)p[0]<<8)|(uint32_t)p[1]; }

static bool is_type(const uint8_t* t,const char* s){ return memcmp(t,s,4)==0; }

// chunk types land in error messages, so a crafted file cannot inject control bytes
static void type_name(const uint8_t* t,char out[5]){
    for(int i=0;i<4;i++) out[i]=(t[i]>=0x20u&&t[i]<0x7Fu)?(char)t[i]:'?';
    out[4]='\0';
}

// ---- header validity -----------------------------------------------------

static uint32_t chan_count(uint32_t ctype){
    switch(ctype){
    case 0: case 3: return 1;
    case 4: return 2;
    case 2: return 3;
    case 6: return 4;
    }
    return 0;
}

static bool depth_ok(uint32_t ctype,uint32_t depth){
    switch(ctype){
    case 0: return depth==1||depth==2||depth==4||depth==8||depth==16;
    case 3: return depth==1||depth==2||depth==4||depth==8;
    case 2: case 4: case 6: return depth==8||depth==16;
    }
    return false;
}

// everything expand() needs, so the per-pixel loops take one pointer
typedef struct Img {
    uint32_t w,h,nchan,depth,ctype;
    size_t rowbytes,stride;
    const uint8_t* plte; size_t plte_n;   // borrowed from the caller's file image
    const uint8_t* trns; size_t trns_n;
    bool has_key; uint32_t key[3];        // tRNS as a colour key, types 0 and 2
} Img;

// ---- unfiltering ---------------------------------------------------------

static uint8_t paeth(uint8_t a,uint8_t b,uint8_t c){
    int p=(int)a+(int)b-(int)c;
    int pa=abs(p-(int)a),pb=abs(p-(int)b),pc=abs(p-(int)c);
    if(pa<=pb&&pa<=pc)return a;            // ties prefer a, then b
    return pb<=pc?b:c;
}

// in place: the filter byte stays put and each row's data is reconstructed over
// itself, so the previous row is just the previous slice of the same buffer.
static bool unfilter(uint8_t* raw,const Img* im,size_t bpp,char* err,size_t elen){
    const uint8_t* prev=NULL;
    for(size_t y=0;y<im->h;y++){
        uint8_t f=raw[y*im->stride];
        uint8_t* cur=raw+y*im->stride+1;
        size_t n=im->rowbytes;
        switch(f){
        case 0: break;
        case 1:
            for(size_t i=bpp;i<n;i++) cur[i]=(uint8_t)(cur[i]+cur[i-bpp]);
            break;
        case 2:
            if(prev) for(size_t i=0;i<n;i++) cur[i]=(uint8_t)(cur[i]+prev[i]);
            break;
        case 3:
            for(size_t i=0;i<n;i++){
                uint32_t a=i>=bpp?cur[i-bpp]:0u, b=prev?prev[i]:0u;
                cur[i]=(uint8_t)(cur[i]+(a+b)/2u);
            }
            break;
        case 4:
            for(size_t i=0;i<n;i++){
                uint8_t a=i>=bpp?cur[i-bpp]:(uint8_t)0;
                uint8_t b=prev?prev[i]:(uint8_t)0;
                uint8_t c=(prev&&i>=bpp)?prev[i-bpp]:(uint8_t)0;
                cur[i]=(uint8_t)(cur[i]+paeth(a,b,c));
            }
            break;
        default:
            seterr(err,elen,"row %zu: filter type %u is not 0-4",y,(unsigned)f);
            return false;
        }
        prev=cur;
    }
    return true;
}

// ---- expansion to rgba8 --------------------------------------------------

// sample i of a row at the file's native bit depth, MSB-first within a byte.
// returns the raw value, unscaled, because tRNS colour keys compare against it.
static uint32_t get_samp(const uint8_t* row,size_t i,uint32_t depth){
    if(depth==8)return row[i];
    if(depth==16)return ((uint32_t)row[i*2]<<8)|row[i*2+1];
    uint32_t spb=8u/depth;
    uint32_t shift=8u-depth*((uint32_t)(i%spb)+1u);
    return ((uint32_t)row[i/spb]>>shift)&((1u<<depth)-1u);
}

// full-range scale, not a shift: 4-bit 15 has to reach 255, not 240
static uint8_t scale8(uint32_t v,uint32_t depth){
    switch(depth){
    case 1: return (uint8_t)(v*255u);
    case 2: return (uint8_t)(v*85u);
    case 4: return (uint8_t)(v*17u);
    case 8: return (uint8_t)v;
    default: return (uint8_t)(v>>8);       // 16-bit: keep the high byte
    }
}

static bool expand(const Img* im,const uint8_t* raw,uint8_t* rgba,char* err,size_t elen){
    uint32_t d=im->depth;
    for(size_t y=0;y<im->h;y++){
        const uint8_t* row=raw+y*im->stride+1;
        uint8_t* o=rgba+y*(size_t)im->w*4u;
        switch(im->ctype){
        case 0:
            for(size_t x=0;x<im->w;x++,o+=4){
                uint32_t s=get_samp(row,x,d);
                o[0]=o[1]=o[2]=scale8(s,d);
                o[3]=(uint8_t)((im->has_key&&s==im->key[0])?0u:255u);
            }
            break;
        case 2:
            for(size_t x=0;x<im->w;x++,o+=4){
                uint32_t r=get_samp(row,x*3u,d),g=get_samp(row,x*3u+1u,d),b=get_samp(row,x*3u+2u,d);
                o[0]=scale8(r,d); o[1]=scale8(g,d); o[2]=scale8(b,d);
                bool clear=im->has_key&&r==im->key[0]&&g==im->key[1]&&b==im->key[2];
                o[3]=(uint8_t)(clear?0u:255u);
            }
            break;
        case 3:
            for(size_t x=0;x<im->w;x++,o+=4){
                size_t i=get_samp(row,x,d);
                if(i>=im->plte_n){
                    seterr(err,elen,"pixel (%zu,%zu): palette index %zu but PLTE has %zu entries",
                           x,y,i,im->plte_n);
                    return false;
                }
                o[0]=im->plte[i*3]; o[1]=im->plte[i*3+1]; o[2]=im->plte[i*3+2];
                o[3]=i<im->trns_n?im->trns[i]:(uint8_t)255;   // entries past tRNS are opaque
            }
            break;
        case 4:
            for(size_t x=0;x<im->w;x++,o+=4){
                o[0]=o[1]=o[2]=scale8(get_samp(row,x*2u,d),d);
                o[3]=scale8(get_samp(row,x*2u+1u,d),d);
            }
            break;
        default:
            for(size_t x=0;x<im->w;x++,o+=4){
                o[0]=scale8(get_samp(row,x*4u,d),d);
                o[1]=scale8(get_samp(row,x*4u+1u,d),d);
                o[2]=scale8(get_samp(row,x*4u+2u,d),d);
                o[3]=scale8(get_samp(row,x*4u+3u,d),d);
            }
            break;
        }
    }
    return true;
}

// ---- idat accumulation ---------------------------------------------------

// the IDATs are one zlib stream cut at arbitrary points -- up to ~46 pieces in
// the models this was written for -- so they have to be joined before inflating.
static bool buf_append(uint8_t** buf,size_t* n,size_t* cap,const uint8_t* src,size_t add){
    size_t need;
    if(!sz_add(*n,add,&need))return false;
    if(need>*cap){
        size_t nc=*cap?*cap:16384u;
        while(nc<need){ if(nc>SIZE_MAX/2)return false; nc*=2; }
        uint8_t* q=realloc(*buf,nc);
        if(!q)return false;
        *buf=q; *cap=nc;
    }
    memcpy(*buf+*n,src,add);
    *n=need;
    return true;
}

// ---- decode --------------------------------------------------------------

bool png_decode(const void* data,size_t len,PngImage* out,char* err,size_t elen){
    static const uint8_t sig[8]={0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A};
    const uint8_t* p=(const uint8_t*)data;
    uint8_t *idat=NULL,*raw=NULL,*rgba=NULL;
    size_t idat_n=0,idat_cap=0,off=8,npix=0,rgba_sz=0,raw_sz=0,bits=0,tmp=0,got=0,bpp=0;
    bool have_ihdr=false,have_idat=false,idat_closed=false,have_iend=false;
    char sub[224],name[5];
    Img im={0};

    if(err&&elen)err[0]='\0';
    if(!out)return false;
    out->width=0; out->height=0; out->rgba=NULL;

    if(!p||len<8||memcmp(p,sig,8)!=0){ seterr(err,elen,"not a PNG: bad signature"); return false; }

    while(1){
        if(len-off<8){ seterr(err,elen,"truncated: no chunk header at offset %zu",off); goto fail; }
        uint32_t clen=rd_be32(p+off);
        const uint8_t* type=p+off+4;
        type_name(type,name);
        if(clen>0x7FFFFFFFu){ seterr(err,elen,"chunk '%s' length %u exceeds 2^31-1",name,clen); goto fail; }
        if((size_t)clen+4u>len-off-8u){
            seterr(err,elen,"chunk '%s' at offset %zu claims %u bytes, %zu remain",
                   name,off,clen,len-off-8u);
            goto fail;
        }
        const uint8_t* body=p+off+8;
        uint32_t want=rd_be32(body+clen),have=crc32_of(type,(size_t)clen+4u);
        if(want!=have){
            seterr(err,elen,"chunk '%s' at offset %zu is corrupt: CRC %08X, expected %08X",
                   name,off,have,want);
            goto fail;
        }
        if(!have_ihdr&&!is_type(type,"IHDR")){
            seterr(err,elen,"first chunk is '%s', expected IHDR",name); goto fail;
        }
        if(have_idat&&!is_type(type,"IDAT"))idat_closed=true;   // IDATs must be one run

        if(is_type(type,"IHDR")){
            if(have_ihdr||off!=8){ seterr(err,elen,"IHDR must be the first chunk and appear once"); goto fail; }
            if(clen!=13){ seterr(err,elen,"IHDR is %u bytes, expected 13",clen); goto fail; }
            im.w=rd_be32(body); im.h=rd_be32(body+4);
            im.depth=body[8]; im.ctype=body[9];
            uint32_t comp=body[10],filt=body[11],ilace=body[12];
            if(im.w==0||im.h==0){ seterr(err,elen,"image is %ux%u",im.w,im.h); goto fail; }
            if(im.w>PNG_MAX_DIM||im.h>PNG_MAX_DIM){ seterr(err,elen,"image %ux%u exceeds the 2^31-1 limit",im.w,im.h); goto fail; }
            if(comp!=0){ seterr(err,elen,"compression method %u, only 0 is defined",comp); goto fail; }
            if(filt!=0){ seterr(err,elen,"filter method %u, only 0 is defined",filt); goto fail; }
            if(ilace==1){ seterr(err,elen,"Adam7 interlaced PNGs are not supported"); goto fail; }
            if(ilace!=0){ seterr(err,elen,"interlace method %u is not defined",ilace); goto fail; }
            im.nchan=chan_count(im.ctype);
            if(!im.nchan){ seterr(err,elen,"colour type %u is not defined",im.ctype); goto fail; }
            if(!depth_ok(im.ctype,im.depth)){
                seterr(err,elen,"bit depth %u is not valid for colour type %u",im.depth,im.ctype); goto fail;
            }
            if(!sz_mul(im.w,im.h,&npix)||!sz_mul(npix,4u,&rgba_sz)||
               !sz_mul(im.w,im.nchan,&tmp)||!sz_mul(tmp,im.depth,&bits)){
                seterr(err,elen,"image %ux%u is too large to address",im.w,im.h); goto fail;
            }
            im.rowbytes=(bits+7u)/8u;
            if(!sz_add(im.rowbytes,1u,&im.stride)||!sz_mul(im.stride,im.h,&raw_sz)){
                seterr(err,elen,"image %ux%u is too large to address",im.w,im.h); goto fail;
            }
            have_ihdr=true;
        }else if(is_type(type,"PLTE")){
            if(have_idat){ seterr(err,elen,"PLTE appears after IDAT"); goto fail; }
            if(im.plte){ seterr(err,elen,"duplicate PLTE"); goto fail; }
            if(im.ctype==0||im.ctype==4){ seterr(err,elen,"PLTE is not allowed with colour type %u",im.ctype); goto fail; }
            if(clen==0||clen%3u){ seterr(err,elen,"PLTE is %u bytes, not a non-zero multiple of 3",clen); goto fail; }
            if(clen/3u>256u){ seterr(err,elen,"PLTE has %u entries, the maximum is 256",clen/3u); goto fail; }
            im.plte=body; im.plte_n=clen/3u;
        }else if(is_type(type,"tRNS")){
            if(have_idat){ seterr(err,elen,"tRNS appears after IDAT"); goto fail; }
            if(im.trns||im.has_key){ seterr(err,elen,"duplicate tRNS"); goto fail; }
            if(im.ctype==0){
                if(clen!=2){ seterr(err,elen,"tRNS is %u bytes, expected 2 for colour type 0",clen); goto fail; }
                im.key[0]=rd_be16(body); im.has_key=true;
            }else if(im.ctype==2){
                if(clen!=6){ seterr(err,elen,"tRNS is %u bytes, expected 6 for colour type 2",clen); goto fail; }
                im.key[0]=rd_be16(body); im.key[1]=rd_be16(body+2); im.key[2]=rd_be16(body+4);
                im.has_key=true;
            }else if(im.ctype==3){
                if(!im.plte){ seterr(err,elen,"tRNS appears before PLTE"); goto fail; }
                if(clen>im.plte_n){
                    seterr(err,elen,"tRNS has %u entries, PLTE has %zu",clen,im.plte_n); goto fail;
                }
                im.trns=body; im.trns_n=clen;
            }else{
                seterr(err,elen,"tRNS is not allowed with colour type %u",im.ctype); goto fail;
            }
        }else if(is_type(type,"IDAT")){
            if(idat_closed){ seterr(err,elen,"IDAT chunks are not consecutive"); goto fail; }
            if(im.ctype==3&&!im.plte){ seterr(err,elen,"colour type 3 requires a PLTE chunk"); goto fail; }
            if(clen&&!buf_append(&idat,&idat_n,&idat_cap,body,clen)){
                seterr(err,elen,"out of memory joining IDAT chunks"); goto fail;
            }
            have_idat=true;
        }else if(is_type(type,"IEND")){
            if(clen!=0){ seterr(err,elen,"IEND is %u bytes, expected 0",clen); goto fail; }
            have_iend=true;
            break;
        }else if(!(type[0]&0x20u)){
            // bit 5 of the first byte clear means critical: skipping it would
            // change the pixels, so an unknown one has to be fatal
            seterr(err,elen,"unknown critical chunk '%s'",name); goto fail;
        }
        off+=(size_t)clen+12u;                                  // length + type + data + crc
    }

    if(!have_iend){ seterr(err,elen,"missing IEND"); goto fail; }
    if(!have_idat||idat_n==0){ seterr(err,elen,"no IDAT data"); goto fail; }

    raw=malloc(raw_sz);
    if(!raw){ seterr(err,elen,"out of memory: %zu bytes of scanlines",raw_sz); goto fail; }
    sub[0]='\0';
    if(!inflate_zlib(idat,idat_n,raw,raw_sz,&got,sub,sizeof sub)){
        seterr(err,elen,"IDAT: %s",sub[0]?sub:"malformed zlib stream");
        goto fail;
    }
    if(got!=raw_sz){
        seterr(err,elen,"IDAT expanded to %zu bytes, expected %zu",got,raw_sz); goto fail;
    }

    // filtering works on bytes, so sub-byte depths all use a distance of 1
    bpp=(size_t)(im.nchan*im.depth)/8u;
    if(bpp<1)bpp=1;
    if(!unfilter(raw,&im,bpp,err,elen))goto fail;

    rgba=malloc(rgba_sz);
    if(!rgba){ seterr(err,elen,"out of memory: %zu bytes of pixels",rgba_sz); goto fail; }
    if(!expand(&im,raw,rgba,err,elen))goto fail;

    free(raw); free(idat);
    out->width=im.w; out->height=im.h; out->rgba=rgba;
    return true;

fail:
    free(rgba); free(raw); free(idat);
    out->width=0; out->height=0; out->rgba=NULL;
    return false;
}

void png_free(PngImage* img){
    if(!img)return;
    free(img->rgba);
    img->rgba=NULL; img->width=0; img->height=0;
}

// ---- sampling ------------------------------------------------------------

static double wrap_unit(double t,PngWrap w){
    switch(w){
    case PNG_WRAP_CLAMP: return t<0.0?0.0:(t>1.0?1.0:t);
    case PNG_WRAP_MIRROR:{ double m=fmod(fabs(t),2.0); return m>1.0?2.0-m:m; }
    default:{ double f=t-floor(t); return (f<0.0||f>=1.0)?0.0:f; }
    }
}

// the four bilinear taps straddle the edge, so the mode has to apply again at
// texel granularity or a repeating texture seams on its last column
static int64_t wrap_idx(int64_t i,int64_t n,PngWrap w){
    switch(w){
    case PNG_WRAP_CLAMP: return i<0?0:(i>=n?n-1:i);
    case PNG_WRAP_MIRROR:{ int64_t q=2*n,m=i%q; if(m<0)m+=q; return m<n?m:q-1-m; }
    default:{ int64_t m=i%n; if(m<0)m+=n; return m; }
    }
}

uint32_t png_sample(const PngImage* img,double u,double v,PngWrap wrap,uint32_t fallback){
    if(!img||!img->rgba||img->width==0||img->height==0)return fallback;
    if(!isfinite(u))u=0.0;
    if(!isfinite(v))v=0.0;

    int64_t w=(int64_t)img->width,h=(int64_t)img->height;
    // -0.5 puts a texel's colour at its centre; without it the image shifts half
    // a texel and the last row is never reached
    double fx=wrap_unit(u,wrap)*(double)w-0.5;
    double fy=wrap_unit(v,wrap)*(double)h-0.5;      // v runs downward, same as row order: no flip
    double x0d=floor(fx),y0d=floor(fy);
    double tx=fx-x0d,ty=fy-y0d;

    int64_t xa=wrap_idx((int64_t)x0d,w,wrap),xb=wrap_idx((int64_t)x0d+1,w,wrap);
    int64_t ya=wrap_idx((int64_t)y0d,h,wrap),yb=wrap_idx((int64_t)y0d+1,h,wrap);
    size_t rw=(size_t)w;
    const uint8_t* t00=img->rgba+((size_t)ya*rw+(size_t)xa)*4u;
    const uint8_t* t10=img->rgba+((size_t)ya*rw+(size_t)xb)*4u;
    const uint8_t* t01=img->rgba+((size_t)yb*rw+(size_t)xa)*4u;
    const uint8_t* t11=img->rgba+((size_t)yb*rw+(size_t)xb)*4u;

    uint32_t ch[4];
    for(int c=0;c<4;c++){
        double a=(double)t00[c]+((double)t10[c]-(double)t00[c])*tx;
        double b=(double)t01[c]+((double)t11[c]-(double)t01[c])*tx;
        double r=a+(b-a)*ty+0.5;
        ch[c]=(uint32_t)(r<0.0?0.0:(r>255.0?255.0:r));
    }
    return (ch[3]<<24)|(ch[0]<<16)|(ch[1]<<8)|ch[2];
}

#include "jpeg.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Baseline JPEG (ITU T.81 sequential DCT, Huffman, 8-bit). The layout follows
// the spec's own decomposition: markers -> tables -> a scan of MCUs -> per-block
// Huffman + dequantise + IDCT into one plane per component -> upsample and
// colour-convert into RGBA.
//
// The file is untrusted. Every length, index and sampling factor read out of it
// is checked before it sizes an allocation or addresses a plane, and the entropy
// decoder treats a truncated stream as a run of zero bits rather than reading
// past the end, so a chopped-off file yields a grey block instead of a crash.

#define JPEG_MAX_COMPONENTS 4     // SOF can name up to 255; we support 1 and 3
#define FAST_BITS 9               // Huffman lookahead width

static void set_err(char* err,size_t n,const char* msg){
    if(err&&n)snprintf(err,n,"%s",msg);
}

// zigzag[k] is the natural (row-major) position of the k'th coefficient in the
// order the entropy stream delivers them.
static const uint8_t zigzag[64]={
     0, 1, 8,16, 9, 2, 3,10,
    17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,
    27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,
    29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,
    53,60,61,54,47,55,62,63
};

// ---- huffman -------------------------------------------------------------

typedef struct HuffTable {
    uint8_t  vals[256];
    int32_t  mincode[17];
    int32_t  maxcode[17];   // -1 marks "no code of this length"
    int32_t  valptr[17];
    // codes of FAST_BITS or fewer resolve without the length loop, which is the
    // common case: a typical table puts most symbols at 2..8 bits.
    uint8_t  fast_len[1<<FAST_BITS];   // 0 = miss, fall back to the slow path
    uint8_t  fast_sym[1<<FAST_BITS];
    bool     present;
} HuffTable;

static bool huff_build(HuffTable* h,const uint8_t counts[16],const uint8_t* vals,size_t nvals){
    memset(h->fast_len,0,sizeof h->fast_len);
    memcpy(h->vals,vals,nvals);

    int32_t code=0;
    int32_t k=0;
    uint16_t codes[256];
    uint8_t  lens[256];
    for(int l=1;l<=16;l++){
        h->valptr[l]=k;
        h->mincode[l]=code;
        for(int i=0;i<counts[l-1];i++){
            // a canonical code must stay inside its own length; overflowing here
            // means the counts describe an over-full tree
            if(code>=(1<<l))return false;
            codes[k]=(uint16_t)code;
            lens[k]=(uint8_t)l;
            code++;k++;
        }
        h->maxcode[l]=counts[l-1]?code-1:-1;
        code<<=1;
    }
    if((size_t)k!=nvals)return false;

    for(int32_t i=0;i<k;i++){
        if(lens[i]>FAST_BITS)continue;
        // every longer word with this prefix maps to the same symbol
        int shift=FAST_BITS-lens[i];
        uint32_t base=(uint32_t)codes[i]<<shift;
        for(uint32_t j=0;j<(1u<<shift);j++){
            h->fast_len[base+j]=lens[i];
            h->fast_sym[base+j]=h->vals[i];
        }
    }
    h->present=true;
    return true;
}

// ---- entropy-coded bit stream --------------------------------------------

typedef struct BitReader {
    const uint8_t* p;
    const uint8_t* end;
    uint32_t buf;    // `cnt` valid bits, oldest at position cnt-1
    int      cnt;
    bool     stopped;   // hit a marker or the end: keep feeding zero bits
} BitReader;

// 0xFF is the marker escape: 0xFF 0x00 is a literal 0xFF byte, anything else
// ends the entropy segment. Stopping (rather than consuming the marker) leaves
// it in place for the restart handler and for the caller's marker walk.
static uint8_t br_byte(BitReader* b){
    if(b->stopped)return 0;
    if(b->p>=b->end){ b->stopped=true; return 0; }
    uint8_t c=*b->p++;
    if(c==0xFF){
        if(b->p>=b->end){ b->stopped=true; return 0; }
        if(*b->p==0x00){ b->p++; return 0xFF; }
        b->p--;                 // leave the 0xFF for whoever reads markers next
        b->stopped=true;
        return 0;
    }
    return c;
}

static void br_fill(BitReader* b,int n){
    while(b->cnt<n){
        b->buf=(b->buf<<8)|br_byte(b);
        b->cnt+=8;
    }
}

static uint32_t br_peek(BitReader* b,int n){
    br_fill(b,n);
    return (b->buf>>(b->cnt-n))&((1u<<n)-1u);
}

static void br_drop(BitReader* b,int n){
    b->cnt-=n;
    b->buf&=(b->cnt>=32)?0xFFFFFFFFu:((1u<<b->cnt)-1u);   // keep buf from growing
}

static uint32_t br_get(BitReader* b,int n){
    if(n<=0)return 0;
    uint32_t v=br_peek(b,n);
    br_drop(b,n);
    return v;
}

// T.81 EXTEND: an s-bit magnitude whose top bit is clear is the negative half of
// the range, so it maps to -(2^s - 1) .. -(2^(s-1)).
static int32_t extend(uint32_t v,int s){
    return (int32_t)v < (1<<(s-1)) ? (int32_t)v-(1<<s)+1 : (int32_t)v;
}

static int huff_decode(BitReader* b,const HuffTable* h){
    uint32_t look=br_peek(b,FAST_BITS);
    if(h->fast_len[look]){
        br_drop(b,h->fast_len[look]);
        return h->fast_sym[look];
    }
    // slow path: grow the code one bit at a time until it falls inside a length
    int32_t code=0;
    for(int l=1;l<=16;l++){
        code=(code<<1)|(int32_t)br_get(b,1);
        if(h->maxcode[l]>=0&&code<=h->maxcode[l]&&code>=h->mincode[l])
            return h->vals[h->valptr[l]+code-h->mincode[l]];
    }
    return -1;
}

// Resynchronise at a restart marker: the encoder pads to a byte boundary, emits
// RSTn, and resets the DC predictors.
static bool br_restart(BitReader* b){
    b->cnt=0;b->buf=0;
    while(b->p+1<b->end){
        if(b->p[0]==0xFF&&b->p[1]!=0x00){
            uint8_t m=b->p[1];
            if(m>=0xD0&&m<=0xD7){ b->p+=2; b->stopped=false; return true; }
            return false;    // a real marker: the scan is over
        }
        b->p++;
    }
    return false;
}

// ---- inverse DCT ---------------------------------------------------------

// Loeffler-Ligtenberg-Moschytz butterfly, fixed point. Constants are the AAN
// cosine products scaled by 4096; both passes share the kernel and differ only
// in the rounding shift applied afterwards.
#define F2F(x) ((int32_t)((x)*4096.0+0.5))

typedef struct Idct1D { int32_t x0,x1,x2,x3,t0,t1,t2,t3; } Idct1D;

static Idct1D idct_1d(int32_t s0,int32_t s1,int32_t s2,int32_t s3,
                      int32_t s4,int32_t s5,int32_t s6,int32_t s7){
    Idct1D r;
    int32_t p1,p2,p3,p4,p5,t0,t1,t2,t3;

    // even part: coefficients 0,2,4,6
    p2=s2; p3=s6;
    p1=(p2+p3)*F2F(0.5411961);
    t2=p1+p3*F2F(-1.847759065);
    t3=p1+p2*F2F( 0.765366865);
    p2=s0; p3=s4;
    t0=(p2+p3)*4096;
    t1=(p2-p3)*4096;
    r.x0=t0+t3; r.x3=t0-t3;
    r.x1=t1+t2; r.x2=t1-t2;

    // odd part: coefficients 1,3,5,7
    t0=s7; t1=s5; t2=s3; t3=s1;
    p3=t0+t2; p4=t1+t3;
    p1=t0+t3; p2=t1+t2;
    p5=(p3+p4)*F2F(1.175875602);
    t0=t0*F2F(0.298631336);
    t1=t1*F2F(2.053119869);
    t2=t2*F2F(3.072711026);
    t3=t3*F2F(1.501321110);
    p1=p5+p1*F2F(-0.899976223);
    p2=p5+p2*F2F(-2.562915447);
    p3=p3*F2F(-1.961570560);
    p4=p4*F2F(-0.390180644);
    r.t3=t3+p1+p4;
    r.t2=t2+p2+p3;
    r.t1=t1+p2+p4;
    r.t0=t0+p1+p3;
    return r;
}

static uint8_t clamp_u8(int32_t v){
    if((uint32_t)v>255u)return v<0?0:255;
    return (uint8_t)v;
}

// blk is dequantised coefficients in natural order; writes an 8x8 tile of
// samples at `dst` with row pitch `stride`.
static void idct_block(const int32_t blk[64],uint8_t* dst,size_t stride){
    int32_t mid[64];

    for(int j=0;j<8;j++){
        // an all-zero AC column is the common case in flat areas; the DC term
        // alone already gives the exact answer
        if(!(blk[8+j]|blk[16+j]|blk[24+j]|blk[32+j]|blk[40+j]|blk[48+j]|blk[56+j])){
            int32_t dc=blk[j]*4;
            for(int i=0;i<8;i++)mid[i*8+j]=dc;
            continue;
        }
        Idct1D r=idct_1d(blk[0+j],blk[8+j],blk[16+j],blk[24+j],
                         blk[32+j],blk[40+j],blk[48+j],blk[56+j]);
        r.x0+=512; r.x1+=512; r.x2+=512; r.x3+=512;     // round for the >>10
        mid[0*8+j]=(r.x0+r.t3)>>10;
        mid[7*8+j]=(r.x0-r.t3)>>10;
        mid[1*8+j]=(r.x1+r.t2)>>10;
        mid[6*8+j]=(r.x1-r.t2)>>10;
        mid[2*8+j]=(r.x2+r.t1)>>10;
        mid[5*8+j]=(r.x2-r.t1)>>10;
        mid[3*8+j]=(r.x3+r.t0)>>10;
        mid[4*8+j]=(r.x3-r.t0)>>10;
    }

    for(int i=0;i<8;i++){
        const int32_t* s=mid+i*8;
        Idct1D r=idct_1d(s[0],s[1],s[2],s[3],s[4],s[5],s[6],s[7]);
        // +128 is the level shift back from signed samples, folded into the
        // rounding constant for the >>17
        int32_t bias=65536+(128<<17);
        r.x0+=bias; r.x1+=bias; r.x2+=bias; r.x3+=bias;
        uint8_t* o=dst+(size_t)i*stride;
        o[0]=clamp_u8((r.x0+r.t3)>>17);
        o[7]=clamp_u8((r.x0-r.t3)>>17);
        o[1]=clamp_u8((r.x1+r.t2)>>17);
        o[6]=clamp_u8((r.x1-r.t2)>>17);
        o[2]=clamp_u8((r.x2+r.t1)>>17);
        o[5]=clamp_u8((r.x2-r.t1)>>17);
        o[3]=clamp_u8((r.x3+r.t0)>>17);
        o[4]=clamp_u8((r.x3-r.t0)>>17);
    }
}

// ---- decoder state -------------------------------------------------------

typedef struct Component {
    int id;
    int h,v;            // sampling factors
    int tq;             // quantisation table selector
    int td,ta;          // DC / AC Huffman selectors, set by SOS
    uint8_t* plane;     // (blocks_w*8) x (blocks_h*8), padded to whole MCUs
    size_t   pw,ph;
    int32_t  dc_pred;
} Component;

typedef struct Decoder {
    const uint8_t* d;
    size_t len;
    uint16_t qt[4][64];      // natural order
    bool     qt_present[4];
    HuffTable hdc[4],hac[4];
    Component comp[JPEG_MAX_COMPONENTS];
    int ncomp;
    uint32_t width,height;
    int hmax,vmax;
    size_t mcux,mcuy;
    int restart_interval;
    bool progressive;
    int adobe_transform;     // -1 when there is no APP14 Adobe marker
} Decoder;

static void decoder_free_planes(Decoder* dc){
    for(int i=0;i<JPEG_MAX_COMPONENTS;i++){ free(dc->comp[i].plane); dc->comp[i].plane=NULL; }
}

static bool mul_ok(size_t a,size_t b,size_t* r){
    if(a&&b>SIZE_MAX/a)return false;
    *r=a*b;
    return true;
}

// ---- marker segments -----------------------------------------------------

static bool read_dqt(Decoder* dc,const uint8_t* p,size_t n,char* err,size_t errn){
    while(n>0){
        uint8_t pq=p[0]>>4,tq=p[0]&15;
        p++;n--;
        if(tq>3){ set_err(err,errn,"dqt: table id out of range"); return false; }
        size_t need=pq?128:64;
        if(pq>1||n<need){ set_err(err,errn,"dqt: truncated or bad precision"); return false; }
        for(int k=0;k<64;k++){
            uint16_t v=pq?(uint16_t)((p[2*k]<<8)|p[2*k+1]):p[k];
            if(v==0){ set_err(err,errn,"dqt: zero quantiser"); return false; }
            dc->qt[tq][zigzag[k]]=v;
        }
        dc->qt_present[tq]=true;
        p+=need;n-=need;
    }
    return true;
}

static bool read_dht(Decoder* dc,const uint8_t* p,size_t n,char* err,size_t errn){
    while(n>0){
        if(n<17){ set_err(err,errn,"dht: truncated header"); return false; }
        uint8_t tc=p[0]>>4,th=p[0]&15;
        if(tc>1||th>3){ set_err(err,errn,"dht: table id out of range"); return false; }
        const uint8_t* counts=p+1;
        size_t total=0;
        for(int i=0;i<16;i++)total+=counts[i];
        if(total>256||n<17+total){ set_err(err,errn,"dht: truncated symbol list"); return false; }
        HuffTable* h=tc?&dc->hac[th]:&dc->hdc[th];
        if(!huff_build(h,counts,p+17,total)){ set_err(err,errn,"dht: over-full code table"); return false; }
        p+=17+total;n-=17+total;
    }
    return true;
}

static bool read_sof(Decoder* dc,const uint8_t* p,size_t n,char* err,size_t errn){
    if(n<6){ set_err(err,errn,"sof: truncated"); return false; }
    if(p[0]!=8){ set_err(err,errn,"sof: only 8-bit samples are supported"); return false; }
    dc->height=(uint32_t)(p[1]<<8|p[2]);
    dc->width =(uint32_t)(p[3]<<8|p[4]);
    dc->ncomp =p[5];
    if(dc->width==0||dc->height==0){ set_err(err,errn,"sof: zero dimension"); return false; }
    if(dc->ncomp!=1&&dc->ncomp!=3){ set_err(err,errn,"sof: only 1- and 3-component images are supported"); return false; }
    if(n<6+(size_t)dc->ncomp*3){ set_err(err,errn,"sof: truncated component list"); return false; }

    dc->hmax=1;dc->vmax=1;
    for(int i=0;i<dc->ncomp;i++){
        const uint8_t* q=p+6+i*3;
        Component* c=&dc->comp[i];
        c->id=q[0];
        c->h=q[1]>>4;c->v=q[1]&15;
        c->tq=q[2];
        // 3 and 4 are legal per the spec but no encoder emits them, and they are
        // the values that turn an MCU into an awkward shape; 1 and 2 cover 4:4:4,
        // 4:2:2, 4:4:0 and 4:2:0
        if(c->h<1||c->h>4||c->v<1||c->v>4){ set_err(err,errn,"sof: bad sampling factor"); return false; }
        if(c->tq>3){ set_err(err,errn,"sof: bad quantisation selector"); return false; }
        if(c->h>dc->hmax)dc->hmax=c->h;
        if(c->v>dc->vmax)dc->vmax=c->v;
    }

    dc->mcux=((size_t)dc->width +(size_t)dc->hmax*8-1)/((size_t)dc->hmax*8);
    dc->mcuy=((size_t)dc->height+(size_t)dc->vmax*8-1)/((size_t)dc->vmax*8);

    for(int i=0;i<dc->ncomp;i++){
        Component* c=&dc->comp[i];
        size_t pw,ph,sz;
        if(!mul_ok(dc->mcux,(size_t)c->h*8,&pw)||!mul_ok(dc->mcuy,(size_t)c->v*8,&ph)
         ||!mul_ok(pw,ph,&sz)){ set_err(err,errn,"sof: image dimensions overflow"); return false; }
        c->plane=malloc(sz);
        if(!c->plane){ set_err(err,errn,"out of memory"); return false; }
        // a scan that stops early leaves whole MCUs untouched; mid-grey is a
        // less alarming placeholder than whatever malloc handed back
        memset(c->plane,128,sz);
        c->pw=pw;c->ph=ph;
    }
    return true;
}

// ---- the scan ------------------------------------------------------------

static bool decode_block(Decoder* dc,BitReader* br,Component* c,int32_t blk[64],char* err,size_t errn){
    const HuffTable* hd=&dc->hdc[c->td];
    const HuffTable* ha=&dc->hac[c->ta];
    const uint16_t* q=dc->qt[c->tq];
    memset(blk,0,64*sizeof *blk);

    int t=huff_decode(br,hd);
    if(t<0||t>15){ set_err(err,errn,"scan: bad dc code"); return false; }
    int32_t diff=t?extend(br_get(br,t),t):0;
    c->dc_pred+=diff;
    blk[0]=c->dc_pred*(int32_t)q[0];

    for(int k=1;k<64;){
        int rs=huff_decode(br,ha);
        if(rs<0){ set_err(err,errn,"scan: bad ac code"); return false; }
        int s=rs&15,r=rs>>4;
        if(s==0){
            if(r!=15)break;      // EOB
            k+=16;               // ZRL: sixteen zeroes, no coefficient
            continue;
        }
        k+=r;
        if(k>63){ set_err(err,errn,"scan: run past end of block"); return false; }
        // both the coefficient and its quantiser live at the natural position
        // that zigzag index k maps to
        blk[zigzag[k]]=extend(br_get(br,s),s)*(int32_t)q[zigzag[k]];
        k++;
    }
    return true;
}

static bool read_sos(Decoder* dc,const uint8_t* p,size_t n,
                     const uint8_t* entropy,const uint8_t* end,
                     const uint8_t** resume,char* err,size_t errn){
    if(n<1){ set_err(err,errn,"sos: truncated"); return false; }
    int ns=p[0];
    if(ns!=dc->ncomp){ set_err(err,errn,"sos: non-interleaved multi-scan images are not supported"); return false; }
    if(n<1+(size_t)ns*2+3){ set_err(err,errn,"sos: truncated component list"); return false; }

    for(int i=0;i<ns;i++){
        int id=p[1+i*2],tt=p[2+i*2];
        Component* c=NULL;
        for(int j=0;j<dc->ncomp;j++)if(dc->comp[j].id==id)c=&dc->comp[j];
        if(!c){ set_err(err,errn,"sos: scan names an unknown component"); return false; }
        c->td=tt>>4;c->ta=tt&15;
        if(c->td>3||c->ta>3){ set_err(err,errn,"sos: bad huffman selector"); return false; }
        if(!dc->hdc[c->td].present||!dc->hac[c->ta].present){
            set_err(err,errn,"sos: scan uses a huffman table that was never defined"); return false;
        }
        if(!dc->qt_present[c->tq]){
            set_err(err,errn,"sos: scan uses a quantisation table that was never defined"); return false;
        }
    }
    const uint8_t* sp=p+1+(size_t)ns*2;
    if(sp[0]!=0||sp[1]!=63||sp[2]!=0){
        set_err(err,errn,"sos: spectral selection or successive approximation is progressive-only");
        return false;
    }

    BitReader br={.p=entropy,.end=end};
    int32_t blk[64];
    for(int i=0;i<dc->ncomp;i++)dc->comp[i].dc_pred=0;

    size_t since_restart=0;
    for(size_t my=0;my<dc->mcuy;my++){
        for(size_t mx=0;mx<dc->mcux;mx++){
            for(int ci=0;ci<dc->ncomp;ci++){
                Component* c=&dc->comp[ci];
                for(int by=0;by<c->v;by++){
                    for(int bx=0;bx<c->h;bx++){
                        if(!decode_block(dc,&br,c,blk,err,errn))return false;
                        size_t px=(mx*(size_t)c->h+(size_t)bx)*8;
                        size_t py=(my*(size_t)c->v+(size_t)by)*8;
                        idct_block(blk,c->plane+py*c->pw+px,c->pw);
                    }
                }
            }
            if(dc->restart_interval&&++since_restart==(size_t)dc->restart_interval){
                since_restart=0;
                bool last=(my==dc->mcuy-1&&mx==dc->mcux-1);
                // the encoder does not emit RSTn after the final MCU
                if(!last&&!br_restart(&br)){
                    set_err(err,errn,"scan: missing restart marker");
                    return false;
                }
                for(int i=0;i<dc->ncomp;i++)dc->comp[i].dc_pred=0;
            }
        }
    }

    // hand back the first byte the entropy decoder did not consume so the
    // marker walk can continue from there
    *resume=br.p<end?br.p:end;
    return true;
}

// ---- output --------------------------------------------------------------

// Fixed point YCbCr -> RGB, JFIF coefficients, 16 fractional bits.
static void ycbcr_to_rgb(int32_t y,int32_t cb,int32_t cr,uint8_t* out){
    cb-=128;cr-=128;
    int32_t base=(y<<16)+32768;
    out[0]=clamp_u8((base+ 91881*cr)>>16);
    out[1]=clamp_u8((base- 22554*cb- 46802*cr)>>16);
    out[2]=clamp_u8((base+116130*cb)>>16);
}

static bool emit_rgba(Decoder* dc,uint32_t max_dim,JpegImage* out,char* err,size_t errn){
    uint32_t scale=1;
    if(max_dim){
        uint32_t big=dc->width>dc->height?dc->width:dc->height;
        scale=(big+max_dim-1)/max_dim;
        if(scale<1)scale=1;
    }
    uint32_t ow=(dc->width+scale-1)/scale;
    uint32_t oh=(dc->height+scale-1)/scale;
    if(ow==0)ow=1;
    if(oh==0)oh=1;

    size_t npix,nbytes;
    if(!mul_ok(ow,oh,&npix)||!mul_ok(npix,4,&nbytes)){
        set_err(err,errn,"output dimensions overflow");
        return false;
    }
    uint8_t* rgba=malloc(nbytes);
    if(!rgba){ set_err(err,errn,"out of memory"); return false; }

    bool ycc=(dc->ncomp==3);
    // three components are YCbCr unless an Adobe APP14 says otherwise, or the
    // component ids spell 'R','G','B' -- both are how an encoder marks RGB JPEG
    if(dc->ncomp==3&&(dc->adobe_transform==0||
       (dc->comp[0].id=='R'&&dc->comp[1].id=='G'&&dc->comp[2].id=='B')))ycc=false;

    for(uint32_t oy=0;oy<oh;oy++){
        uint32_t y0=oy*scale,y1=y0+scale;
        if(y1>dc->height)y1=dc->height;
        for(uint32_t ox=0;ox<ow;ox++){
            uint32_t x0=ox*scale,x1=x0+scale;
            if(x1>dc->width)x1=dc->width;
            uint32_t acc[3]={0,0,0},n=0;
            for(uint32_t sy=y0;sy<y1;sy++){
                for(uint32_t sx=x0;sx<x1;sx++){
                    uint8_t px[3];
                    if(dc->ncomp==1){
                        const Component* c=&dc->comp[0];
                        uint8_t g=c->plane[(size_t)sy*c->pw+sx];
                        px[0]=px[1]=px[2]=g;
                    }else{
                        int32_t s[3];
                        for(int ci=0;ci<3;ci++){
                            const Component* c=&dc->comp[ci];
                            // nearest-neighbour upsample: chroma is subsampled by
                            // hmax/h horizontally and vmax/v vertically
                            size_t cx=(size_t)sx*(size_t)c->h/(size_t)dc->hmax;
                            size_t cy=(size_t)sy*(size_t)c->v/(size_t)dc->vmax;
                            if(cx>=c->pw)cx=c->pw-1;
                            if(cy>=c->ph)cy=c->ph-1;
                            s[ci]=c->plane[cy*c->pw+cx];
                        }
                        if(ycc)ycbcr_to_rgb(s[0],s[1],s[2],px);
                        else{ px[0]=(uint8_t)s[0];px[1]=(uint8_t)s[1];px[2]=(uint8_t)s[2]; }
                    }
                    acc[0]+=px[0];acc[1]+=px[1];acc[2]+=px[2];n++;
                }
            }
            uint8_t* o=rgba+((size_t)oy*ow+ox)*4;
            if(!n)n=1;
            o[0]=(uint8_t)(acc[0]/n);
            o[1]=(uint8_t)(acc[1]/n);
            o[2]=(uint8_t)(acc[2]/n);
            o[3]=255;              // jpeg has no alpha channel
        }
    }

    out->width=ow;out->height=oh;out->rgba=rgba;
    return true;
}

// ---- driver --------------------------------------------------------------

bool jpeg_is_jpeg(const void* data,size_t len){
    const uint8_t* d=data;
    return d&&len>=2&&d[0]==0xFF&&d[1]==0xD8;
}

void jpeg_free(JpegImage* img){
    if(!img)return;
    free(img->rgba);
    img->rgba=NULL;img->width=0;img->height=0;
}

bool jpeg_decode(const void* data,size_t len,uint32_t max_dim,
                 JpegImage* out,char* err,size_t err_len){
    if(!out)return false;
    *out=(JpegImage){0};
    if(!jpeg_is_jpeg(data,len)){ set_err(err,err_len,"not a jpeg (no SOI marker)"); return false; }

    Decoder* dc=calloc(1,sizeof *dc);
    if(!dc){ set_err(err,err_len,"out of memory"); return false; }
    dc->d=data;dc->len=len;dc->adobe_transform=-1;

    const uint8_t* p=(const uint8_t*)data+2;
    const uint8_t* end=(const uint8_t*)data+len;
    bool have_sof=false,done=false,ok=false;

    while(p<end){
        // markers may be preceded by any number of 0xFF fill bytes
        if(*p!=0xFF){ p++; continue; }
        while(p<end&&*p==0xFF)p++;
        if(p>=end)break;
        uint8_t m=*p++;

        if(m==0xD9){ done=true; break; }                  // EOI
        if(m==0x01||(m>=0xD0&&m<=0xD7))continue;          // TEM / stray RSTn: no payload

        if(p+2>end){ set_err(err,err_len,"truncated segment header"); goto fail; }
        size_t seg=(size_t)((p[0]<<8)|p[1]);
        if(seg<2||p+seg>end){ set_err(err,err_len,"segment length runs past end of file"); goto fail; }
        const uint8_t* body=p+2;
        size_t blen=seg-2;

        switch(m){
        case 0xC0: case 0xC1:                              // SOF0 / SOF1
            if(have_sof){ set_err(err,err_len,"more than one frame header"); goto fail; }
            if(!read_sof(dc,body,blen,err,err_len))goto fail;
            have_sof=true;
            break;
        case 0xC2:
            set_err(err,err_len,"progressive jpeg (SOF2) is not supported");
            goto fail;
        case 0xC3: case 0xC5: case 0xC6: case 0xC7:
        case 0xC9: case 0xCA: case 0xCB:
        case 0xCD: case 0xCE: case 0xCF:
            set_err(err,err_len,"lossless, differential or arithmetic-coded jpeg is not supported");
            goto fail;
        case 0xC4:                                         // DHT
            if(!read_dht(dc,body,blen,err,err_len))goto fail;
            break;
        case 0xDB:                                         // DQT
            if(!read_dqt(dc,body,blen,err,err_len))goto fail;
            break;
        case 0xDD:                                         // DRI
            if(blen<2){ set_err(err,err_len,"dri: truncated"); goto fail; }
            dc->restart_interval=(body[0]<<8)|body[1];
            break;
        case 0xEE:                                         // APP14, Adobe
            if(blen>=12&&memcmp(body,"Adobe",5)==0)dc->adobe_transform=body[11];
            break;
        case 0xDA: {                                       // SOS
            if(!have_sof){ set_err(err,err_len,"scan before frame header"); goto fail; }
            const uint8_t* resume=NULL;
            if(!read_sos(dc,body,blen,body+blen,end,&resume,err,err_len))goto fail;
            p=resume;
            done=true;                                     // baseline has exactly one scan
            break;
        }
        default:
            break;                                         // APPn, COM, anything else
        }
        if(done)break;
        p+=seg;
    }

    if(!have_sof){ set_err(err,err_len,"no frame header"); goto fail; }
    if(!done){ set_err(err,err_len,"no scan"); goto fail; }
    if(!emit_rgba(dc,max_dim,out,err,err_len))goto fail;
    ok=true;

fail:
    decoder_free_planes(dc);
    free(dc);
    if(!ok)*out=(JpegImage){0};
    return ok;
}

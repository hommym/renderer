#include "inflate.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// DEFLATE (RFC 1951) and the zlib wrapper around it (RFC 1950), decompression
// only. The input is a file someone else wrote, so every bit read is checked
// against the bytes actually present and every byte written against dst_cap.
//
// Nothing here allocates. Both buffers belong to the caller, which is what lets
// every error path be a plain `return false` with no cleanup to get wrong.
//
// The bit order is what bites people: DEFLATE fills a byte LSB-first, but a
// Huffman code is read starting from the MOST significant bit of the code. So
// plain integers come out low bit first (drop_bits shifts the accumulator
// right) while codes are built one bit at a time into the bottom of a value
// that is shifted left (huff_decode). Both live in the same stream and the two
// rules are not interchangeable.

#define MAX_BITS 15
#define MAX_SYMS 288    // literal/length alphabet, the largest of the three

typedef struct Inf {
    const uint8_t* src; size_t src_len; size_t pos;   // pos = next byte to buffer
    uint32_t bitbuf; int bitcnt;                      // bitcnt is always < 32
    uint8_t* dst; size_t dst_cap; size_t out;
    char* err; size_t err_len;
} Inf;

static void seterr(char* err,size_t err_len,const char* msg){
    if(err&&err_len)snprintf(err,err_len,"%s",msg);
}

// the byte offset is the only context worth carrying: it is what a hex editor
// needs to see the malformed stream for itself.
static bool fail(Inf* z,const char* msg){
    if(z->err&&z->err_len)snprintf(z->err,z->err_len,"%s at input byte %zu",msg,z->pos);
    return false;
}

static bool failw(Inf* z,const char* what,const char* msg){
    char m[96];
    snprintf(m,sizeof m,"%s %s",what,msg);
    return fail(z,m);
}

// ---- bit reader ----------------------------------------------------------

// n <= 16 everywhere below, so the accumulator never needs more than 24 bits.
static bool need_bits(Inf* z,int n){
    while(z->bitcnt<n){
        if(z->pos>=z->src_len)return false;
        z->bitbuf|=(uint32_t)z->src[z->pos++]<<z->bitcnt;
        z->bitcnt+=8;
    }
    return true;
}

static uint32_t drop_bits(Inf* z,int n){
    uint32_t v=z->bitbuf&((n==0)?0u:((1u<<n)-1u));
    z->bitbuf>>=n;
    z->bitcnt-=n;
    return v;
}

// whole bytes sitting in the accumulator were never really consumed, so give
// them back to pos -- otherwise a stored block starts several bytes too late.
static void align_byte(Inf* z){
    z->pos-=(size_t)(z->bitcnt>>3);
    z->bitcnt=0;
    z->bitbuf=0;
}

// ---- canonical huffman ---------------------------------------------------

// counts-and-symbols rather than a lookup table: the per-bit walk is slower but
// it is the shape RFC 1951 3.2.2 describes, so it is the one that can be read
// against the spec.
typedef struct Huff {
    uint16_t count[MAX_BITS+1];
    uint16_t symbol[MAX_SYMS];
} Huff;

// allow_single permits the one incomplete set the format documents: a table
// with a single one-bit code (and, the same test, one with no codes at all,
// which a block that never emits a distance is entitled to).
static bool huff_build(Huff* h,const uint8_t* lens,unsigned n,bool allow_single,
                       Inf* z,const char* what){
    for(int i=0;i<=MAX_BITS;i++)h->count[i]=0;
    for(unsigned s=0;s<n;s++){
        if(lens[s]>MAX_BITS)return failw(z,what,"code length exceeds 15 bits");
        h->count[lens[s]]++;
    }
    int left=1;
    for(int len=1;len<=MAX_BITS;len++){
        left<<=1;
        left-=(int)h->count[len];
        if(left<0)return failw(z,what,"code set is over-subscribed");
    }
    if(left>0&&!(allow_single&&n==(unsigned)h->count[0]+(unsigned)h->count[1]))
        return failw(z,what,"code set is incomplete");
    uint16_t offs[MAX_BITS+1];
    offs[0]=0;offs[1]=0;
    for(int len=1;len<MAX_BITS;len++)offs[len+1]=(uint16_t)(offs[len]+h->count[len]);
    for(unsigned s=0;s<n;s++)if(lens[s])h->symbol[offs[lens[s]]++]=(uint16_t)s;
    return true;
}

// walks one bit at a time, MSB of the code first. returns -1 on any failure,
// having already written the reason.
static int huff_decode(Inf* z,const Huff* h){
    int code=0,first=0,index=0;
    for(int len=1;len<=MAX_BITS;len++){
        if(!need_bits(z,1)){fail(z,"out of input inside a huffman code");return -1;}
        code|=(int)drop_bits(z,1);
        int count=(int)h->count[len];
        if(code-first<count)return h->symbol[index+(code-first)];
        index+=count;
        first=(first+count)<<1;
        code<<=1;
    }
    fail(z,"invalid huffman code");
    return -1;
}

// ---- rfc 1951 length/distance tables -------------------------------------

static const uint16_t len_base[29]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,
                                    59,67,83,99,115,131,163,195,227,258};
static const uint8_t  len_extra[29]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const uint16_t dist_base[30]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,
                                     769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint8_t  dist_extra[30]={0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

// permutation from 3.2.7: the lengths most likely to be zero are sent last so
// HCLEN can cut them off.
static const uint8_t clc_order[19]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

// ---- block bodies --------------------------------------------------------

static bool blk_stored(Inf* z){
    align_byte(z);
    if(z->src_len-z->pos<4)return fail(z,"truncated stored block header");
    unsigned len =(unsigned)z->src[z->pos]  |((unsigned)z->src[z->pos+1]<<8);
    unsigned nlen=(unsigned)z->src[z->pos+2]|((unsigned)z->src[z->pos+3]<<8);
    z->pos+=4;
    if(nlen!=(len^0xFFFFu))return fail(z,"stored block LEN/NLEN mismatch");
    if(z->src_len-z->pos<len)return fail(z,"truncated stored block body");
    if(z->dst_cap-z->out<len)return fail(z,"stored block runs past the destination buffer");
    if(len){
        memcpy(z->dst+z->out,z->src+z->pos,len);
        z->pos+=len;
        z->out+=len;
    }
    return true;
}

// the shared body of a fixed and a dynamic block: they differ only in where the
// two tables came from.
static bool blk_codes(Inf* z,const Huff* lc,const Huff* dc){
    for(;;){
        int sym=huff_decode(z,lc);
        if(sym<0)return false;
        if(sym<256){
            if(z->out>=z->dst_cap)return fail(z,"literal runs past the destination buffer");
            z->dst[z->out++]=(uint8_t)sym;
            continue;
        }
        if(sym==256)return true;
        sym-=257;
        if(sym>=29)return fail(z,"length code 286 or 287 is not assigned");
        int nb=len_extra[sym];
        if(!need_bits(z,nb))return fail(z,"out of input reading length extra bits");
        unsigned len=(unsigned)len_base[sym]+drop_bits(z,nb);
        int ds=huff_decode(z,dc);
        if(ds<0)return false;
        if(ds>=30)return fail(z,"distance code 30 or 31 is not assigned");
        nb=dist_extra[ds];
        if(!need_bits(z,nb))return fail(z,"out of input reading distance extra bits");
        size_t dist=(size_t)dist_base[ds]+drop_bits(z,nb);
        if(dist>z->out)return fail(z,"back-reference points before the start of the output");
        if(z->dst_cap-z->out<len)return fail(z,"match runs past the destination buffer");
        // byte at a time, forwards: a match may overlap the write position, and
        // distance 1 legitimately means "repeat the last byte len times".
        size_t from=z->out-dist;
        for(unsigned i=0;i<len;i++)z->dst[z->out++]=z->dst[from++];
    }
}

static bool blk_fixed(Inf* z){
    uint8_t lens[MAX_SYMS];
    unsigned i=0;
    for(;i<144;i++)lens[i]=8;
    for(;i<256;i++)lens[i]=9;
    for(;i<280;i++)lens[i]=7;
    for(;i<288;i++)lens[i]=8;
    Huff lc,dc;
    if(!huff_build(&lc,lens,288,false,z,"fixed literal/length"))return false;
    // 32 five-bit distance codes; 30 and 31 exist in the table but blk_codes
    // rejects them if they ever show up.
    for(i=0;i<32;i++)lens[i]=5;
    if(!huff_build(&dc,lens,32,false,z,"fixed distance"))return false;
    return blk_codes(z,&lc,&dc);
}

static bool blk_dynamic(Inf* z){
    if(!need_bits(z,14))return fail(z,"out of input reading dynamic block header");
    unsigned hlit =drop_bits(z,5)+257;
    unsigned hdist=drop_bits(z,5)+1;
    unsigned hclen=drop_bits(z,4)+4;
    if(hlit>286)return fail(z,"HLIT claims more than 286 literal/length codes");
    if(hdist>30)return fail(z,"HDIST claims more than 30 distance codes");

    uint8_t lens[MAX_SYMS+32]={0};                 // hlit lengths then hdist, contiguous
    for(unsigned i=0;i<hclen;i++){
        if(!need_bits(z,3))return fail(z,"out of input reading code lengths");
        lens[clc_order[i]]=(uint8_t)drop_bits(z,3);
    }
    for(unsigned i=hclen;i<19;i++)lens[clc_order[i]]=0;
    Huff clen;
    if(!huff_build(&clen,lens,19,false,z,"code length"))return false;

    unsigned total=hlit+hdist,i=0;
    while(i<total){
        int sym=huff_decode(z,&clen);
        if(sym<0)return false;
        unsigned rep;
        uint8_t val=0;
        if(sym<16){lens[i++]=(uint8_t)sym;continue;}
        if(sym==16){
            if(i==0)return fail(z,"repeat code 16 with no previous code length");
            val=lens[i-1];
            if(!need_bits(z,2))return fail(z,"out of input reading a repeat count");
            rep=3+drop_bits(z,2);
        }else if(sym==17){
            if(!need_bits(z,3))return fail(z,"out of input reading a repeat count");
            rep=3+drop_bits(z,3);
        }else{
            if(!need_bits(z,7))return fail(z,"out of input reading a repeat count");
            rep=11+drop_bits(z,7);
        }
        if(rep>total-i)return fail(z,"code length repeat runs past the end of the alphabet");
        while(rep--)lens[i++]=val;
    }
    // without it the block can only end by running the input out
    if(lens[256]==0)return fail(z,"no end-of-block code in the literal/length alphabet");

    Huff lc,dc;
    if(!huff_build(&lc,lens,hlit,true,z,"literal/length"))return false;
    if(!huff_build(&dc,lens+hlit,hdist,true,z,"distance"))return false;
    return blk_codes(z,&lc,&dc);
}

static bool inflate_core(Inf* z){
    unsigned last;
    do{
        if(!need_bits(z,3))return fail(z,"out of input reading a block header");
        last=drop_bits(z,1);
        unsigned type=drop_bits(z,2);
        bool ok;
        if(type==0)ok=blk_stored(z);
        else if(type==1)ok=blk_fixed(z);
        else if(type==2)ok=blk_dynamic(z);
        else return fail(z,"block type 3 is reserved");
        if(!ok)return false;
    }while(!last);
    return true;
}

static bool inf_setup(Inf* z,const void* src,size_t src_len,void* dst,size_t dst_cap,
                      char* err,size_t err_len){
    *z=(Inf){.src=src,.src_len=src_len,.dst=dst,.dst_cap=dst_cap,.err=err,.err_len=err_len};
    if(!src&&src_len){seterr(err,err_len,"null source with a non-zero length");return false;}
    if(!dst&&dst_cap){seterr(err,err_len,"null destination with a non-zero capacity");return false;}
    return true;
}

bool inflate_raw(const void* src,size_t src_len,void* dst,size_t dst_cap,size_t* out_len,
                 char* err,size_t err_len){
    if(out_len)*out_len=0;
    if(err&&err_len)err[0]='\0';
    Inf z;
    if(!inf_setup(&z,src,src_len,dst,dst_cap,err,err_len))return false;
    if(!inflate_core(&z))return false;
    if(out_len)*out_len=z.out;
    return true;
}

// ---- zlib wrapper --------------------------------------------------------

static uint32_t adler32(const uint8_t* d,size_t n){
    uint32_t a=1,b=0;
    while(n){
        // 5552 is the longest run of 0xFF bytes before b can overflow 32 bits,
        // so the two modulos only have to happen once per block
        size_t k=n<5552?n:5552;
        n-=k;
        while(k--){a+=*d++;b+=a;}
        a%=65521;b%=65521;
    }
    return (b<<16)|a;
}

bool inflate_zlib(const void* src,size_t src_len,void* dst,size_t dst_cap,size_t* out_len,
                  char* err,size_t err_len){
    if(out_len)*out_len=0;
    if(err&&err_len)err[0]='\0';
    Inf z;
    if(!inf_setup(&z,src,src_len,dst,dst_cap,err,err_len))return false;
    if(src_len<6){seterr(err,err_len,"zlib stream shorter than its own framing");return false;}
    const uint8_t* s=z.src;
    unsigned cmf=s[0],flg=s[1];
    if((cmf&0x0Fu)!=8){seterr(err,err_len,"zlib CM is not 8 (deflate)");return false;}
    if((cmf>>4)>7){seterr(err,err_len,"zlib CINFO claims a window larger than 32K");return false;}
    if(((cmf<<8)|flg)%31u){seterr(err,err_len,"zlib header check bits are wrong");return false;}
    // a preset dictionary means the first bytes were compressed against data we
    // were never given, so the stream is undecodable here rather than malformed
    if(flg&0x20u){seterr(err,err_len,"zlib preset dictionary is not supported");return false;}
    z.pos=2;
    if(!inflate_core(&z))return false;
    align_byte(&z);
    if(z.src_len-z.pos<4)return fail(&z,"zlib stream ends before its adler32");
    uint32_t want=((uint32_t)z.src[z.pos]<<24)|((uint32_t)z.src[z.pos+1]<<16)|
                  ((uint32_t)z.src[z.pos+2]<<8)|(uint32_t)z.src[z.pos+3];
    z.pos+=4;
    if(want!=adler32(z.dst,z.out))return fail(&z,"adler32 mismatch: the data is corrupt");
    if(out_len)*out_len=z.out;
    return true;
}

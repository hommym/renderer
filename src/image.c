#include "image.h"
#include "png.h"
#include "jpeg.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Format sniffing plus the one conversion both decoders need: their byte-per-
// channel RGBA into the packed 0xAARRGGBB word the renderer samples.

static const uint8_t PNG_MAGIC[8]={0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};

static void set_err(char* err,size_t n,const char* msg){
    if(err&&n)snprintf(err,n,"%s",msg);
}

static bool mul_ok(size_t a,size_t b,size_t* r){
    if(a&&b>SIZE_MAX/a)return false;
    *r=a*b;
    return true;
}

// Takes ownership of rgba either way: on success it is freed after packing, on
// failure it is freed before returning.
static bool pack_rgba(uint8_t* rgba,size_t w,size_t h,Image* out,char* err,size_t errn){
    size_t npix,nbytes;
    if(!mul_ok(w,h,&npix)||!mul_ok(npix,sizeof(uint32_t),&nbytes)||npix==0){
        free(rgba);
        set_err(err,errn,"image has no pixels");
        return false;
    }
    uint32_t* px=malloc(nbytes);
    if(!px){ free(rgba); set_err(err,errn,"out of memory"); return false; }
    for(size_t i=0;i<npix;i++){
        const uint8_t* s=rgba+i*4;
        px[i]=((uint32_t)s[3]<<24)|((uint32_t)s[0]<<16)|((uint32_t)s[1]<<8)|(uint32_t)s[2];
    }
    free(rgba);
    out->pixels=px;out->width=w;out->height=h;
    return true;
}

// Box average by an integer factor. Only PNG needs this -- the JPEG decoder
// reduces during its own colour-convert pass, before a full-size buffer exists.
static bool shrink(Image* img,uint32_t max_dim){
    if(!max_dim)return true;
    size_t big=img->width>img->height?img->width:img->height;
    if(big<=max_dim)return true;
    size_t scale=(big+max_dim-1)/max_dim;
    size_t ow=(img->width+scale-1)/scale, oh=(img->height+scale-1)/scale;
    if(!ow)ow=1;
    if(!oh)oh=1;

    size_t n;
    if(!mul_ok(ow,oh,&n))return false;
    uint32_t* dst=malloc(n*sizeof *dst);
    if(!dst)return false;   // keeping the full-size image beats failing the load

    for(size_t oy=0;oy<oh;oy++){
        size_t y0=oy*scale,y1=y0+scale;
        if(y1>img->height)y1=img->height;
        for(size_t ox=0;ox<ow;ox++){
            size_t x0=ox*scale,x1=x0+scale;
            if(x1>img->width)x1=img->width;
            uint32_t a=0,r=0,g=0,b=0,cnt=0;
            for(size_t y=y0;y<y1;y++){
                const uint32_t* row=img->pixels+y*img->width;
                for(size_t x=x0;x<x1;x++){
                    uint32_t p=row[x];
                    a+=p>>24;r+=(p>>16)&0xFF;g+=(p>>8)&0xFF;b+=p&0xFF;cnt++;
                }
            }
            if(!cnt)cnt=1;
            dst[oy*ow+ox]=((a/cnt)<<24)|((r/cnt)<<16)|((g/cnt)<<8)|(b/cnt);
        }
    }
    free(img->pixels);
    img->pixels=dst;img->width=ow;img->height=oh;
    return true;
}

bool image_decode(const void* data,size_t len,uint32_t max_dim,
                  Image* out,char* err,size_t err_len){
    if(!out)return false;
    *out=(Image){0};
    if(!data||len==0){ set_err(err,err_len,"empty image data"); return false; }

    if(len>=8&&memcmp(data,PNG_MAGIC,8)==0){
        PngImage p={0};
        if(!png_decode(data,len,&p,err,err_len))return false;
        if(!pack_rgba(p.rgba,p.width,p.height,out,err,err_len))return false;
        // no png_free here: pack_rgba already took ownership of p.rgba
        shrink(out,max_dim);   // a failed shrink keeps the full-size image, which still works
        return true;
    }
    if(jpeg_is_jpeg(data,len)){
        JpegImage j={0};
        if(!jpeg_decode(data,len,max_dim,&j,err,err_len))return false;
        return pack_rgba(j.rgba,j.width,j.height,out,err,err_len);
    }
    set_err(err,err_len,"unrecognised image format (not png or jpeg)");
    return false;
}

bool image_decode_file(const char* path,uint32_t max_dim,
                       Image* out,char* err,size_t err_len){
    if(!out)return false;
    *out=(Image){0};
    if(!path){ set_err(err,err_len,"no path"); return false; }
    FILE* f=fopen(path,"rb");
    if(!f){ set_err(err,err_len,"could not open image file"); return false; }
    if(fseek(f,0,SEEK_END)!=0){ fclose(f); set_err(err,err_len,"image file is not seekable"); return false; }
    long sz=ftell(f);
    if(sz<0){ fclose(f); set_err(err,err_len,"could not size image file"); return false; }
    rewind(f);
    uint8_t* buf=malloc((size_t)sz?(size_t)sz:1);
    if(!buf){ fclose(f); set_err(err,err_len,"out of memory"); return false; }
    size_t got=fread(buf,1,(size_t)sz,f);
    fclose(f);
    if(got!=(size_t)sz){ free(buf); set_err(err,err_len,"truncated image file"); return false; }
    bool ok=image_decode(buf,got,max_dim,out,err,err_len);
    free(buf);
    return ok;
}

bool image_solid(uint32_t argb,Image* out){
    if(!out)return false;
    *out=(Image){0};
    uint32_t* p=malloc(sizeof *p);
    if(!p)return false;
    *p=argb;
    out->pixels=p;out->width=1;out->height=1;
    return true;
}

void image_free(Image* img){
    if(!img)return;
    free(img->pixels);
    img->pixels=NULL;img->width=0;img->height=0;
}

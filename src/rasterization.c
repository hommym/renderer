#include "rasterization.h"
#include "renderer.h"
#include "projection.h"
#include "utils.h"
#include "interpolation.h"
#include "transform.h"
#include <string.h>

// v3: binned, lock-free, deterministic edge-walk rasterizer.
//
// Two passes over a chunk of triangles:
//
//   A. SETUP (parallel by triangle). Each thread takes a contiguous, statically
//      assigned range of the chunk, transforms/culls/clips/projects it, and
//      writes the survivors into its OWN slice of a shared setup array. It then
//      records each survivor's index in its own bin for every row BAND the
//      triangle touches. Nothing is shared, so nothing is locked.
//
//   B. FILL (parallel by band). A band is a horizontal strip of the frame
//      buffer, and a band is claimed by exactly ONE thread, so a pixel has
//      exactly one writer and the lock disappears entirely.
//
// Determinism falls out of this for free. Thread t's setup slice sits entirely
// below thread t+1's, so walking a band's bins thread-by-thread visits
// triangles in increasing source order. Two fragments at exactly the same depth
// therefore resolve the same way on every run, which is what issue 1 was about.

#define NEAR_PLANE_MARGIN 1.0
#define BACKFACE_SIGN (-1.0)

// A chunk is sized so its setup array stays in cache rather than so that it
// holds a whole model: 2M triangles' worth of setup would be hundreds of MB.
#define CHUNK_TRIS   32768u
#define BAND_ROWS    16u

// Below this alpha a texel is treated as a hole rather than a colour: not
// painted, and not depth-written. True alpha blending would need the triangles
// sorted back to front, which a z-buffer-only pipeline has no machinery for.
#define ALPHA_CUTOFF 128u

static bool cull_forced=false;

typedef struct CamSnap {
    double z,z_end,focal_l;
    double x_centre,y_centre;
    double half_w,half_h;
    double eye[3];
} CamSnap;
static CamSnap cam;

void rasterization_frame_begin(void){
    Camera c=get_camera_pos();
    cam.z=c.z; cam.z_end=c.z_end; cam.focal_l=c.focal_l;
    cam.x_centre=c.x+(c.x_end-c.x)/2.0;
    cam.y_centre=c.y+(c.y_end-c.y)/2.0;
    cam.half_w=(double)screen_width/2.0;
    cam.half_h=(double)screen_hieght/2.0;
    const double* e=view_eye();
    cam.eye[0]=e[0]; cam.eye[1]=e[1]; cam.eye[2]=e[2];
}

// ---- setup records and bins ------------------------------------------------

typedef struct SetupTri {
    double x[3],y[3];        // screen space, floored, in y order
    double uw[3],vw[3],iw[3];// u/w, v/w and 1/w: the three things linear in screen space
    int32_t row0,row1;       // rows this triangle covers, clamped to the screen
} SetupTri;

typedef struct Bin { uint32_t* idx; uint32_t n,cap; } Bin;

static SetupTri* setup=NULL;
static size_t    setup_cap=0;
static uint32_t* setup_n=NULL;      // per thread: how many records it wrote
static Bin*      bins=NULL;         // [thread*nbands + band]
static uint32_t  nbands=0;
static uint32_t  nthreads=0;

static bool bin_push(Bin* b,uint32_t v){
    if(b->n==b->cap){
        uint32_t cap=b->cap?b->cap*2u:64u;
        uint32_t* p=realloc(b->idx,(size_t)cap*sizeof *p);
        if(!p)return false;          // drop the triangle rather than crash
        b->idx=p; b->cap=cap;
    }
    b->idx[b->n++]=v;
    return true;
}

// Sized once per window/thread-count and reused for the life of the frame loop.
bool rasterization_pool_init(uint32_t threads){
    uint32_t nb=(screen_hieght+BAND_ROWS-1u)/BAND_ROWS;
    if(nb==0)nb=1;
    if(threads==0)threads=1;
    if(bins&&nb==nbands&&threads==nthreads&&setup)return true;

    rasterization_pool_release();
    nbands=nb; nthreads=threads;
    // two output triangles per source triangle is the near clip's worst case
    setup_cap=(size_t)CHUNK_TRIS*2u;
    setup=malloc(setup_cap*sizeof *setup);
    setup_n=calloc(threads,sizeof *setup_n);
    bins=calloc((size_t)threads*nbands,sizeof *bins);
    if(!setup||!setup_n||!bins){ rasterization_pool_release(); return false; }
    return true;
}

void rasterization_pool_release(void){
    if(bins)for(size_t i=0;i<(size_t)nthreads*nbands;i++)free(bins[i].idx);
    free(bins); free(setup); free(setup_n);
    bins=NULL; setup=NULL; setup_n=NULL;
    setup_cap=0; nbands=0; nthreads=0;
}

// ---- geometry --------------------------------------------------------------

static Vectex lerp_vectex(Vectex a,Vectex b,double t){
if(t<0.0)t=0.0;
if(t>1.0)t=1.0;
Vectex out;
out.x=a.x+(b.x-a.x)*t;
out.y=a.y+(b.y-a.y)*t;
out.z=a.z+(b.z-a.z)*t;
out.u=a.u+(b.u-a.u)*t;
out.v=a.v+(b.v-a.v)*t;
out.colour=interpolate_colour(a.colour,b.colour,1000,(size_t)(t*1000.0+0.5));
return out;
}

static uint8_t clip_triangle_near(Vectex tri[3],Vectex out[2][3]){
double plane_z=cam.z+NEAR_PLANE_MARGIN;
Vectex inside[3],outside[3];
uint8_t in_n=0,out_n=0;
for(uint8_t i=0;i<3;i++){
    if(tri[i].z>=plane_z)inside[in_n++]=tri[i];
    else outside[out_n++]=tri[i];
}
if(in_n==0)return 0;
if(in_n==3){ out[0][0]=tri[0];out[0][1]=tri[1];out[0][2]=tri[2]; return 1; }
#define CROSS_T(a,b) ((plane_z-(a).z)/((b).z-(a).z))
if(in_n==1){
    out[0][0]=inside[0];
    out[0][1]=lerp_vectex(inside[0],outside[0],CROSS_T(inside[0],outside[0]));
    out[0][2]=lerp_vectex(inside[0],outside[1],CROSS_T(inside[0],outside[1]));
    return 1;
}
Vectex c0=lerp_vectex(inside[0],outside[0],CROSS_T(inside[0],outside[0]));
Vectex c1=lerp_vectex(inside[1],outside[0],CROSS_T(inside[1],outside[0]));
#undef CROSS_T
out[0][0]=inside[0];out[0][1]=inside[1];out[0][2]=c0;
out[1][0]=inside[1];out[1][1]=c1;out[1][2]=c0;
return 2;
}

bool is_vectex_visible(Vectex point,PixelCord* pxcord_p){
Camera c=get_camera_pos();
if(point.z>c.z_end){ (*pxcord_p).is_visible=false; return false; }
double y_center=c.y+(c.y_end-c.y)/2;
double y_offset=(screen_hieght/2)*(point.z-c.z)/c.focal_l;
double x_center=c.x+(c.x_end-c.x)/2;
double x_offset=(screen_width/2)*(point.z-c.z)/c.focal_l;
(*pxcord_p).is_visible = point.x>=(x_center-x_offset) && point.x<(x_center+x_offset)
                      && point.y>=(y_center-y_offset) && point.y<(y_center+y_offset);
return (*pxcord_p).is_visible;
}

static bool is_back_facing(const Vectex t[3]){
double ax=t[1].x-t[0].x, ay=t[1].y-t[0].y, az=t[1].z-t[0].z;
double bx=t[2].x-t[0].x, by=t[2].y-t[0].y, bz=t[2].z-t[0].z;
double nx=ay*bz-az*by, ny=az*bx-ax*bz, nz=ax*by-ay*bx;
double ex=t[0].x-cam.eye[0], ey=t[0].y-cam.eye[1], ez=t[0].z-cam.eye[2];
return (nx*ex+ny*ey+nz*ez)*BACKFACE_SIGN > 0.0;
}

void set_backface_cull_forced(bool on){ cull_forced=on; }

// ---- pass A: setup ---------------------------------------------------------

// Builds one SetupTri from a clipped, front-facing triangle. Returns false when
// nothing of it can land on screen.
static bool setup_one(const Vectex tri[3],SetupTri* s){
    double x[3],y[3],uw[3],vw[3],iw[3];
    for(int k=0;k<3;k++){
        double w=tri[k].z-cam.z;
        if(w<NEAR_PLANE_MARGIN)w=NEAR_PLANE_MARGIN;   // the near clip guarantees this
        double r=1.0/w;
        // identical arithmetic to perspective_projection(), with 1/w in hand
        x[k]=floor(cam.half_w+(tri[k].x-cam.x_centre)*cam.focal_l*r);
        y[k]=floor(cam.half_h+(tri[k].y-cam.y_centre)*cam.focal_l*r);
        uw[k]=tri[k].u*r; vw[k]=tri[k].v*r; iw[k]=r;
    }

    // screen-space bounding box reject. this is what replaces the old
    // "is any vertex inside the camera box" test, which threw away every
    // triangle bigger than the screen -- all three corners are outside one.
    double bx0=x[0],bx1=x[0],by0=y[0],by1=y[0];
    for(int k=1;k<3;k++){
        if(x[k]<bx0)bx0=x[k]; else if(x[k]>bx1)bx1=x[k];
        if(y[k]<by0)by0=y[k]; else if(y[k]>by1)by1=y[k];
    }
    if(bx1<0.0||by1<0.0||bx0>=(double)screen_width||by0>=(double)screen_hieght)return false;
    // a whole triangle behind the far plane contributes nothing
    if(tri[0].z>cam.z_end&&tri[1].z>cam.z_end&&tri[2].z>cam.z_end)return false;

    int i0=0,i1=1,i2=2;
    if(y[i0]>y[i1]){int t=i0;i0=i1;i1=t;}
    if(y[i1]>y[i2]){int t=i1;i1=i2;i2=t;}
    if(y[i0]>y[i1]){int t=i0;i0=i1;i1=t;}
    const int ord[3]={i0,i1,i2};
    for(int k=0;k<3;k++){
        int o=ord[k];
        s->x[k]=x[o]; s->y[k]=y[o];
        s->uw[k]=uw[o]; s->vw[k]=vw[o]; s->iw[k]=iw[o];
    }
    int64_t r0=(int64_t)s->y[0], r1=(int64_t)s->y[2];
    if(r0<0)r0=0;
    if(r1>(int64_t)screen_hieght-1)r1=(int64_t)screen_hieght-1;
    if(r0>r1)return false;
    s->row0=(int32_t)r0; s->row1=(int32_t)r1;
    return true;
}

// Thread `tid` processes source triangles [lo,hi) of the object.
static void setup_range(const Object* obj,uint64_t lo,uint64_t hi,uint32_t tid,uint64_t chunk_lo){
    // thread t owns setup slots [2*(lo-chunk_lo), 2*(hi-chunk_lo)) and nothing else
    size_t slot=(size_t)(lo-chunk_lo)*2u;
    size_t written=0;
    const uint64_t* conn=obj->connectors_sequence;
    const Vectex*  vtx=obj->vertices;

    for(uint64_t t=lo;t<hi;t++){
        Vectex src[3]={vtx[conn[t*3]],vtx[conn[t*3+1]],vtx[conn[t*3+2]]};
        Vectex vw3[3]={view_apply(src[0]),view_apply(src[1]),view_apply(src[2])};
        if((cull_forced||!obj->double_sided)&&is_back_facing(vw3))continue;

        Vectex clipped[2][3];
        uint8_t nc=clip_triangle_near(vw3,clipped);
        for(uint8_t c=0;c<nc;c++){
            SetupTri* s=&setup[slot+written];
            if(!setup_one(clipped[c],s))continue;
            uint32_t b0=(uint32_t)s->row0/BAND_ROWS;
            uint32_t b1=(uint32_t)s->row1/BAND_ROWS;
            if(b1>=nbands)b1=nbands-1;
            for(uint32_t b=b0;b<=b1;b++)bin_push(&bins[(size_t)tid*nbands+b],(uint32_t)(slot+written));
            written++;
        }
    }
    setup_n[tid]=(uint32_t)written;
}

// ---- pass B: fill ----------------------------------------------------------

static void fill_tri(const SetupTri* s,const Object* obj,int64_t band_r0,int64_t band_r1){
    PixelCord* fb=renderer_back_buffer();
    const size_t tw=obj->texture_width, th=obj->texture_height;
    const uint32_t* texture=obj->texture;

    int64_t r0=s->row0, r1=s->row1;
    if(r0<band_r0)r0=band_r0;
    if(r1>band_r1)r1=band_r1;
    if(r0>r1)return;

    double y0=s->y[0],y1=s->y[1],y2=s->y[2];
    // one reciprocal per edge per triangle instead of a divide per row
    double inv_ac=(y2!=y0)?1.0/(y2-y0):0.0;
    double inv_ab=(y1!=y0)?1.0/(y1-y0):0.0;
    double inv_bc=(y2!=y1)?1.0/(y2-y1):0.0;

    for(int64_t row=r0;row<=r1;row++){
        double y=(double)row;

        double t1=(y-y0)*inv_ac;
        if(t1<0.0)t1=0.0; else if(t1>1.0)t1=1.0;
        double xl =s->x[0] +(s->x[2] -s->x[0])*t1;
        double uwl=s->uw[0]+(s->uw[2]-s->uw[0])*t1;
        double vwl=s->vw[0]+(s->vw[2]-s->vw[0])*t1;
        double iwl=s->iw[0]+(s->iw[2]-s->iw[0])*t1;

        double xr,uwr,vwr,iwr;
        if(y<y1||inv_bc==0.0){
            if(inv_ab==0.0){ xr=s->x[1]; uwr=s->uw[1]; vwr=s->vw[1]; iwr=s->iw[1]; }
            else{
                double t2=(y-y0)*inv_ab;
                if(t2<0.0)t2=0.0; else if(t2>1.0)t2=1.0;
                xr =s->x[0] +(s->x[1] -s->x[0])*t2;
                uwr=s->uw[0]+(s->uw[1]-s->uw[0])*t2;
                vwr=s->vw[0]+(s->vw[1]-s->vw[0])*t2;
                iwr=s->iw[0]+(s->iw[1]-s->iw[0])*t2;
            }
        }else{
            double t2=(y-y1)*inv_bc;
            if(t2<0.0)t2=0.0; else if(t2>1.0)t2=1.0;
            xr =s->x[1] +(s->x[2] -s->x[1])*t2;
            uwr=s->uw[1]+(s->uw[2]-s->uw[1])*t2;
            vwr=s->vw[1]+(s->vw[2]-s->vw[1])*t2;
            iwr=s->iw[1]+(s->iw[2]-s->iw[1])*t2;
        }

        if(xl>xr){
            double t;
            t=xl;xl=xr;xr=t; t=uwl;uwl=uwr;uwr=t;
            t=vwl;vwl=vwr;vwr=t; t=iwl;iwl=iwr;iwr=t;
        }

        double span=xr-xl;
        double duw=0.0,dvw=0.0,diw=0.0;
        if(span>0.0){
            double inv=1.0/span;
            duw=(uwr-uwl)*inv; dvw=(vwr-vwl)*inv; diw=(iwr-iwl)*inv;
        }

        int64_t px0=(int64_t)xl, px1=(int64_t)xr;
        double lead=0.0;
        if(px0<0){ lead=(double)(0-px0); px0=0; }
        if(px1>(int64_t)screen_width-1)px1=(int64_t)screen_width-1;
        if(px0>px1)continue;

        double uw=uwl+duw*lead, vw=vwl+dvw*lead, iw=iwl+diw*lead;
        PixelCord* rowp=fb+(size_t)row*(size_t)screen_width;

        for(int64_t x=px0;x<=px1;x++){
            // the single divide per pixel. it buys the true depth AND
            // perspective-correct texture coordinates at the same time.
            double w=(iw>0.0)?1.0/iw:0.0;
            double z=cam.z+w;
            if(z>=cam.z&&z<=cam.z_end){
                PixelCord* dst=rowp+x;
                if(!(dst->in_use&&dst->z<z)){
                    double uu=uw*w, vv=vw*w;
                    // The renderer holds this bound itself rather than trusting
                    // every producer of an Object to have wrapped its uv. The
                    // cast is to a SIGNED type on purpose: a negative v cast
                    // straight to size_t is an enormous subscript, and no clamp
                    // afterwards can undo that.
                    int64_t f_row=(int64_t)(vv*(double)th);
                    int64_t f_col=(int64_t)(uu*(double)tw);
                    if(f_row<0)f_row=0; else if(f_row>=(int64_t)th)f_row=(int64_t)th-1;
                    if(f_col<0)f_col=0; else if(f_col>=(int64_t)tw)f_col=(int64_t)tw-1;
                    uint32_t texel=texture[(size_t)f_row*tw+(size_t)f_col];

                    // Alpha cutout. A base-colour texture uses alpha to cut a
                    // leaf out of the quad it is drawn on -- 37.7% of the eco
                    // house's atlas is fully transparent -- and a transparent
                    // texel that wins the depth test punches a hole through
                    // everything behind it. Skipping the pixel entirely is what
                    // a cutout material wants, and it costs one branch.
                    if((texel>>24)<ALPHA_CUTOFF){ uw+=duw; vw+=dvw; iw+=diw; continue; }

                    dst->px=(double)x; dst->py=(double)row; dst->z=z;
                    dst->u=(float)uu; dst->v=(float)vv;
                    dst->is_visible=true; dst->in_use=true;
                    dst->colour=texel;
                }
            }
            uw+=duw; vw+=dvw; iw+=diw;
        }
    }
}

// One band, walked in canonical order: every thread's bin in thread order, and
// each bin in push order. That is increasing source-triangle order, so a depth
// tie resolves identically on every run.
static void fill_band(const Object* obj,uint32_t band){
    int64_t r0=(int64_t)band*BAND_ROWS;
    int64_t r1=r0+BAND_ROWS-1;
    if(r1>(int64_t)screen_hieght-1)r1=(int64_t)screen_hieght-1;
    for(uint32_t t=0;t<nthreads;t++){
        Bin* b=&bins[(size_t)t*nbands+band];
        for(uint32_t i=0;i<b->n;i++)fill_tri(&setup[b->idx[i]],obj,r0,r1);
    }
}

// ---- the frame worker ------------------------------------------------------

static _Atomic uint32_t band_cursor;

void rasterization_setup_pass(const Object* obj,uint64_t chunk_lo,uint64_t chunk_hi,uint32_t tid){
    uint64_t n=chunk_hi-chunk_lo;
    uint64_t lo=chunk_lo+n*tid/nthreads;
    uint64_t hi=chunk_lo+n*(tid+1)/nthreads;
    setup_range(obj,lo,hi,tid,chunk_lo);
}

void rasterization_fill_pass(const Object* obj){
    uint32_t b;
    while((b=atomic_fetch_add(&band_cursor,1u))<nbands)fill_band(obj,b);
}

void rasterization_chunk_reset(uint32_t tid){
    for(uint32_t b=0;b<nbands;b++)bins[(size_t)tid*nbands+b].n=0;
    setup_n[tid]=0;
    if(tid==0)atomic_store(&band_cursor,0u);
}

uint32_t rasterization_chunk_size(void){ return CHUNK_TRIS; }

#include "renderer.h"





// double camera.focal_l=30;
uint32_t screen_width; //max column on fram_buffer
uint32_t screen_hieght; // max row on frame_buffer
static Camera camera={
0.0,
0.0,
0.0,
0.0,
0.0,
15000.0, //default far plane distance
0.0,           // focal_l recomputed in setup_camera
1.047197551,   // v_fov (const): 60 deg in rad
0.0,           // h_fov recomputed in setup_camera
};



Object* objects;
static size_t objects_len=0;
static void* frame=NULL;



static void setup_camera(){

    // v_fov fixes the vertical angle; focal_l follows from screen height,
    // and h_fov falls out of the resulting aspect ratio.
    camera.focal_l=screen_hieght/(2*tan(0.5*camera.v_fov));
    camera.h_fov=2*atan(screen_width/(2*camera.focal_l));

    // calculate x and y points of the extremes of the fov
    double x_center=camera.x+(camera.x_end-camera.x)/2;
    double x_offset=(screen_width/2)*(camera.z_end-camera.z)/camera.focal_l;
    

    double y_center=camera.y+(camera.y_end-camera.y)/2;
    double y_offset=(screen_hieght/2)*(camera.z_end-camera.z)/camera.focal_l;
   

    camera.x_end=x_center +x_offset;
    camera.x=x_center-x_offset;

    camera.y_end=y_center+y_offset;
    camera.y=y_center-y_offset;
    printf("X =%f\n",camera.x);
    printf("X end=%f\n",camera.x_end);

    printf("Y =%f\n",camera.y);
    printf("Y end=%f\n",camera.y_end);



}


static void create_frame_buffer(uint32_t win_w,uint32_t win_h){
frame= calloc((win_h*win_w),sizeof(PixelCord));
}

static bool is_vectex_visible(Vectex point,PixelCord* pxcord_p){
bool is_z_in_view=point.z<=camera.z_end;
if(!is_z_in_view){
    // write the flag even on the early out. callers read it back off the
    // PixelCord afterwards, and leaving it untouched hands them whatever
    // happened to be on the stack.
    (*pxcord_p).is_visible=false;
    return false;
}

double y_center=camera.y+(camera.y_end-camera.y)/2;
double y_offset=(screen_hieght/2)*(point.z-camera.z)/camera.focal_l;


double x_center=camera.x+(camera.x_end-camera.x)/2;    
double x_offset=(screen_width/2)*(point.z-camera.z)/camera.focal_l;


bool is_x_in_view=point.x>=(x_center-x_offset) && point.x<(x_center+x_offset);
bool is_y_in_view=point.y>=(y_center-y_offset) && point.y<(y_center+y_offset);

(*pxcord_p).is_visible=is_x_in_view && is_y_in_view;


return (*pxcord_p).is_visible;
}


Camera get_camera_pos(){
return camera;
}


// How far in front of the camera the near plane sits, in world units. The
// perspective divide is only meaningful in front of the camera: at depth 0 it
// is undefined, and behind the camera it flips the sign of the offset, so
// geometry projects inverted instead of disappearing.
#define NEAR_PLANE_MARGIN 1.0

// Walk t of the way from a to b, in world space and in colour.
static Vectex lerp_vectex(Vectex a,Vectex b,double t){
if(t<0.0)t=0.0;
if(t>1.0)t=1.0;
Vectex out;
out.x=a.x+(b.x-a.x)*t;
out.y=a.y+(b.y-a.y)*t;
out.z=a.z+(b.z-a.z)*t;
// reuse the per-channel lerp rather than blending the packed word, which
// would carry bits between channels. 1000 steps is far finer than 8 bits.
out.colour=interpolate_colour(a.colour,b.colour,1000,(size_t)(t*1000.0+0.5));
return out;
}

// Clip a triangle against the near plane. Writes up to 2(max) replacement triangles
// into out argument of the function and returns how many triangles were written: 0 when the triangle is wholly behind the
// plane, 1 when it is wholly in front (unchanged) or only one corner survives,
// 2 when two corners survive and the remainder is a quad.
static uint8_t clip_triangle_near(Vectex tri[3],Vectex out[2][3]){
double plane_z=camera.z+NEAR_PLANE_MARGIN;

Vectex inside[3],outside[3];
uint8_t inside_n=0,outside_n=0;
//checking which vertex of the triangle is inside the near plane or outside it
for(uint8_t i=0;i<3;i++){
    if(tri[i].z>=plane_z)inside[inside_n++]=tri[i];
    else outside[outside_n++]=tri[i];
}

if(inside_n==0)return 0;
if(inside_n==3){
    out[0][0]=tri[0];
    out[0][1]=tri[1];
    out[0][2]=tri[2];
    return 1;
}

// where along in->out the edge crosses the plane. the two points sit on
// opposite sides, so the denominator is never zero and t lands in [0,1].
#define CROSS_T(in_v,out_v) ((plane_z-(in_v).z)/((out_v).z-(in_v).z))

if(inside_n==1){
    // one corner survives: what is left is a smaller triangle.
    out[0][0]=inside[0];
    out[0][1]=lerp_vectex(inside[0],outside[0],CROSS_T(inside[0],outside[0]));
    out[0][2]=lerp_vectex(inside[0],outside[1],CROSS_T(inside[0],outside[1]));
    return 1;
}

// two corners survive: what is left is the quad (inside0, inside1, cross1,
// cross0). split it on the inside1..cross0 diagonal.
Vectex cross0=lerp_vectex(inside[0],outside[0],CROSS_T(inside[0],outside[0]));
Vectex cross1=lerp_vectex(inside[1],outside[0],CROSS_T(inside[1],outside[0]));
#undef CROSS_T

out[0][0]=inside[0];
out[0][1]=inside[1];
out[0][2]=cross0;

out[1][0]=inside[1];
out[1][1]=cross1;
out[1][2]=cross0;
return 2;
}


void* get_frame_buffer(){
    return frame;
}

void render_init(Object* objs,uint64_t len,uint32_t win_w,uint32_t win_h,bool wirefame_mode){
// needs to be called once to initialise the renderer
(void)wirefame_mode; // the mode is chosen per call in render(), not stored here
create_frame_buffer(win_w,win_h);
objects=objs;
objects_len=len;
screen_width=win_w;
screen_hieght=win_h;
setup_camera();
}


bool render(bool wirefame_mode){
if(frame==NULL)return false;

PixelCord (*frame_buffer)[screen_width]= (PixelCord (*)[screen_width])frame;   


// rasterization
for(size_t x=0;x<objects_len;x++){
Object obj=objects[x];

// no connectors -> paint each visible vertex as one pixel
if(obj.len_of_connectors==0){
    for(size_t v=0;v<obj.len_of_vertices;v++){
        Vectex pt=obj.vertices[v];
        PixelCord pc={.z=pt.z,.is_visible=false,.in_use=false,.colour=pt.colour};
        if(!is_vectex_visible(pt,&pc)) continue;
        pc.px=perspective_projection(pt.x,pt.z,camera.z,camera.focal_l,camera.x,camera.x_end,screen_width);
        pc.py=perspective_projection(pt.y,pt.z,camera.z,camera.focal_l,camera.y,camera.y_end,screen_hieght);
        PixelCord point0=frame_buffer[(uint64_t)pc.py][(uint64_t)pc.px];
        if(point0.in_use && point0.z<pt.z) continue;
        pc.in_use=true;
        frame_buffer[(uint64_t)pc.py][(uint64_t)pc.px]=pc;
    }
    continue;
}

// triangles are stored as flat triples of vertex indices: (v0,v1,v2, v3,v4,v5, ...).
// step by 3 to walk one triangle per iteration.
// tri_base+2 so a connector list that is not a whole number of triples stops
// short instead of reading past the end.
for(size_t tri_base=0;tri_base+2<obj.len_of_connectors;tri_base+=3){
Vectex source_triangle[3]={obj.vertices[obj.connectors_sequence[tri_base]],
obj.vertices[obj.connectors_sequence[tri_base+1]]
,obj.vertices[obj.connectors_sequence[tri_base+2]]};

// clip against the near plane before projecting. a vertex at or behind the
// camera has no meaningful projection, so it has to be replaced by the point
// where its edges cross the plane — one triangle can come back as two.
Vectex clipped[2][3];
uint8_t clipped_count=clip_triangle_near(source_triangle,clipped);

for(uint8_t clip_i=0;clip_i<clipped_count;clip_i++){
Vectex triangle[3]={clipped[clip_i][0],clipped[clip_i][1],clipped[clip_i][2]};
double min_y=0,max_y=0;

// screen-space versions of the 3 world-space vertices above.
PixelCord triangle_proj[3]={0};
// one bresenham line per triangle edge: (0,1), (0,2), (1,2). populated in that order.
PixelCord* edge_pixels[3]={NULL,NULL,NULL};
uint64_t edge_pixel_count[3]={0,0,0};
uint8_t edge_count=0;


// early frustum cull: if no vertex is visible, no pixel of this triangle can
// be. call all three unconditionally — || would short-circuit on the first
// true and leave the other two triangle_proj slots unwritten.
bool is_v0_visible=is_vectex_visible(triangle[0],triangle_proj+0);
bool is_v1_visible=is_vectex_visible(triangle[1],triangle_proj+1);
bool is_v2_visible=is_vectex_visible(triangle[2],triangle_proj+2);
if(!(is_v0_visible || is_v1_visible || is_v2_visible))continue;

// walk the 3 unique unordered edges of the triangle:
//   edge_a=0 → pairs (0,1) and (0,2)
//   edge_a=1 → pair  (1,2)
// project each vertex once, run bresenham on each edge once.
for(size_t edge_a=0;edge_a<2;edge_a++){
// caches "triangle_proj[edge_a] has already been projected" so the second
// edge_b iteration on the same edge_a doesn't reproject the same vertex.
bool outer_projected=false;
    for(size_t edge_b=edge_a+1;edge_b<=2;edge_b++){
        // when edge_a==1, both endpoints (indices 1 and 2) were already
        // projected during edge_a==0's iterations — skip projection and go
        // straight to bresenham on the (1,2) edge.
        Vectex v1=triangle[edge_a];
        Vectex v2=triangle[edge_b];
        if(edge_a!=1){

         if(!outer_projected){
            triangle_proj[edge_a]=(PixelCord){.z=v1.z,.colour=v1.colour,.is_visible=triangle_proj[edge_a].is_visible,.in_use=true};
            triangle_proj[edge_a].px=perspective_projection(v1.x,v1.z,camera.z,camera.focal_l,camera.x,camera.x_end,screen_width);
            triangle_proj[edge_a].py=perspective_projection(v1.y,v1.z,camera.z,camera.focal_l,camera.y,camera.y_end,screen_hieght);

            // seed the projected bounding box from the first projected vertex.
            min_y=triangle_proj[edge_a].py;
            max_y=triangle_proj[edge_a].py;
         }
         triangle_proj[edge_b]=(PixelCord){.z=v2.z,.colour=v2.colour,.is_visible=triangle_proj[edge_b].is_visible,.in_use=true};
         triangle_proj[edge_b].px=perspective_projection(v2.x,v2.z,camera.z,camera.focal_l,camera.x,camera.x_end,screen_width);
         triangle_proj[edge_b].py=perspective_projection(v2.y,v2.z,camera.z,camera.focal_l,camera.y,camera.y_end,screen_hieght);

        
         if(min_y>triangle_proj[edge_b].py)min_y=triangle_proj[edge_b].py;
         if(max_y<triangle_proj[edge_b].py)max_y=triangle_proj[edge_b].py;

        }

        // rasterize this edge. bresenham fills edge_line with one PixelCord
        // per step along the edge, each carrying its own interpolated z and
        // colour so the scanline fill below can sample them by px position.
        int64_t dx=   triangle_proj[edge_a].px-triangle_proj[edge_b].px;
        int64_t dy=   triangle_proj[edge_a].py-triangle_proj[edge_b].py;

        if(dx<0)dx*=-1;
        if(dy<0)dy*=-1;

        uint64_t edge_len=dx>=dy?(dx+1):(dy+1);
        PixelCord* edge_line=calloc(edge_len,sizeof(PixelCord)); // pixel strip for this edge, one PixelCord per bresenham step
        if(edge_line==NULL)continue;
        bresenhame_line_algo(triangle_proj[edge_a],triangle_proj[edge_b],edge_line);
        edge_pixels[edge_count]=edge_line;
        edge_pixel_count[edge_count]=edge_len;
        outer_projected=true;
        edge_count++;
    }

}

// scanline pass: for each pixel row from min_y to max_y, gather the edge
// pixels that landed on that row, sort them left-to-right, and fill the
// gaps between consecutive pairs.
uint64_t scanline_count=((uint64_t)(max_y-min_y))+1;
uint64_t scanline_slot_cap=edge_pixel_count[0]+edge_pixel_count[1]+edge_pixel_count[2];
// each entry is a POINTER into the edge_pixels arrays — no copies, so the
// pointed-to PixelCords keep their original z/colour for interpolation. one
// row at a time: the row is gathered, sorted, filled, then never revisited.
PixelCord** scanline_pixels=scanline_slot_cap>0?calloc(scanline_slot_cap,sizeof(PixelCord*)):NULL;

// only walk rows that are actually on screen. removing rows from the triangle which is off screen
int64_t first_row=0;
int64_t last_row=(int64_t)scanline_count-1;
if(min_y<0.0){
    int64_t skip=(int64_t)(-min_y);
    if(skip>first_row)first_row=skip;
}
if(max_y>(double)screen_hieght-1.0){
    int64_t stop=(int64_t)((double)screen_hieght-1.0-min_y);
    if(stop<last_row)last_row=stop;
}

// each bresenham strip is monotonic in py — y only ever moves by a fixed
// step_y — so the rows can be walked with one cursor per edge. rescanning all
// three strips for every row made the gather rows x pixels, which is what
// turned a triangle near the camera into a multi-second stall.
int8_t edge_dir[3]={1,1,1};
int64_t edge_cursor[3]={0,0,0};
for(uint8_t edge_i=0;edge_i<edge_count;edge_i++){
    uint64_t n=edge_pixel_count[edge_i];
    bool ascending= n<2 || edge_pixels[edge_i][n-1].py>=edge_pixels[edge_i][0].py;
    edge_dir[edge_i]= ascending?1:-1;
    edge_cursor[edge_i]= ascending?0:(int64_t)n-1;
}

for(int64_t row=first_row;scanline_pixels!=NULL && row<=last_row;row++){
double row_y=min_y+(double)row;
uint64_t scanline_pixel_count=0;

// take this row's pixels off each strip, advancing the cursor past them.
for(uint8_t edge_i=0;edge_i<edge_count;edge_i++){
PixelCord* line=edge_pixels[edge_i];
int64_t n=(int64_t)edge_pixel_count[edge_i];
int64_t cursor=edge_cursor[edge_i];
int8_t dir=edge_dir[edge_i];

while(cursor>=0 && cursor<n && line[cursor].py<row_y)cursor+=dir;
while(cursor>=0 && cursor<n && line[cursor].py==row_y){
    scanline_pixels[scanline_pixel_count]=line+cursor;
    scanline_pixel_count++;
    cursor+=dir;
}

edge_cursor[edge_i]=cursor;
}

// left-to-right sort by px so the pair walk below defines contiguous spans.
sort_pixelcords_by_px(scanline_pixels, scanline_pixel_count);

// walk consecutive pairs (current,next). the gap between them is the row's
// interior on this side of the triangle — fill it with interpolated pixels.
for(uint64_t pair_i=0;pair_i<scanline_pixel_count;pair_i++){
PixelCord current=*(scanline_pixels[pair_i]);

if((pair_i+1)!=scanline_pixel_count){
PixelCord next=*(scanline_pixels[pair_i+1]);

if((next.px-current.px)!=1){
    size_t span=(size_t)(next.px-current.px);

    // clamp the walk to the screen. the span itself stays the interpolation
    // denominator, so the colours and depths of the pixels we do write are
    // unchanged — we just skip the ones that could never be stored.
    double span_start=current.px+1;
    double span_end=next.px;
    if(span_start<0.0)span_start=0.0;
    if(span_end>(double)screen_width)span_end=(double)screen_width;

    for(double fill_x=span_start;fill_x<span_end;fill_x++){
        // how far along the span this pixel sits. derived from fill_x rather
        // than counted, so clamping the start cannot shift the gradient.
        size_t fill_i=(size_t)(fill_x-current.px);
        PixelCord fill_pixel={.py=current.py,.px=fill_x,
        .z=interpolate(current.z,next.z,span,fill_i),
        .colour=interpolate_colour(current.colour,next.colour,span,fill_i),
        .in_use=true
        };
        fill_pixel.is_visible=(current.py>=0&&current.py<screen_hieght) && fill_pixel.z>=camera.z&&fill_pixel.z<=camera.z_end;
        if(!fill_pixel.is_visible)continue;
        PixelCord existing=frame_buffer[(uint64_t)fill_pixel.py][(uint64_t)fill_pixel.px];

        // z-buffer test: only overwrite if this pixel is closer to the camera.
        if(!(existing.in_use && existing.z<fill_pixel.z))frame_buffer[(uint64_t)fill_pixel.py][(uint64_t)fill_pixel.px]=fill_pixel;
    }
}



}

if(!current.is_visible)continue;
PixelCord existing=frame_buffer[(uint64_t)current.py][(uint64_t)current.px];

// z-buffer test again for the edge pixel itself.
if(!(existing.in_use && existing.z<current.z))frame_buffer[(uint64_t)current.py][(uint64_t)current.px]=current;
}

}

for(uint8_t h=0;h<edge_count;h++)free(edge_pixels[h]);
free(scanline_pixels);
}
}
    
}

return true;
}

void clear_frame_buffer(bool keep_frame){
    if(!keep_frame)free(frame);
    if(screen_hieght!=0 && screen_width!=0)create_frame_buffer(screen_width,screen_hieght);
    else frame=NULL;
}

void renderer_resize(uint32_t win_w,uint32_t win_h){
    screen_width=win_w;
    screen_hieght=win_h;
    setup_camera();
    clear_frame_buffer(false);
}

void move_camera(double unit,Movement direction){
    switch (direction)
    {
    case MOV_LEFT:
        camera.x-=unit;
        camera.x_end-=unit;
        break;
    case MOV_RIGHT:
        camera.x+=unit;
        camera.x_end+=unit;
        break;
    case MOV_UP:
        camera.y+=unit;
        camera.y_end+=unit;
        break;
    case MOV_DOWN:
        camera.y-=unit;
        camera.y_end-=unit;
        break;
    case MOV_FORWARD:
        camera.z+=unit;
        camera.z_end+=unit;
        break;
    default:
        //backewards
        camera.z-=unit;
        camera.z_end-=unit;
        break;
    }   

}
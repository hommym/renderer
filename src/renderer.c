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
bool is_z_in_view=point.z>=camera.z && point.z<=camera.z_end;
if(!is_z_in_view)return false;

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


void* get_frame_buffer(){
    return frame;
}

void render_init(Object* objs,uint64_t len,uint32_t win_w,uint32_t win_h,bool wirefame_mode){
// needs to be called once to initialise the renderer  
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

if(wirefame_mode){
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

for(size_t a=0;a<obj.len_of_connectors;a+=2){
    Vectex v1=obj.vertices[obj.connectors_sequence[a]];
    PixelCord p1={.z=v1.z,.is_visible=false,.in_use=false,.colour=v1.colour};

    Vectex v2=obj.vertices[obj.connectors_sequence[a+1]];
    PixelCord p2={.z=v2.z,.is_visible=false,.in_use=false,.colour=v2.colour};

    
    if(!(is_vectex_visible(v1,&p1) || is_vectex_visible(v2,&p2)))continue;

    p1.px=perspective_projection(v1.x,v1.z,camera.z,camera.focal_l,camera.x,camera.x_end,screen_width);
    p1.py=perspective_projection(v1.y,v1.z,camera.z,camera.focal_l,camera.y,camera.y_end,screen_hieght);


    p2.px=perspective_projection(v2.x,v2.z,camera.z,camera.focal_l,camera.x,camera.x_end,screen_width);
    p2.py=perspective_projection(v2.y,v2.z,camera.z,camera.focal_l,camera.y,camera.y_end,screen_hieght);

    int64_t ch_x= p1.px-p2.px;
    int64_t ch_y= p1.py-p2.py;
  
    if(ch_x<0)ch_x*=-1;
    if(ch_y<0)ch_y*=-1;

    size_t lines_len=ch_x>=ch_y?(ch_x+1):(ch_y+1);
    if(lines_len==1)continue;
    PixelCord* lines_arr=calloc(lines_len,sizeof(PixelCord)); // an array for storing vectex pointer to be coloured to form the line
    bresenhame_line_algo(p1,p2,lines_arr);


    for(size_t i=0;i<lines_len;i++){
        PixelCord point=lines_arr[i];
        if(!point.is_visible)continue;
        PixelCord point0=frame_buffer[(uint64_t)point.py][(uint64_t)point.px];

        if(point0.in_use && point0.z<point.z) continue;
        frame_buffer[(uint64_t)point.py][(uint64_t)point.px]=point;
    }
    free(lines_arr);
}

}


}

else{

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
for(size_t tri_base=0;tri_base<obj.len_of_connectors;tri_base+=3){
double min_y,max_y,min_x,max_x;
Vectex triangle[3]={obj.vertices[obj.connectors_sequence[tri_base]],
obj.vertices[obj.connectors_sequence[tri_base+1]]
,obj.vertices[obj.connectors_sequence[tri_base+2]]};

// screen-space versions of the 3 world-space vertices above.
PixelCord triangle_proj[3];
// one bresenham line per triangle edge: (0,1), (0,2), (1,2). populated in that order.
PixelCord* edge_pixels[3];
uint64_t edge_pixel_count[3];
uint8_t edge_count=0;


// early frustum cull: if no vertex is visible, no pixel of this triangle can
// be. short-circuit on || stops at the first true — is_vectex_visible only
// runs for as many vertices as needed to prove "at least one visible".
bool is_any_vectex_visible=is_vectex_visible(triangle[0],triangle_proj+0) || is_vectex_visible(triangle[1],triangle_proj+1) || is_vectex_visible(triangle[2],triangle_proj+2);
if(!is_any_vectex_visible)continue;

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
            min_x=triangle_proj[edge_a].px;
            max_x=triangle_proj[edge_a].px;
         }
         triangle_proj[edge_b]=(PixelCord){.z=v2.z,.colour=v2.colour,.is_visible=triangle_proj[edge_b].is_visible,.in_use=true};
         triangle_proj[edge_b].px=perspective_projection(v2.x,v2.z,camera.z,camera.focal_l,camera.x,camera.x_end,screen_width);
         triangle_proj[edge_b].py=perspective_projection(v2.y,v2.z,camera.z,camera.focal_l,camera.y,camera.y_end,screen_hieght);

         // extend bounding box to cover the newly projected vertex.
         if(min_y>triangle_proj[edge_b].py)min_y=triangle_proj[edge_b].py;
         if(max_y<triangle_proj[edge_b].py)max_y=triangle_proj[edge_b].py;

         if(min_x>triangle_proj[edge_b].px)min_x=triangle_proj[edge_b].px;
         if(max_x<triangle_proj[edge_b].px)max_x=triangle_proj[edge_b].px;

        }

        // rasterize this edge. bresenham fills edge_line with one PixelCord
        // per step along the edge, each carrying its own interpolated z and
        // colour so the scanline fill below can sample them by px position.
        int64_t dx=   triangle_proj[edge_a].px-triangle_proj[edge_b].px;
        int64_t dy=   triangle_proj[edge_a].py-triangle_proj[edge_b].py;

        if(dx<0)dx*=-1;
        if(dy<0)dy*=-1;

        uint64_t edge_len=dx>=dy?(dx+1):(dy+1);
        if(edge_len==1)continue;
        PixelCord* edge_line=calloc(edge_len,sizeof(PixelCord)); // pixel strip for this edge, one PixelCord per bresenham step
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
double scanline_ys [scanline_count];
// each entry is an array of POINTERS into the edge_pixels arrays — no copies,
// so the pointed-to PixelCords keep their original z/colour for interpolation.
PixelCord** scanline_pixels[scanline_count];
uint64_t scanline_slot_cap=((uint64_t)(max_y-min_y))+1;

for(uint64_t row=0;row<scanline_count;row++){
scanline_ys[row]=min_y+row;
scanline_pixels[row]=calloc(scanline_slot_cap,sizeof(PixelCord*));
uint64_t scanline_pixel_count=0;

// scan every edge output; pull out pixels sitting on this row.
for(uint8_t edge_i=0;edge_i<3;edge_i++){
PixelCord* line=edge_pixels[edge_i];

for(uint64_t pixel_i=0;pixel_i<edge_pixel_count[edge_i];pixel_i++){
PixelCord edge_pixel=line[pixel_i];
if(edge_pixel.py==scanline_ys[row]){
    scanline_pixels[row][scanline_pixel_count]=line+pixel_i;
    scanline_pixel_count++;
}

}

}

// left-to-right sort by px so the pair walk below defines contiguous spans.
sort_pixelcords_by_px(scanline_pixels[row], scanline_pixel_count);

// walk consecutive pairs (current,next). the gap between them is the row's
// interior on this side of the triangle — fill it with interpolated pixels.
for(uint64_t pair_i=0;pair_i<scanline_pixel_count;pair_i++){
PixelCord current=*(scanline_pixels[row][pair_i]);

if((pair_i+1)!=scanline_pixel_count){
PixelCord next=*(scanline_pixels[row][pair_i+1]);

if(next.px-current.px!=1){
    size_t fill_i=0;
    for(double fill_x=current.px+1;fill_x<next.px;fill_x++){
        PixelCord* fill_pixel=calloc(1,sizeof(PixelCord));
        *(fill_pixel)=(PixelCord){.py=current.py,.px=fill_x,
        .z=interpolate(current.z,next.z,next.px-current.px,fill_i),
        .colour=interpolate_colour(current.colour,next.colour,next.px-current.px,fill_i),
        .in_use=true
        };
        (*fill_pixel).is_visible=(fill_x>=0&&fill_x<screen_width) && (current.py>=0&&current.py<screen_hieght) && (*fill_pixel).z>=camera.z&&(*fill_pixel).z<=camera.z_end;
        if(!(*fill_pixel).is_visible)continue;
        PixelCord existing=frame_buffer[(uint64_t)(*fill_pixel).py][(uint64_t)(*fill_pixel).px];

        // z-buffer test: only overwrite if this pixel is closer to the camera.
        if(existing.in_use && existing.z<(*fill_pixel).z) continue;
        frame_buffer[(uint64_t)(*fill_pixel).py][(uint64_t)(*fill_pixel).px]=*fill_pixel;
        fill_i++;
    }
}

if(!current.is_visible)continue;
PixelCord existing=frame_buffer[(uint64_t)current.py][(uint64_t)current.px];

// z-buffer test again for the edge pixel itself.
if(existing.in_use && existing.z<current.z) continue;
frame_buffer[(uint64_t)current.py][(uint64_t)current.px]=current;

}


}

}




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
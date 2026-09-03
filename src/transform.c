#include "transform.h"
#include <math.h>

// This renderer's axes: +x right, +y DOWN, +z forward into the screen. The y
// flip is why `down` rather than `up` is the second basis vector -- carrying an
// up vector would mean negating it again at every use.
//
// yaw turns about the vertical axis, positive to the right.
// pitch tilts about the horizontal axis, positive looking up.
// At yaw = pitch = 0 the basis is the identity: right (1,0,0), down (0,1,0),
// forward (0,0,1), which is precisely the camera the rest of the renderer was
// written against.

typedef struct Basis {
    double eye[3];
    double right[3];
    double down[3];
    double fwd[3];
} Basis;

// Rebuilt once per frame by view_refresh() and only read after that, including
// by the rasterization threads.
static Basis view={
    {0.0,0.0,0.0},
    {1.0,0.0,0.0},
    {0.0,1.0,0.0},
    {0.0,0.0,1.0},
};

static void basis_from_angles(double yaw,double pitch,
                              double fwd[3],double right[3],double down[3]){
double sy=sin(yaw),cy=cos(yaw);
double sp=sin(pitch),cp=cos(pitch);

// forward. -sp on y because +y is down: pitching up has to move the
// direction towards negative y.
if(fwd){ fwd[0]=sy*cp;  fwd[1]=-sp;  fwd[2]=cy*cp; }

// right is yaw only, so strafing never drifts vertically no matter how far up
// or down you are looking. it is already unit length.
if(right){ right[0]=cy; right[1]=0.0; right[2]=-sy; }

// down = fwd x right, which keeps the three mutually perpendicular and lands
// on (0,1,0) at zero rotation
if(down){ down[0]=sy*sp; down[1]=cp; down[2]=cy*sp; }
}

void camera_eye(double eye[3]){
Camera c=get_camera_pos();
// x and x_end are the far-plane extents, not the eye: the eye is the centre of
// that box. z is the eye directly.
eye[0]=c.x+(c.x_end-c.x)/2.0;
eye[1]=c.y+(c.y_end-c.y)/2.0;
eye[2]=c.z;
}

void camera_axes(double fwd[3],double right[3],double down[3]){
Camera c=get_camera_pos();
basis_from_angles(c.yaw,c.pitch,fwd,right,down);
}

void view_refresh(void){
Camera c=get_camera_pos();
camera_eye(view.eye);
basis_from_angles(c.yaw,c.pitch,view.fwd,view.right,view.down);
}

const double* view_eye(void){
return view.eye;
}

Vectex view_apply(Vectex v){
double dx=v.x-view.eye[0];
double dy=v.y-view.eye[1];
double dz=v.z-view.eye[2];

// R^T * d, one dot product per axis. The basis is orthonormal, so the
// transpose IS the inverse and there is nothing to invert at runtime.
double vx=view.right[0]*dx+view.right[1]*dy+view.right[2]*dz;
double vy=view.down[0]*dx+view.down[1]*dy+view.down[2]*dz;
double vz=view.fwd[0]*dx+view.fwd[1]*dy+view.fwd[2]*dz;

// put it back at the eye. this is what lets the projection, the near clip and
// the frustum cull keep using the camera box they already have: at zero
// rotation the whole function is the identity.
v.x=view.eye[0]+vx;
v.y=view.eye[1]+vy;
v.z=view.eye[2]+vz;
return v;
}

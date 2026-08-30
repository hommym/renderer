#ifndef RASTERIZATION
#define RASTERIZATION

typedef struct Vectex Vectex;
typedef struct PixelCord PixelCord;

void* rasterization_worker(void* args);
bool is_vectex_visible(Vectex point,PixelCord* pxcord_p);

#endif
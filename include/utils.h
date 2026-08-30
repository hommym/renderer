#ifndef UTILS
#define UTILS
#include <stdlib.h>

#if defined(__unix__) || defined(__APPLE__)
    #include <unistd.h>

#elif defined(_WIN32) || defined(_WIN64)
    #include<windows.h>         
#endif
typedef struct PixelCord PixelCord;

void sort_pixelcords_by_px(PixelCord** arr, size_t n);
int get_number_of_cores();
#endif

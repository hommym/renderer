#include "utils.h"
#include "renderer.h"

void sort_pixelcords_by_px(PixelCord** arr, size_t n){
    for(size_t i=1; i<n; i++){
        PixelCord* key=arr[i];
        size_t j=i;
        while(j>0 && arr[j-1]->px > key->px){
            arr[j]=arr[j-1];
            j--;
        }
        arr[j]=key;
    }
}

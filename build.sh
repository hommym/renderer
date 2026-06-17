# !/bin/bash

Run=$1

cmake --build build
if [ "$Run" == "yes" ]; then
    SDL_VIDEODRIVER=x11 build/renderer
fi

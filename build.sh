# !/bin/bash

Run=$1

cmake --build build
if [ "$Run" == "yes" ]; then
    build/renderer
fi

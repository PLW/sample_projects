#!/bin/zsh

# construct build files
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug 

# run build files
cmake --build . -j


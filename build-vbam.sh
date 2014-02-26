#!/bin/sh
export PATH=/Users/comex/Documents/emsdk_portable/emscripten/incoming/:$PATH
set -xe
rm -rf vbam-build
mkdir vbam-build
cd vbam-build
touch ./-lc # lol
cmake ../vbam -DENABLE_SDL=OFF -DENABLE_GTK=OFF -DENABLE_DEBUGGER=OFF -DENABLE_NLS=OFF -DENABLE_GBA_LOGGING=OFF -DCMAKE_CXX_FLAGS="-DJS -DNO_PNG" -DCMAKE_C_FLAGS="-DJS -DNO_PNG" -DCMAKE_C_COMPILER=emcc -DCMAKE_CXX_COMPILER=em++ -DSDL_LIBRARY=-lc -DOPENGL_gl_LIBRARY=-lc -DOPENGL_glu_LIBRARY=-lc
exec ../rebuild-vbam.sh

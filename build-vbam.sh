#!/bin/sh
export PATH=/Users/comex/Documents/emsdk_portable/emscripten/1.8.2/:$PATH
set -e
rm -rf vbam-build
mkdir vbam-build
cd vbam-build
touch ./-lc # lol
cmake ../vbam -DENABLE_SDL=ON -DENABLE_GTK=OFF -DENABLE_DEBUGGER=OFF -DENABLE_NLS=OFF -DENABLE_GBA_LOGGING=OFF -DCMAKE_CXX_FLAGS="-DJS -DNO_PNG" -DCMAKE_C_FLAGS="-DJS -DNO_PNG" -DCMAKE_C_COMPILER=emcc -DCMAKE_CXX_COMPILER=em++ -DSDL_LIBRARY=-lc -DOPENGL_gl_LIBRARY=-lc -DOPENGL_glu_LIBRARY=-lc
make -j8 VERBOSE=1
ln -f vbam vbam.bc || true
emcc -O2 -o vbam.js vbam.bc --closure 1 -s EXPORTED_FUNCTIONS="['_vbam_js_init', '_vbam_js_main']"
cp vbam.js ../
# -s LEGACY_GL_EMULATION=1

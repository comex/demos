#!/bin/sh
O="$1"
[ -z "$O" ] && O=-O2
export PATH=/Users/comex/Documents/emsdk_portable/emscripten/incoming/:$PATH
set -xe
make -j8 VERBOSE=1
ln -f vbam vbam.bc || true
emcc "$O" -o vbam.js vbam.bc --closure 1 -s EXPORTED_FUNCTIONS="['_vbam_js_init', '_vbam_js_main']"
cp vbam.js ../
# -s LEGACY_GL_EMULATION=1

#!/bin/sh
set -xe
mkdir libwebsockets-build
cd libwebsockets-build
cmake ../libwebsockets -DLWS_WITHOUT_EXTENSIONS=ON -DCMAKE_C_FLAGS="-I.."
make -j8

#!/bin/sh
set -xe
mkdir libwebsockets-build
cd libwebsockets-build
cmake ../libwebsockets -DLWS_WITHOUT_EXTENSIONS=ON -DLWS_WITH_SSL=OFF -DCMAKE_C_FLAGS="-I.."
make -j8

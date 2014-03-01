ifeq ($(wildcard /Users),)
BLOCKS_RUNTIME := -lBlocksRuntime -ldispatch 
endif
keyserver: keyserver.cpp Makefile
	clang++ -o $@ $< -g3 -O2 -Ilibwebsockets/lib -Ilibwebsockets-build libwebsockets-build/lib/libwebsockets.a -std=c++11 -fblocks $(BLOCKS_RUNTIME)

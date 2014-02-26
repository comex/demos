keyserver: keyserver.cpp Makefile
	g++ -o $@ $< -g3 -O2 -Ilibwebsockets/lib -Ilibwebsockets-build libwebsockets-build/lib/libwebsockets.a -std=c++11 

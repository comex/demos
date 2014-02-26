keyserver: keyserver.cpp Makefile
	gcc -o $@ $< -O2 -std=c++11

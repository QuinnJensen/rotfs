all:	rotfs

rotfs:	rotfs.c
	cc -Wall -Wextra -O2 $< -o $@ $$(pkg-config fuse3 --cflags --libs)

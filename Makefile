CC = cc
CFLAGS = -Wall -g $(shell pkg-config --cflags x11 xft fontconfig freetype2)
LDFLAGS = $(shell pkg-config --libs x11 xft fontconfig freetype2) -lm

all: vyg

vyg: vyg.o
	$(CC) -o $@ vyg.o $(LDFLAGS)

vyg.o: vyg.c vyg.h
	$(CC) $(CFLAGS) -c vyg.c

clean:
	rm -f vyg *.o

.PHONY: all clean

CC     = gcc
CFLAGS = -Wall -O2 $(shell sdl2-config --cflags)
LIBS   = $(shell sdl2-config --libs) -lSDL2_ttf -lSDL2_image -lwebsockets -lpthread -lX11

editor: editor.c
	$(CC) $(CFLAGS) -o editor editor.c $(LIBS)

clean:
	rm -f editor

.PHONY: clean

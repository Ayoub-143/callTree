CC      = gcc
CFLAGS  = -O2 -Wall -Wextra
LDFLAGS = -lncurses

.PHONY: all clean

all: calltree calltui

calltree: calltree.c
	$(CC) $(CFLAGS) -o $@ $<

calltree.o: calltree.c calltree.h
	$(CC) $(CFLAGS) -DCALLTREE_LIB -c -o $@ $<

calltui: tui.c calltree.o calltree.h
	$(CC) $(CFLAGS) -o $@ tui.c calltree.o $(LDFLAGS)

clean:
	rm -f calltree calltui calltree.o

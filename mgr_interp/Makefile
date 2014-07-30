CC=gcc
CFLAGS=-std=gnu99   -I/root/ccan   -O3   -W -Wall -Wextra -Wno-unused-parameter -Wshadow   -DDEBUG   -g

LIBS=-lpcap -L/root/ccan -lccan
SRCS=$(wildcard *.c)
OBJS=$(patsubst %.c,%.o,${SRCS})

MAKEDEPEND = gcc -M $(CPPFLAGS) -o $*.d $<

include $(wildcard *.d)

all: mgr_interp

mgr_interp: $(OBJS)
	$(CC) $(CFLAGS) -o mgr_interp $(OBJS) $(LIBS)

clean:
	rm -rf *.o *.d *~

%.o: %.c
	$(COMPILE.c) -MMD -o $@ $<

.PHONY: all clean
.DEFAULT_GOAL=all

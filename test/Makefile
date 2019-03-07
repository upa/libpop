CC = gcc
INCLUDE	:= -I../include
LDFLAGS	:= -L../lib
LDLIBS	:= -lpop -lnetmap
CFLAGS	:= -g -Wall $(INCLUDE)

PROGNAME = test_ctx test_pbuf test_netmap_open test_netmap_write

all: $(PROGNAME)

.c.o:
	$(CC) $< -o $@

clean:
	rm -rf *.o
	rm -rf $(PROGNAME)

CC = gcc
INCLUDE	:= -I../include -I../unvme/src
LDFLAGS	:= -L../lib  -L../unvme/src
LDLIBS	:= -lpop -lnetmap -lunvme
CFLAGS	:= -g -Wall $(INCLUDE)

PROGNAME = test_mem test_pbuf \
	   test_netmap_write test_netmap_read	\
	   test_unvme

all: $(PROGNAME)

.c.o:
	$(CC) $< -o $@

clean:
	rm -rf *.o
	rm -rf $(PROGNAME)

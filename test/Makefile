CC = gcc
INCLUDE	:= -I../include
LDFLAGS	:= -L../lib
LDLIBS	:= -lpop
CFLAGS	:= -g -Wall $(INCLUDE)

PROGNAME = test_ctx

all: $(PROGNAME)

.c.o:
	$(CC) $< -o $@

clean:
	rm -rf *.o
	rm -rf $(PROGNAME)

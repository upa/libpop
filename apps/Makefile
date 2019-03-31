CC = gcc
INCLUDE := -I../include -I../unvme/src
LDFLAGS := -L../lib -L../unvme/src
LDLIBS  := -pthread -lunvme -lpop
CFLAGS  := -g -Wall $(INCLUDE)

PROGNAME = bench-nvme generator store

all: $(PROGNAME)

.c.o:
	$(CC) $< -o $@

clean:
	rm -rf *.o
	rm -rf $(PROGNAME)

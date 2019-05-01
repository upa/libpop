CC = gcc
INCLUDE := -I../include -I../unvme/src
LDFLAGS := -L../lib -L../unvme/src
LDLIBS  := -pthread -lunvme -lpop -lm
CFLAGS  := -O2 -Wall $(INCLUDE) 

PROGNAME = bench-nvme generator store mb

all: $(PROGNAME)

.c.o:
	$(CC) $< -o $@

clean:
	rm -rf *.o
	rm -rf $(PROGNAME)

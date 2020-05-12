CC = gcc
INCLUDE := -I../include -I../unvme/src
LDFLAGS := -L../lib -L../unvme/src
LDLIBS  := -pthread -lunvme -lpop -lm -lnetmap
CFLAGS  := -O1 -march=native -g -Wall $(INCLUDE) -DLIBNETMAP

PROGNAME = bench-nvme generator store mb put_packet nmgen nvgen

all: $(PROGNAME)

.c.o:
	$(CC) $< -o $@


clean:
	rm -rf *.o
	rm -rf $(PROGNAME)


CC = gcc
INCLUDE := -I./ -I../include
CFLAGS := -g -Wall $(INCLUDE) -DPOP_DRIVER_NETMAP
LDL_FLAGS :=

OBJECTS := libpop.o pop_netmap.o

PROGNAME = libpop.a

all: $(PROGNAME)

.c.o:
	$(CC) $(INCLUDE) $(LDL_FLAGS) $(CFLAGS) -c $<

libpop.o: libpop.c libpop_util.h

libpop.a: $(OBJECTS)
	ar rcs libpop.a $(OBJECTS)

clean:
	rm -rf *.o *.a

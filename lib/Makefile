
CC = gcc
INCLUDE := -I./ -I../include
CFLAGS := -g -Wall $(INCLUDE)
LDL_FLAGS :=
LIB :=

PROGNAME = libpop.a

all: $(PROGNAME)

.c.o:
	$(CC) $(INCLUDE) $(LDL_FLAGS) $(CFLAGS) -c $<

libpop.o: libpop.c libpop_util.h

libpop.a: libpop.o 
	ar rcs libpop.a libpop.o

clean:
	rm -rf *.o *.a


CC = gcc
INCLUDE := -I../include
CFLAGS := -g -Wall $(INCLUDE)
LDL_FLAGS :=
LIB :=

PROGNAME = libpop.a

all: $(PROGNAME)

.c.o:
	$(CC) $(INCLUDE) $(LDL_FLAGS) $(CFLAGS) -c $<

libpop.a: libpop.o
	ar rcs libpop.a libpop.o

clean:
	rm -rf *.o *.a

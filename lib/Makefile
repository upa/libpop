
CC = gcc
INCLUDE := -I../include
CFLAGS := -g -Wall $(INCLUDE)
LDL_FLAGS := -fPIC -shared -ldl
LIB :=

PROGNAME = libpop.so

all: $(PROGNAME)

.c.o:
	$(CC) $(INCLUDE) $(LDL_FLAGS) $(CFLAGS) -c $<

libpop.so: libpop.c
	$(CC) $< $(CFLAGS) $(LDL_FLAGS) $(LIB) -o $@

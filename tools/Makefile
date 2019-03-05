
CC = gcc
INCLUDE := -I../include
CFLAGS := -g -Wall $(INCLUDE)

PROGNAME = pop-ioctl

all: $(PROGNAME)

.c.o:
	$(CC) $< -o $@

clean:
	rm -rf *.o
	rm -rf $(PROGNAME)

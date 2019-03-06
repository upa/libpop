#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include <libpop.h>

void usage(void) {
	printf("usage: pop-mmap\n"
	       "    -p path     pop pci device path\n"
	       "    -s size     size of mmap()ed region\n"
	       "    -o offset   offset of mmap\n");

};

int main(int argc, char **argv)
{
	int fd, ch, size, ret = 0;
	size_t offset = 0;
	char *path = NULL;
	void *p2pmem;

	while((ch = getopt(argc, argv, "p:s:o:")) != -1){
		switch(ch) {
		case 'p':
			path = optarg;
			break;

		case 's':
			size = atoi(optarg);
			break;

		case 'o':
			offset = atoi(optarg);
			break;

		default:
			usage();
			return 1;
		}
	}

	fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "failed to open %s\n", path);
		perror("open");
		return -1;
	}

	p2pmem = mmap(0, size, PROT_READ | PROT_WRITE,
		      MAP_LOCKED | MAP_SHARED, fd, offset);
	if (p2pmem == MAP_FAILED) {
		perror("mmap");
		ret = 1;
		goto err_out;
	}

err_out:
	close(fd);
	return ret;
}



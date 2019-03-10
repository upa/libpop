#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include <libpop.h>

void usage(void) {

	printf("usage: mem, testing pop_mem_t\n"
	       "    -b pci    PCI bus slot\n"
	       "    -p port   netmap port\n");
}

int main(int argc, char **argv)
{
	int ch, ret;
	char *pci = NULL;
	char *port = NULL;

#define NUM_BUFS	16
	pop_mem_t mem;
	//pop_buf_t *pbuf[NUM_BUFS];
	pop_driver_t drv;

	/* enable verbose log */
	libpop_verbose_enable();

	while ((ch = getopt(argc, argv, "b:p:")) != -1){

		switch (ch) {
		case 'b' :
			pci = optarg;
			break;
		case 'p':
			port = optarg;
			break;
		default:
			usage();
			return -1;
		}
	}

	/* allocate p2pmem on NoLoad */
	ret = pop_mem_init(&mem, pci);
	if (ret != 0) {
		perror("pop_mem_init");
	}
	assert(ret == 0);

	/* open netmap port */
	ret = pop_driver_init(&drv, POP_DRIVER_TYPE_NETMAP, port);
	if (ret != 0) {
		perror("pop_driver_init");
	}
	assert(ret == 0);

	pop_driver_exit(&drv);
	pop_mem_exit(&mem);

	return 0;
}

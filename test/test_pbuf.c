#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include <libpop.h>

void usage(void) {

	printf("usage: mem, testing pop_mem_t\n"
	       "    -b pci    PCI bus slot\n");
}

int main(int argc, char **argv)
{
	int ch, n;
	char *pci = NULL;

#define NUM_BUFS	4
	pop_mem_t *mem;
	pop_buf_t *pbuf[NUM_BUFS];

	libpop_verbose_enable();

	while ((ch = getopt(argc, argv, "b:")) != -1) {

		switch (ch) {
		case 'b':
			pci = optarg;
			break;
		default:
			usage();
			return 1;
		}
	}

	/* allocate p2pmem on NoLoad */
	mem = pop_mem_init(pci, 0);
	if (!mem)
		perror("pop_mem_init");
	assert(mem);
	
	/* allocate pbuf */

	printf("allocate 4096byte pbuf\n");
	for (n = 0; n < NUM_BUFS; n++) {
		pbuf[n] = pop_buf_alloc(mem, 4096);
		pop_buf_put(pbuf[n], 1 << n);
		pop_buf_pull(pbuf[n], n);

		printf("\nAlloced pbuf %d\n", n);
		print_pop_buf(pbuf[n]);
	}

	printf("\n\n");

	/* allocate pbuf */
	printf("allocate 8192byte pbuf");
	for (n = 0; n < NUM_BUFS; n++) {
		pbuf[n] = pop_buf_alloc(mem, 8192);
		pop_buf_put(pbuf[n], 1 << n);
		pop_buf_pull(pbuf[n], n);

		printf("\nAlloced pbuf %d\n", n);
		print_pop_buf(pbuf[n]);
	}

	/* free pbuf */
	for (n = 0; n < NUM_BUFS; n++) {
		pop_buf_free(pbuf[n]);
	}

	pop_mem_exit(mem);
	return 0;
}

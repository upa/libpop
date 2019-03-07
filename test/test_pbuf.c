#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include <libpop.h>

void usage(void) {

	printf("usage: ctx, testing pop_ctx_t\n"
	       "    -b pci    PCI bus slot\n");
}

int main(int argc, char **argv)
{
	int ch, ret, n;
	char *pci = NULL;

#define NUM_BUFS	8
	pop_ctx_t ctx;
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
	ret = pop_ctx_init(&ctx, pci, 4096 * NUM_BUFS * 2);
	if (ret != 0) {
		perror("pop_ctx_init");
	}
	assert(ret == 0);
	
	/* allocate pbuf */
	for (n = 0; n < NUM_BUFS; n++) {
		pbuf[n] = pop_buf_alloc(&ctx, 4096);
		pop_buf_put(pbuf[n], 1 << n);
		pop_buf_pull(pbuf[n], n);

		printf("\nAlloced pbuf %d\n", n);
		print_pop_buf(pbuf[n]);
	}

	/* free pbuf */
	for (n = 0; n < NUM_BUFS; n++) {
		pop_buf_free(pbuf[n]);
	}

	pop_ctx_exit(&ctx);
}

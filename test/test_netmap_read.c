#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <sched.h>
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <arpa/inet.h>

#include <libpop.h>

void hexdump(void *buf, int len)
{
	int n;
	unsigned char *p = buf;

	printf("Hex dump\n");

	for (n = 0; n < len; n++) {
		printf("%02x", p[n]);

		if ((n + 1) % 2 == 0)
			printf(" ");
		if ((n + 1) % 32 == 0)
			printf("\n");
	}
	printf("\n\n");
}

static int count_online_cpus(void)
{
 	cpu_set_t cpu_set;

	if (sched_getaffinity(0, sizeof(cpu_set_t), &cpu_set) == 0)
		return CPU_COUNT(&cpu_set);

	return -1;
}

void usage(void) {

	printf("usage: ctx, testing pop_ctx_t\n"
	       "    -b pci    PCI bus slot\n"
	       "    -p port   netmap port\n");
}

int main(int argc, char **argv)
{
	int ch, ret, n;
	char *pci = NULL;
	char *port = NULL;
	int cnt = 0;

#define NUM_BUFS	4
	pop_ctx_t ctx;
	pop_buf_t *pbuf[NUM_BUFS];
	pop_driver_t drv;

	/* enable verbose log */
	libpop_verbose_enable();

	while ((ch = getopt(argc, argv, "b:p:l:")) != -1){

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
	ret = pop_ctx_init(&ctx, pci, 4096 * NUM_BUFS);
	if (ret != 0) {
		perror("pop_ctx_init");
	}
	assert(ret == 0);

	/* open netmap port */
	ret = pop_driver_init(&drv, POP_DRIVER_TYPE_NETMAP, port);
	if (ret != 0) {
		perror("pop_driver_init");
	}
	assert(ret == 0);

	/* build packet */
	for (n = 0; n < NUM_BUFS; n++) {
		pbuf[n] = pop_buf_alloc(&ctx, 4096);
		pop_buf_put(pbuf[n], 2048);
	}

	/* recv packet */
	int i, ncpus = count_online_cpus();

	while (1) {
		for (n = 0; n < ncpus; n++) {
			ret = pop_read(&drv, pbuf, NUM_BUFS, n);

			if (ret > 0) {
				printf("pop_read returns %d on queue %d\n",
				       ret, n);
				for (i = 0; i < ret; i++) {
					printf("pkt %d\n", cnt++);
					hexdump(pop_buf_data(pbuf[i]), 128);
				}
			}
		}
		usleep(1);
	}


	pop_driver_exit(&drv);
	pop_ctx_exit(&ctx);

	return 0;
}

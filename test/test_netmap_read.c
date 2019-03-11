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

#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>


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

void usage(void) {

	printf("usage: mem, testing pop_mem_t\n"
	       "    -b pci    PCI bus slot\n"
	       "    -p port   netmap port\n"
	       "    -c count  number of received packet to end\n");
}

int main(int argc, char **argv)
{
	int ch, ret;
	char *pci = NULL;
	char *port = NULL;
	int cnt = -1, received = 0;
	pop_mem_t mem;

	/* enable verbose log */
	libpop_verbose_enable();

	while ((ch = getopt(argc, argv, "b:p:c:")) != -1){

		switch (ch) {
		case 'b' :
			pci = optarg;
			break;
		case 'p':
			port = optarg;
			break;
		case 'c':
			cnt = atoi(optarg);
			break;
		default:
			usage();
			return -1;
		}
	}


	/* allocate p2pmem */
	ret = pop_mem_init(&mem, pci);
	if (ret != 0) {
		perror("pop_mem_init");
	}
	assert(ret == 0);

	/* open netmap port */
	struct nm_desc base_nmd, *d;
	char errmsg[64];
	memset(&base_nmd, 0, sizeof(base_nmd));
	nm_parse(port, &base_nmd, errmsg);

	d = nm_open(port, NULL, NM_OPEN_IFNAME, &base_nmd);
	if (!d) {
		perror("nm_open failed\n");
		return -1;
	}

	/* correlate netmap rxring with p2p memory */
	struct pop_nm_rxring *prxrings[64];
	unsigned int ri;
	for (ri = d->first_rx_ring; ri <= d->last_rx_ring; ri++) {
		struct netmap_ring *ring = NETMAP_RXRING(d->nifp, ri);

		prxrings[ri] = pop_nm_rxring_init(d->fd, ring, &mem);
		if (!prxrings[ri]) {
			perror("pop_nm_rxring_init: failed");
			return -1;
		}
	}

	unsigned int head;
	void *pkt;
	received = 0;
	while (1) {
		ioctl(d->fd, NIOCRXSYNC, NULL);

		for (ri = d->first_rx_ring; ri <= d->last_rx_ring; ri++) {
			struct pop_nm_rxring *prxring = prxrings[ri];
			struct netmap_ring *ring = prxring->ring;
			struct netmap_slot *slot;

			while (!nm_ring_empty(ring)) {
				received++;
				head = ring->head;
				slot = &ring->slot[head];
				pkt = pop_nm_rxring_buf(prxring, head);

				printf("%uth pkt at ring %u, at %p, ptr %lx\n",
				       received, ri, pkt, slot->ptr);
				hexdump(pkt, slot->len);
				head = nm_ring_next(ring, head);
				ring->head = ring->cur = head;
			}
		}
		usleep(1);

		if (cnt && received >= cnt)
			break;
	}

	for (ri = d->first_rx_ring; ri <= d->last_rx_ring; ri++)
		pop_nm_rxring_exit(prxrings[ri]);

	nm_close(d);
	pop_mem_exit(&mem);

	return 0;
}

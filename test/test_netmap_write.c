#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
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


void build_pkt(char *buf, int len, int id) {

	struct ether_header *eth;
	struct ip *ip;
	struct udphdr *udp;

	memset(buf, 0, len);

	eth = (struct ether_header *)buf;
	eth->ether_shost[0] = 0x01;
	eth->ether_shost[1] = 0x02;
	eth->ether_shost[2] = 0x03;
	eth->ether_shost[3] = 0x04;
	eth->ether_shost[4] = 0x05;
	eth->ether_shost[5] = 0x06;

	eth->ether_dhost[0] = 0xff;
	eth->ether_dhost[1] = 0xff;
	eth->ether_dhost[2] = 0xff;
	eth->ether_dhost[3] = 0xff;
	eth->ether_dhost[4] = 0xff;
	eth->ether_dhost[5] = 0xff;

	eth->ether_type = htons(ETHERTYPE_IP);

	ip = (struct ip*)(eth + 1);
	ip->ip_v	= IPVERSION;
	ip->ip_hl       = 5;
	ip->ip_id       = 0;
	ip->ip_tos      = IPTOS_LOWDELAY;
	ip->ip_len      = htons(len - sizeof(*eth));
	ip->ip_off      = 0;
	ip->ip_ttl      = 16;
	ip->ip_p	= IPPROTO_UDP;
	ip->ip_sum      = 0;
	ip->ip_src.s_addr = inet_addr("10.0.0.2");
	ip->ip_dst.s_addr = inet_addr("10.0.0.1");


	udp = (struct udphdr*)(ip + 1);
	udp->uh_ulen    = htons(len - sizeof(*eth) - sizeof(*ip));
	udp->uh_dport   = htons(60000);
	udp->uh_sport   = htons(id + 60000);
	udp->uh_sum     = 0;
}


void usage(void) {

	printf("usage: ctx, testing pop_ctx_t\n"
	       "    -b pci    PCI bus slot\n"
	       "    -p port   netmap port\n"
	       "    -l len    packet length\n"
	       "    -q qid    queue id to xmit\n");
}

int main(int argc, char **argv)
{
	int ch, ret, n, len = 64, qid = 0;
	char *pci = NULL;
	char *port = NULL;

#define NUM_BUFS	4
	pop_ctx_t ctx;
	pop_buf_t *pbuf[NUM_BUFS];
	pop_driver_t drv;

	/* enable verbose log */
	libpop_verbose_enable();

	while ((ch = getopt(argc, argv, "b:p:l:q:")) != -1){

		switch (ch) {
		case 'b' :
			pci = optarg;
			break;
		case 'p':
			port = optarg;
			break;
		case 'l':
			len = atoi(optarg);
			break;
		case 'q':
			qid = atoi(optarg);
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
		pop_buf_put(pbuf[n], len);
		build_pkt(pop_buf_data(pbuf[n]), len, n);
		hexdump(pop_buf_data(pbuf[n]), 128);
	}

	/* xmit packet at bulk */
	ret = pop_write(&drv, pbuf, NUM_BUFS, qid);
	printf("%d packets xmitted\n", ret);

	pop_driver_exit(&drv);
	pop_ctx_exit(&ctx);

	return 0;
}

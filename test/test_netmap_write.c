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

	printf("usage: mem, testing pop_mem_t\n"
	       "    -b pci    PCI bus slot\n"
	       "    -p port   netmap port\n"
	       "    -l len    packet length\n"
	       "    -q qid    queue id to xmit\n"
	       "    -n num    number of packets to be sent\n"
	       "    -D        don't build packet\n");
}

int main(int argc, char **argv)
{
	int ch, n, len = 64, qid = 0;
	int num = 1;
	char *pci = NULL;
	char *port = NULL;
	int dont_build_pkt = 0;

	pop_mem_t *mem;
	pop_buf_t **pbuf;

	/* enable verbose log */
	libpop_verbose_enable();

	while ((ch = getopt(argc, argv, "b:p:l:q:n:D")) != -1){

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
		case 'n':
			num = atoi(optarg);
			break;
		case 'D':
			dont_build_pkt = 1;
			break;
		default:
			usage();
			return -1;
		}
	}
	if (num < 1 || num > 64) {
		fprintf(stderr, "invalid num of packets\n");
		return -1;
	}
	pbuf = calloc(num, sizeof(pop_buf_t));

	/* allocate p2pmem */
	mem= pop_mem_init(pci, 0);
	if (!mem)
		perror("pop_mem_init");
	assert(mem);

	/* open netmap port */
	struct nm_desc base_nmd, *d;
	char errmsg[64];
	memset(&base_nmd, 0, sizeof(base_nmd));
	nm_parse(port, &base_nmd, errmsg);
	base_nmd.req.nr_ringid = qid;
	base_nmd.req.nr_flags = NR_REG_ONE_NIC;

	d = nm_open(port, NULL, NM_OPEN_IFNAME, &base_nmd);
	if (!d) {
		perror("nm_open failed\n");
		return 1;
	}

	/* build packet */
	for (n = 0; n < num; n++) {
		pbuf[n] = pop_buf_alloc(mem, 2048);
		pop_buf_put(pbuf[n], len);
		if (!dont_build_pkt) {
			build_pkt(pop_buf_data(pbuf[n]), len, n);
			hexdump(pop_buf_data(pbuf[n]), 128);
		}
	}

	/* xmit packet at bulk */
	struct netmap_ring *ring = NETMAP_TXRING(d->nifp, qid);
	unsigned int head = ring->head;


	for (n = 0; n < num; n++) {
		struct netmap_slot *slot = &ring->slot[head];
		pop_nm_set_buf(slot, pbuf[n]);
		head = nm_ring_next(ring, head);
	}

	ring->head = ring->cur = head;

	printf("start to send %d packets at a batch\n", num);
	while (nm_tx_pending(ring)) {
                printf("pending=%d\n", nm_tx_pending(ring));
                ioctl(d->fd, NIOCTXSYNC, NULL);
                sleep(1);
        }

	pop_mem_exit(mem);

	return 0;
}

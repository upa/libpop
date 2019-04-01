/* store.c */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <arpa/inet.h>

#include <libpop.h>
#include <unvme.h>

#include "pkt_desc.h"


void build_pkt(void *buf, int len, unsigned int id)
{
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
	udp->uh_sport   = htons(id);
	udp->uh_sum     = 0;

	get_pktlen_from_desc(buf, 2048) = len;
}

void usage(void)
{
	printf("usage: store\n"
	       "    -u pci               nvme slot under unvme\n"
	       "    -l len               packet length\n"
	       "    -b batch             # of batched packet in a write\n"
	       "    -s sltart lba (hex)  start logical block address\n"
	       "    -e end lba (hex)     end logical block address\n"
		);
}

int main(int argc, char **argv)
{
	int ch, ret;
	int pktlen = 64;
	int batch = 512;
	char *nvme = NULL;
	unsigned long lba_start = 0, lba_end = 0, lba;
	const unvme_ns_t *unvme = NULL;
	pop_mem_t *mem;
	pop_buf_t *pbuf;

	while ((ch = getopt(argc, argv, "u:l:b:s:e:")) != -1) {
		switch (ch) {
		case 'u':
			nvme = optarg;
			break;
		case 'l':
			pktlen = atoi(optarg);
			if (pktlen < 64 || pktlen > 1500) {
				printf("invalid pkt len %s\n", optarg);
				return -1;
			}
			break;
		case 'b':
			batch = atoi(optarg);
			if (batch < 0) {
				printf("invalid batch size %s\n", optarg);
				return -1;
			}
			break;
		case 's':
			ret = sscanf(optarg, "0x%lx", &lba_start);
			if (ret < 1) {
				printf("invalid start lba %s\n", optarg);
				return -1;
			}				
			break;
		case 'e':
			ret = sscanf(optarg, "0x%lx", &lba_end);
			if (ret < 1) {
				printf("invalid end lba %s\n", optarg);
				return -1;
			}				
			break;
		default:
			usage();
			return -1;
		}
	}
	
	unvme = unvme_open(nvme);
	if (!unvme) {
		printf("failed to unvme_open(%s)\n", nvme);
		return -1;
	}
	mem = pop_mem_init(NULL, 0);
	unvme_register_pop_mem(mem);

	int b;
	int buflen = 2048 * batch;
	int nblocks = buflen / 512;
	unsigned long npkts = ((lba_end - lba_start) / 4);
	unsigned long num = 0;

	pbuf = pop_buf_alloc(mem, batch * 4 * 512);
	pop_buf_put(pbuf, batch * 4 * 512);

	printf("write packets from 0x%lx to 0x%lx, batch %d (%d blocks)\n",
	       lba_start, lba_end, batch, nblocks);

	for (lba = lba_start; lba < lba_end; lba += nblocks) {

		printf("\r");

		for (b = 0; b < batch; b++) {
			void *pkt = pop_buf_data(pbuf) + 2048 * b;
			build_pkt(pkt, pktlen, lba + b);
			num++;
		}

		ret = unvme_write(unvme, 0, pop_buf_data(pbuf), lba, nblocks);
		if (ret != 0) {
			printf("unvme_write failed on lba 0x%lx\n", lba);
			perror("unvme_write");
			return -1;
		}

		printf("write %ld/%ld packets", num, npkts);
	}
	printf("\n");

	printf("%d-length %ld packets written\n", pktlen, num);

	return 0;
}

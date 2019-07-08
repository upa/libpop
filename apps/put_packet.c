/* put_packet.c */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <err.h>
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <arpa/inet.h>

#include <libpop.h>


void build_pkt(void *buf, int len, unsigned int id)
{
        struct ether_header *eth;
        struct ip *ip;
        struct udphdr *udp;

        //memset(buf, 0, len);

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
        ip->ip_v        = IPVERSION;
        ip->ip_hl       = 5;
        ip->ip_id       = 0;
        ip->ip_tos      = 0;
        ip->ip_len      = htons(len - sizeof(*eth));
        ip->ip_off      = 0;
        ip->ip_ttl      = 16;
        ip->ip_p        = IPPROTO_UDP;
        ip->ip_sum      = 0;
        ip->ip_src.s_addr = inet_addr("10.0.0.2");
        ip->ip_dst.s_addr = inet_addr("10.0.0.1");


        udp = (struct udphdr*)(ip + 1);
        udp->uh_ulen    = htons(len - sizeof(*eth) - sizeof(*ip));
        udp->uh_dport   = htons(60000);
        udp->uh_sport   = htons(id);
        udp->uh_sum     = 0;
}

void copy(void *dst, void *src, unsigned int len)
{
	unsigned int n;
	uint64_t *b;

	for (n = 0; n <= (len >> 3); n++) {
		b = (uint64_t *)(src + (n << 3));
		*((uint64_t *)(dst + (n << 3))) = *b;
	}
}

static uintptr_t phy_addr(void* virt) {
	int fd;
	long pagesize;
	off_t ret;
	ssize_t rc;
	uintptr_t entry = 0;

	fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0)
		err(1, "open /proc/self/pagemap: %s\n", strerror(errno));

	pagesize = sysconf(_SC_PAGESIZE);

	ret = lseek(fd, (uintptr_t)virt / pagesize * sizeof(uintptr_t),
		    SEEK_SET);
	if (ret < 0)
		err(1, "lseek for /proc/self/pagemap: %s\n", strerror(errno));


	rc = read(fd, &entry, sizeof(entry));
	if (rc < 1 || entry == 0)
		err(1, "read for /proc/self/pagemap: %s\n", strerror(errno));

	close(fd);

	return (entry & 0x7fffffffffffffULL) * pagesize +
		   ((uintptr_t)virt) % pagesize;
}


void usage(void) {
	printf("usage:\n"
	       "    -p: PCI slot of p2pmem\n"
	       "    -s: packet size (default 64)\n"
	       "    -n: number of packets to be placed (default 1)\n"
		);
}

int main(int argc, char **argv)
{
	int n, ch;
	int pktlen = 64;
	int pktnum = 1;
	char *pci = NULL;
	pop_mem_t *pmem;
	pop_buf_t *pbuf;
	
	while ((ch = getopt(argc, argv, "p:s:n:h")) != -1){
		switch (ch) {
		case 'p':
			pci = optarg;
			break;
		case 's':
			pktlen = atoi(optarg);
			if (pktlen < 60 || pktlen > 2048) {
				printf("invalid packet size\n");
				return -1;
			}
			break;
		case 'n':
			pktnum = atoi(optarg);
			if (pktnum < 1) {
				printf("invalid packet num\n");
				return -1;
			}
			break;
		case 'h':
		default:
			usage();
			return -1;
		}
	}


	pmem = pop_mem_init(pci, 2048 * pktnum);
	if (!pmem) {
		perror("pop_mem_init");
		return -1;
	}
	pbuf = pop_buf_alloc(pmem, 2048 * pktnum);
	pop_buf_put(pbuf, 2048 * pktnum);
	


	for (n = 0; n < pktnum; n++) {
		void *p = pop_buf_data(pbuf) + (n * 2048);
		printf("put %d byte packet to %p, %#lx\n",
		       pktlen, p, phy_addr(p));
		build_pkt(p, pktlen, n);
	}

	return 0;
}

/* nmgen.c */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sched.h>
#include <signal.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <net/ethernet.h>
#include <linux/if_ether.h>

#include <libpop.h>

#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>
#include <libnetmap.h>


static int caught_signal = 0;

void sig_handler(int sig)
{
	if (sig == SIGINT)
		caught_signal = 1;
}

int count_online_cpus(void)
{
	cpu_set_t cpu_set;
	if (sched_getaffinity(0, sizeof(cpu_set_t), &cpu_set) == 0)
		return CPU_COUNT(&cpu_set);
	return -1;
}

#define NMGEN_MODE_TX	0
#define NMGEN_MODE_RX	1

#define MAX_BATCH_NUM	64
#define MAX_CPU_NUM	32

struct nmgen {
	char	*port;	/* netmap port	*/
	char	*pci;
	int	mode;	/* NMGEN_MODE_TX or NMGEN_MODE_RX */

	int	pktlen;	/* packet length	*/
	int	ncpus;	/* number of cpus to be used	*/
	int	batch;	/* batch size	*/

	float	interval;	/* report interval (usec) */
	int	timeout;

	struct in_addr dstip, srcip;
	uint8_t dstmac[ETH_ALEN], srcmac[ETH_ALEN];

	pop_mem_t	*mem;	/* pop memory */
};

struct nmgen_thread {
	struct nmgen	*gen;

	pthread_t	tid;
	int		cpu;

#ifdef LIBNETMAP
	struct nmport_d *nmd;
#else
	struct nm_desc	*nmd;	/* nm_desc for this thread	*/
#endif /* LIBNETMAP */

	int		cancel;
	unsigned long	npkts;	/* packet counter	*/
	unsigned long	nbits;	/* byte counter	*/
};


/* from netmap pkt-gen.c */
static uint16_t
checksum (const void *data, uint16_t len, uint32_t sum)
{
	const uint8_t *addr = data;
	uint32_t i;

	for (i = 0; i < (len & ~1U); i += 2) {
		sum += (u_int16_t)ntohs(*((u_int16_t *)(addr + i)));
		if (sum > 0xFFFF)
			sum -= 0xFFFF;
	}

	if (i < len) {
		sum += addr[i] << 8;
		if (sum > 0xFFFF)
			sum -= 0xFFFF;
	}

	return sum;
}

static u_int16_t
wrapsum (u_int32_t sum)
{
	sum = ~sum & 0xFFFF;
	return (htons(sum));
}

void build_packet(struct nmgen_thread *th, void *pkt)
{
	struct nmgen *gen = th->gen;
	struct ether_header *eth;
	struct udphdr *udp;
	struct ip *ip;

	memset(pkt, 0, gen->pktlen);

	eth = (struct ether_header *)pkt;
	memcpy(eth->ether_dhost, gen->dstmac, ETH_ALEN);
	memcpy(eth->ether_shost, gen->srcmac, ETH_ALEN);
	eth->ether_type = htons(ETHERTYPE_IP);

	ip = (struct ip*)(eth + 1);
	ip->ip_v	= IPVERSION;
	ip->ip_hl	= 5;
	ip->ip_id	= 0;
	ip->ip_tos	= IPTOS_LOWDELAY;
	ip->ip_len	= htons(gen->pktlen - sizeof(*eth));
	ip->ip_off	= 0;
	ip->ip_ttl	= 16;
	ip->ip_p	= IPPROTO_UDP;
	ip->ip_sum	= 0;
	ip->ip_src	= gen->srcip;
	ip->ip_dst	= gen->dstip;
	ip->ip_sum	= wrapsum(checksum(ip, sizeof(*ip), 0));

	udp = (struct udphdr *)(ip + 1);
	udp->uh_ulen	= htons(gen->pktlen - sizeof(*eth) - sizeof(*ip));
	udp->uh_dport	= htons(60000 + th->cpu);
	udp->uh_sport	= rand() & 0xFFFF;
	udp->uh_sum	= 0;
}

void *nmgen_sender_body(void *arg)
{
	unsigned long npkts, nbits, head, space, batch, b;
	double elapsed;
	struct timeval start, end;
	struct nmgen_thread *th = arg;
	struct nmgen *gen = th->gen;
	struct netmap_ring *ring;
	cpu_set_t target_cpu_set;
	pop_buf_t *pbuf;
	void *pkts[MAX_BATCH_NUM];
	uintptr_t pkts_phy[MAX_BATCH_NUM];	/* phy addr of packets */
	int n;

	/* pin this thread on the cpu */
	CPU_ZERO(&target_cpu_set);
	CPU_SET(th->cpu, &target_cpu_set);
	pthread_setaffinity_np(th->tid, sizeof(cpu_set_t), &target_cpu_set);

	/* allcoate and build packet buffer from libpop memory */
	pbuf = pop_buf_alloc(gen->mem, 2048 * gen->batch);
	if (!pbuf) {
		fprintf(stderr, "pop_buf_alloc() on cpu %d: %s\n",
			th->cpu, strerror(errno));
		goto out;
	}
	pop_buf_put(pbuf, 2048 * gen->batch);
	for (n = 0; n < gen->batch; n++) {
		pkts[n] = pop_buf_data(pbuf) + 2048 * n;
		pkts_phy[n]= pop_virt_to_phys(gen->mem, pkts[n]);
		build_packet(th, pkts[n]);
	}


	ring = NETMAP_TXRING(th->nmd->nifp, th->cpu);

	printf("start xmit loop on cpu %d, fd %d\n", th->cpu, th->nmd->fd);

	gettimeofday(&start, NULL);

	/* xmit loop */
	while (!th->cancel) {
		npkts = 0;
		nbits = 0;

		if (ioctl(th->nmd->fd, NIOCTXSYNC, NULL) < 0) {
			fprintf(stderr, "ioctl error on cpu %d: %s\n",
				th->cpu, strerror(errno));
			goto out;
		}

		space = nm_ring_space(ring);
		if (!space)
			continue;
		batch = space > gen->batch ? gen->batch : space;
		head = ring->head;

		for (b = 0; b < batch; b++) {
			struct netmap_slot *slot = &ring->slot[head];
			slot->flags |= NS_PHY_INDIRECT;
			slot->ptr = pkts_phy[b];
			slot->len = gen->pktlen;

			npkts++;
			nbits += (gen->pktlen << 3);

			head = nm_ring_next(ring, head);
		}

		ring->head = ring->cur = head;

		th->npkts += npkts;
		th->nbits += nbits;
	}

	while (nm_tx_pending(ring)) {
		ioctl(th->nmd->fd, NIOCTXSYNC, NULL);
		usleep(1);
	}

	gettimeofday(&end, NULL);

	elapsed = end.tv_sec * 1000000 + end.tv_usec;
	elapsed -= (start.tv_sec * 1000000 + start.tv_usec);
	elapsed /= 1000000;	/* sec */

	printf("CPU=%d %.2f pps, %.2f Mpps, %.2f bps, %.2f Mbps\n", th->cpu,
	       th->npkts / elapsed, th->npkts / elapsed / 1000000,
	       th->nbits / elapsed, th->nbits / elapsed / 1000000);

out:
	return NULL;
}

void *nmgen_receiver_body(void *arg)
{
	unsigned long npkts, nbits, head;
	double elapsed;
	struct timeval start, end;
	struct pop_nm_rxring *prxring;
	struct nmgen_thread *th = arg;
	struct nmgen *gen = th->gen;
	struct netmap_ring *ring;
	struct netmap_slot *slot;
	cpu_set_t target_cpu_set;

	/* pin this thread on the cpu */
	CPU_ZERO(&target_cpu_set);
	CPU_SET(th->cpu, &target_cpu_set);
	pthread_setaffinity_np(th->tid, sizeof(cpu_set_t), &target_cpu_set);

	/* correlate netmap rxring with pop memory */
	ring = NETMAP_RXRING(th->nmd->nifp, th->cpu);
	prxring = pop_nm_rxring_init(th->nmd->fd, ring, gen->mem);

	printf("start recv loop on cpu %d, fd %d\n", th->cpu, th->nmd->fd);

	gettimeofday(&start, NULL);

	/* recv loop */
	while (!th->cancel) {
		npkts = 0;
		nbits = 0;

		if (ioctl(th->nmd->fd, NIOCRXSYNC, NULL) < 0) {
			fprintf(stderr, "ioctl error on cpu %d: %s\n",
				th->cpu, strerror(errno));
			goto out;
		}


		while (!nm_ring_empty(ring)) {
			head = ring->head;
			slot = &ring->slot[head];

			npkts++;
			nbits += (slot->len << 3);

			head = nm_ring_next(ring, head);
			ring->head = ring->cur = head;
		}

		th->npkts += npkts;
		th->nbits += nbits;
	}

	gettimeofday(&end, NULL);

	elapsed = end.tv_sec * 1000000 + end.tv_usec;
	elapsed -= (start.tv_sec * 1000000 + start.tv_usec);
	elapsed /= 1000000;	/* sec */

	printf("CPU=%d %.2f pps, %.2f Mpps, %.2f bps, %.2f Mbps\n", th->cpu,
	       th->npkts / elapsed, th->npkts / elapsed / 1000000,
	       th->nbits / elapsed, th->nbits / elapsed / 1000000);

out:
	return NULL;
}

void *count_thread(void *arg)
{
	struct nmgen_thread *ths = arg;
	struct nmgen *gen = ths[0].gen;
	unsigned long npkts_b[MAX_CPU_NUM], npkts_a[MAX_CPU_NUM];
	unsigned long nbits_b[MAX_CPU_NUM], nbits_a[MAX_CPU_NUM];
	double pps, bps, elapsed;
	struct timeval b, a;
	cpu_set_t cpu_set;
	int cpu, n;


	/* pin this thread on the last cpu */
	cpu = count_online_cpus() -1;
	CPU_ZERO(&cpu_set);
	CPU_SET(cpu, &cpu_set);
	pthread_setaffinity_np(pthread_self(), sizeof(cpu_set), &cpu_set);

	printf("start count thread on cpu %d\n", cpu);

	while (!caught_signal) {

		/* gather counters */
		for (n = 0; n < gen->ncpus; n++) {
			npkts_b[n] = ths[n].npkts;
			nbits_b[n] = ths[n].nbits;
		}

		gettimeofday(&b, NULL);
		usleep(gen->interval);
		gettimeofday(&a, NULL);

		for (n = 0; n < gen->ncpus; n++) {
			npkts_a[n] = ths[n].npkts;
			nbits_a[n] = ths[n].nbits;
		}

		
		/* totaling the counters */
		for (pps = 0, bps = 0, n = 0; n < gen->ncpus; n++) {
			pps += npkts_a[n] - npkts_b[n];
			bps += nbits_a[n] - nbits_b[n];
		}
		     
		/* elapsed tie is usec */
		elapsed  =  a.tv_sec * 1000000 + a.tv_usec;
		elapsed -= (b.tv_sec * 1000000 + b.tv_usec);
		elapsed /= 1000000;	/* usec -> sec */

		if (caught_signal)
			break;

		printf("[INTERIM] %.2f pps, %.2f Mpps, %.2f bps, %.2f Mbps\n",
		       pps / elapsed, pps / elapsed / 1000000,
		       bps / elapsed, bps / elapsed / 1000000);
	}
	
	/* stop sender threads */
	printf("\n");
	for (n = 0; n < gen->ncpus; n++)
		ths[n].cancel = 1;

	return NULL;
}

void print_nmgen_info(struct nmgen *gen)
{
	char buf[32];

	printf("================ nmgen ================\n");
	printf("netmap port (-p):    %s\n", gen->port);
	printf("pci (-P):            %s\n", gen->pci ? gen->pci : "hugepage");
	printf("mode (-m):           %s\n",
	       gen->mode == NMGEN_MODE_TX ? "tx" : "rx");
	printf("pktlen (-l):         %d\n", gen->pktlen);
	printf("ncpus (-n):          %d\n", gen->ncpus);
	printf("batch (-b):          %d\n", gen->batch);
	printf("interval (-i):       %f\n", gen->interval / 1000000);
	printf("timeout (-t):        %d\n", gen->timeout);

	inet_ntop(AF_INET, &gen->dstip, buf, sizeof(buf));
	printf("dstip (-d):          %s\n", buf);
	inet_ntop(AF_INET, &gen->srcip, buf, sizeof(buf));
	printf("srcip (-s):          %s\n", buf);

	printf("dstmac (-D):         %02x:%02x:%02x:%02x:%02x:%02x\n",
	       gen->dstmac[0], gen->dstmac[1], gen->dstmac[2],
	       gen->dstmac[3], gen->dstmac[4], gen->dstmac[5]);
	printf("srcmac (-S):         %02x:%02x:%02x:%02x:%02x:%02x\n",
	       gen->srcmac[0], gen->srcmac[1], gen->srcmac[2],
	       gen->srcmac[3], gen->srcmac[4], gen->srcmac[5]);
	printf("=======================================\n");
}

void usage(void) {
	printf("\nusage: nmge\n"
	       "\n"
	       "    -p port           netmap port\n"
	       "    -P pci            pop memory slot or 'hugepage'\n"
	       "    -m tx/rx          direction\n"
	       "\n"
	       "    -l pktlen         packet length\n"
	       "    -n ncpus          number of CPUs to be used\n"
	       "    -b batch          batch size\n"
	       "\n"
	       "    -i interval       report interval\n"
	       "    -t timeout        timeout to stop\n"
	       "\n"
	       "    -d dstip          destination IPv4 address\n"
	       "    -s srcip          source IPv4 address\n"
	       "    -D dstmac         destination MAC address\n"
	       "    -S srcmac         source MAC address\n"
	       "\n"
		);
}


int main(int argc, char **argv)
{
	struct nmgen_thread ths[MAX_CPU_NUM];
	struct nmgen gen;
	pthread_t ctid;
	int ch, n;

	srand((unsigned)time(NULL));

	memset(&gen, 0, sizeof(gen));
	gen.mode = NMGEN_MODE_TX;
	gen.pktlen = 60;
	gen.ncpus = 1;
	gen.batch = 1;
	gen.interval = 1000000;
	gen.timeout = 0;
	inet_pton(AF_INET, "10.0.10.1", &gen.dstip);
	inet_pton(AF_INET, "10.0.10.2", &gen.srcip);
	memset(gen.dstmac, 0xFF, ETH_ALEN);

	while ((ch = getopt(argc, argv, "p:P:m:l:n:b:i:t:d:s:D:S:h")) != -1) {
		switch (ch) {
		case 'p':
			gen.port = optarg;
			break;
		case 'P':
			if (strncmp(optarg, "hugepage", 8) == 0)
				gen.pci = NULL;
			else
				gen.pci = optarg;
			break;
		case 'm':
			if (strncmp(optarg, "tx", 2) == 0)
				gen.mode = NMGEN_MODE_TX;
			else if (strncmp(optarg, "rx", 2) == 0)
				gen.mode = NMGEN_MODE_RX;
			else {
				fprintf(stderr, "invalid mode %s\n", optarg);
				return -1;
			}
			break;
		case 'l':
			gen.pktlen = atoi(optarg);
			break;
		case 'n':
			gen.ncpus = atoi(optarg);
			if (gen.ncpus > MAX_CPU_NUM) {
				fprintf(stderr, "too many cpus (> %d)\n",
					MAX_CPU_NUM);
				return -1;
			}
			break;
		case 'b':
			gen.batch = atoi(optarg);
			if (gen.batch > MAX_BATCH_NUM) {
				fprintf(stderr, "too large batch(> %d)\n",
					MAX_BATCH_NUM);
				return -1;
			}
			break;
		case 'i':
			sscanf(optarg, "%f", &gen.interval);
			gen.interval *= 1000000;
			break;
		case 't':
			gen.timeout = atoi(optarg);
			break;
		case 'd':
			inet_pton(AF_INET, optarg, &gen.dstip);
			break;
		case 's':
			inet_pton(AF_INET, optarg, &gen.srcip);
			break;
		case 'D':
			sscanf(optarg, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
			       &gen.dstmac[0], &gen.dstmac[1], &gen.dstmac[2],
			       &gen.dstmac[3], &gen.dstmac[4], &gen.dstmac[5]);
			break;
		case 'S':
			sscanf(optarg, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
			       &gen.srcmac[0], &gen.srcmac[1], &gen.srcmac[2],
			       &gen.srcmac[3], &gen.srcmac[4], &gen.srcmac[5]);
			break;
		case 'h':
		default:
			usage();
			return -1;
		}
	}
	
	if (!gen.port) {
		fprintf(stderr, "-p port must be specified\n");
		return -1;
	}

	/* rx needs all cpus to check all queues (skimped work) */
	if (gen.mode == NMGEN_MODE_RX)
		gen.ncpus = count_online_cpus();

	print_nmgen_info(&gen);

	gen.mem = pop_mem_init(gen.pci, 0);
	if (!gen.mem) {
		fprintf(stderr, "pop_mem_init(%s): %s\n",
			gen.pci, strerror(errno));
		return -1;
	}

	if (signal(SIGINT, sig_handler) == SIG_ERR) {
		perror("signal");
		return -1;
	}

	
	/* initialize netmap ports  */
	for (n = 0; n < gen.ncpus; n++) {
		struct nmgen_thread *th = &ths[n];

		memset(th, 0, sizeof(*th));
		th->gen = &gen;
		th->cpu = n;

#ifdef LIBNETMAP
		if (n == 0) {
			th->nmd = nmport_prepare(gen.port);
			if (!th->nmd) {
				fprintf(stderr, "nmport_preapre failed\n");
				return -1;
			}
			th->nmd->reg.nr_mode = NR_REG_ONE_NIC;
			if (nmport_open_desc(th->nmd) < 0) {
				fprintf(stderr, "nmport_open_desc failed\n");
				return -1;
			}

		} else {
			th->nmd = nmport_clone(ths[0].nmd);
			th->nmd->reg.nr_ringid = n;
			if (nmport_open_desc(th->nmd)< 0) {
				fprintf(stderr,
					"nmport_open_desc for %d failed", n);
				return -1;
			}
		}
#else
		if (n == 0) {
			/* initialize the first netmap port on queue 0*/
			struct nm_desc base_nmd;
			char errmsg[64];
			int flags;
			int ret;
			memset(&base_nmd, 0, sizeof(base_nmd));
			ret = nm_parse(gen.port, &base_nmd, errmsg);
			if (ret) {
				fprintf(stderr, "nm_parse %s failed: %s\n",
					gen.port, errmsg);
				perror("nm_parse");
				return -1;
			}

			flags = NM_OPEN_IFNAME | NM_OPEN_ARG1 | NM_OPEN_ARG2 |
				NM_OPEN_ARG3 | NM_OPEN_RING_CFG;
			base_nmd.req.nr_flags &= ~NR_REG_MASK;
			base_nmd.req.nr_flags |= NR_REG_ONE_NIC;
			base_nmd.req.nr_ringid = 0;
			th->nmd = nm_open(gen.port, NULL, flags, &base_nmd);
		} else {
			/* initialize queues next to 0 */
			struct nm_desc nmd = *ths[0].nmd;
			nmd.req.nr_flags = (ths[0].nmd->req.nr_flags &
					    ~NR_REG_MASK);
			nmd.req.nr_flags |= NR_REG_ONE_NIC;
			nmd.req.nr_ringid = n;
			th->nmd = nm_open(gen.port, NULL, NM_OPEN_NO_MMAP,
					  &nmd);
		}

		if (!th->nmd) {
			fprintf(stderr, "nm_open for %d failed: %s\n", n,
				strerror(errno));
			return -1;
		}
#endif
	}


	/* spawn the threads */
	for (n = 0; n < gen.ncpus; n++) {
		switch (gen.mode) {
		case NMGEN_MODE_TX:
			pthread_create(&ths[n].tid, NULL,
				       nmgen_sender_body, &ths[n]);
			break;
		case NMGEN_MODE_RX:
			pthread_create(&ths[n].tid, NULL,
				       nmgen_receiver_body, &ths[n]);
			break;
		}

		usleep(1000);	/* 1msec */
	}

	/* spwan counter thread */
	pthread_create(&ctid, NULL, count_thread, ths);

	if (gen.timeout) {
		sleep(gen.timeout);
		caught_signal = 1;
	}

	/* join the threads */
	pthread_join(ctid, NULL);
	for (n = 0; n < gen.ncpus; n++)
		pthread_join(ths[n].tid, NULL);

	pop_mem_exit(gen.mem);

	return 0;
}
	

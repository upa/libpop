/* generator.c */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sched.h>
#include <pthread.h>
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <arpa/inet.h>

#include <libpop.h>

#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>
#include <unvme.h>

#include "pkt_desc.h"

#define MAX_CPUS		32
#define MAX_BATCH_SIZE		64
#define MAX_NMPORT_NAME		64

#define WALK_MODE_SEQ  		0
#define WALK_MODE_RANDOM	1

#define NM_BATCH_TO_NBLOCKS(b, u)					\
	((b) << 11 >= (u)->blocksize ? (((b) << 11) >> (u)->blockshift) : 1)
/* if batch num * 2048 is smaller than block size, use 1 */



static const char *walk_mode_string[] = {
	"seq", "random"
};

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

#define printv1(fmt, ...) if (gen.verbose >= 1) \
		fprintf(stdout, fmt, ##__VA_ARGS__)
#define printv2(fmt, ...) if (gen.verbose >= 2) \
		fprintf(stdout, "%s: " fmt, __func__, ##__VA_ARGS__)
#define printv3(fmt, ...) if (gen.verbose >= 3) \
		fprintf(stdout, "%s: " fmt, __func__, ##__VA_ARGS__)

/* structure describing this program */
struct generator {

	/* arguments */
	char	*pci;	/* p2pmem slot	*/
	char	*nvme;	/* nvme slot	*/
	char	*port;	/* netmap port	*/
	int	ncpus;	/* # of cpus to be used	*/
	int	batch;	/* # of batch	*/
	int	walk;	/* walk mode	*/
	unsigned long	lba_start, lba_end;	/* start and end of slba */

	/* variables shared among threads */
	const unvme_ns_t	*unvme;	/* unvme */
	pop_mem_t	*mem;	/* pop memory */
	
	/* misc */
	pthread_t	tid_cnt;	/* tid for count thread */
	int	fake_packet;	/* use fake packet instead of nvme */
	int	hex_dump;	/* print hexdump of read packet */
	int	interval;	/* usec */
	int	verbose;	/* verbose level */
	int	timeout;	/* timeout to end */

} gen;

/* structure describing a thread */
struct gen_thread {
	pthread_t	tid;	/* pthread tid */
	int	cpu;		/* cpu num on which this thread runs */
	char	nmport[MAX_NMPORT_NAME];	/* netmap:foo-NN */

	struct nm_desc	*nmd;	/* nm_desc no this thread */

	unsigned long	lba_start, lba_end;	/* lba range for this thread*/

	unsigned long	npkts;	/* number of packets TXed */
	unsigned long	nbytes;	/* number of bytes TXed */
	unsigned long	nbytes_nvme;	/* number of bytes nvme read */
	unsigned long	no_slot;	/* number of no netmap slot cases */

	struct timeval	start, end;	/* start and end time */
} gen_th[MAX_CPUS];


void print_gen_info(void)
{
	printf("============= generator =============\n");
	printf("p2pmem (-p):    %s\n", gen.pci);
	printf("unvme (-u):     %s\n", gen.nvme);
	printf("port (-i):      %s\n", gen.port);
	printf("ncpus (-n):     %d\n", gen.ncpus);
	printf("batch (-b):     %d\n", gen.batch);
	printf("walk (-w):      %s\n", walk_mode_string[gen.walk]);
	printf("start lba (-s): 0x%lx\n", gen.lba_start);
	printf("end lba (-e):   0x%lx\n", gen.lba_end);
	printf("interval (-I):  %d\n", gen.interval);
	printf("timeout (-T):   %d\n", gen.timeout);
	printf("\n");
	printf("nblocks in a nvme cmd: %d (%d byte, %u byte block)\n",
	       NM_BATCH_TO_NBLOCKS(gen.batch, gen.unvme),
	       NM_BATCH_TO_NBLOCKS(gen.batch, gen.unvme) <<
	       gen.unvme->blockshift,
	       gen.unvme->blocksize);
	printf("=====================================\n");
}

void usage(void)
{
	printf("usage: generator\n"
	       "    -p pci               p2pmem slot, none means hugepage\n"
	       "    -u pci               nvme slot under unvme\n"
	       "    -i port              network interface name\n"
	       "    -n ncpus             number of cpus\n"
	       "    -b batch             batch size in a netmap iteration\n"
	       "    -w walk mode         seq or random\n"
	       "    -s start lba (hex)   start logical block address\n"
	       "    -e end lba (hex)     end logical block address\n"
	       "    -I interval (msec)   interval between each xmit\n"
	       "    -F                   use fake packet instead of read from nvme\n"
	       "    -H                   print hexdump of nvme read\n"
	       "    -T time (sec)        timeout\n"
	       "    -v                   verbose mode\n"
		);
}

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

unsigned long next_lba(unsigned long lba,
		       unsigned long lba_start, unsigned long lba_end,
		       unsigned long nblocks)
{
	switch (gen.walk) {
	case WALK_MODE_SEQ:
		/* XXX: netmap_slot is 2048 byte, compised of
		 * 4 blocks on nvme. */
		if ((lba + nblocks) < lba_end)
			return lba + nblocks;
		else
			return lba_start;
		break;
	case WALK_MODE_RANDOM:
		/* align in 4 blocks */
		return ((rand() % (lba_end - lba_start - nblocks)) + lba_start)
			<< 2;
		break;
	}

	/* not reached */
	return 0;
}


void *thread_body(void *arg)
{
	int n, ret;
	struct gen_thread *th = arg;
	int qid = th->cpu;
	cpu_set_t target_cpu_set;
	pop_buf_t *buf;
	void *pkts[MAX_BATCH_SIZE];
	unvme_iod_t iod;
	unsigned long lba, batch, nblocks, b, head, nbytes, npkts, space;
	struct netmap_ring *ring = NETMAP_TXRING(th->nmd->nifp, th->cpu);

	/* pin this thread on the cpu */
	CPU_ZERO(&target_cpu_set);
	CPU_SET(th->cpu, &target_cpu_set);
	pthread_setaffinity_np(th->tid, sizeof(cpu_set_t), &target_cpu_set);

	/* allocate packet buffer. we use a single pop_buf for
	 * multiple packet buffers. It enalbes us to get all batched
	 * packets from nvme in a single read command */
	buf = pop_buf_alloc(gen.mem, 2048 * gen.batch);
	pop_buf_put(buf, 2048 * gen.batch);
	for (n = 0; n < gen.batch; n++)
		pkts[n] = pop_buf_data(buf) + 2048 * n;

	/* initialize the start LBA */
	lba = th->lba_start;

	printf("THREAD: qid %d, port %s, lba 0x%lx-0x%lx, pbuf 0x%lx start\n",
	       qid, th->nmport, th->lba_start, th->lba_end,
	       pop_buf_paddr(buf));
	gettimeofday(&th->start, NULL);

	while (!caught_signal) {

		npkts = 0;
		nbytes = 0;
		nblocks = 0;

		space = nm_ring_space(ring);
		batch = space > gen.batch ? gen.batch : space;

		if (batch == 0) {
			printv3("no space left on %d\n", th->cpu);
			printv3("head=%u cur=%u tail=%u\n",
				ring->head, ring->cur, ring->tail);
			th->no_slot++;
			goto sync_out;
		}
		head = ring->head;

		/* execute a single read command for all packets 
		 * in this batch. */
		if (!gen.fake_packet) {
			nblocks = NM_BATCH_TO_NBLOCKS(batch, gen.unvme);
			iod = unvme_aread(gen.unvme, qid, pop_buf_data(buf),
					  lba, nblocks);
			lba = next_lba(lba, th->lba_start, th->lba_end,
				       nblocks);
		}

		/* fill the netmap_slot with the packet info in
		 * this pop buf region */
		for (b = 0; b < batch; b++) {
			void *pkt = pkts[b];
			struct netmap_slot *slot = &ring->slot[head];

			if (!pkt) {
				printv3("pkt is null, "
					"b=%lu, batch=%lu, "
					"gen.batch=%u cpu=%d\n",
					b, batch, gen.batch, th->cpu);
			}

			if (gen.fake_packet) {
				build_pkt(pkt, 1500, b);
				get_pktlen_from_desc(pkt, 2048) = 1500;
			}

			slot->flags |= NS_PHY_INDIRECT;
			slot->ptr = pop_virt_to_phys(gen.mem, pkt);
			slot->len = get_pktlen_from_desc(pkt, 2048);

			head = nm_ring_next(ring, head);

			npkts++;
			nbytes += get_pktlen_from_desc(pkt, 2048);

			printv2("CPU=%d LBA=0x%lx head=%lu pkt=%p\n",
				th->cpu, lba, head, pkt);

		}

		if (!gen.fake_packet) {
			ret = unvme_apoll(iod, UNVME_TIMEOUT);
			if (ret != 0) {
				printv3("unvme_apoll timeout on cpu %d\n",
					th->cpu);
				continue;
			}
		}

		if (gen.hex_dump) {
			int i;
			for (i = 0; i < batch; i++) {
				printf("%d of batch %lu\n", i, batch);
				hexdump(pkts[i], 256);
			}
		}

		/* ok, an nvme read command finished, and all netmap
		 * slots for this iteration are filled. Advance the
		 * cur and call ioctl() to xmit packets */
		ring->head = ring->cur = head;

	sync_out:
		if (batch < gen.batch) {
			/* tell netmap that we need more slots */
			ring->cur = ring->tail;
		}

		if (ioctl(th->nmd->fd, NIOCTXSYNC, NULL) < 0) {
			printf("TXSYNC error on cpu %d\n", th->cpu);
			perror("ioctl(TXSYNC)");
			break;
		}
		printv3("TXSYNC on cpu %d\n", th->cpu);

		th->npkts += npkts;
		th->nbytes += nbytes;
		th->nbytes_nvme += nblocks << gen.unvme->blockshift;

		if (gen.interval)
			usleep(gen.interval);
	}

	gettimeofday(&th->end, NULL);

	/* xmit pending packets */
	/*
	while (nm_tx_pending(ring)) {
		ioctl(th->nmd->fd, NIOCTXSYNC, NULL);
		usleep(1);
	}
	*/

	return NULL;
}

void *count_thread(void *arg)
{
	int n, cpu;
	cpu_set_t target_cpu_set;
	unsigned long pps, npkts_before[MAX_CPUS], npkts_after[MAX_CPUS];
	unsigned long bps, nbytes_before[MAX_CPUS], nbytes_after[MAX_CPUS];
	unsigned long bpsn, nbytesn_before[MAX_CPUS], nbytesn_after[MAX_CPUS];
	unsigned long ns, no_slot_before[MAX_CPUS], no_slot_after[MAX_CPUS];

	/* pin this thread on the last cpu */
	cpu = count_online_cpus() - 1;
	CPU_ZERO(&target_cpu_set);
	CPU_SET(cpu, &target_cpu_set);
	pthread_setaffinity_np(gen.tid_cnt, sizeof(target_cpu_set),
			       &target_cpu_set);
	printf("count thread on cpu %d\n", cpu);

	while (1) {
		if (caught_signal)
			break;

		/* gather counters */
		for (n = 0; n < gen.ncpus; n++) {
			npkts_before[n] = gen_th[n].npkts;
			nbytes_before[n] = gen_th[n].nbytes;
			nbytesn_before[n] = gen_th[n].nbytes_nvme;
			no_slot_before[n] = gen_th[n].no_slot;
		}

		sleep(1);

		for (n = 0; n < gen.ncpus; n++) {
			npkts_after[n] = gen_th[n].npkts;
			nbytes_after[n] = gen_th[n].nbytes;
			nbytesn_after[n] = gen_th[n].nbytes_nvme;
			no_slot_after[n] = gen_th[n].no_slot;
		}

		/* totaling the counters */
		for (pps = 0, bps = 0, bpsn = 0, ns = 0,
			n = 0; n < gen.ncpus; n++) {
			pps += npkts_after[n] - npkts_before[n];
			bps += (nbytes_after[n] - nbytes_before[n]) * 8;
			bpsn += nbytesn_after[n] - nbytesn_before[n];
			ns += no_slot_after[n] - no_slot_before[n];
		}

		printf("SUM-PPS: %lu pps (%.2f Mpps)\n",
		       pps, (double)pps / 1000000);
		for (n = 0; n < gen.ncpus; n++)
			printv1("    CPU-PPS %02d: %lu pps\n", n,
				npkts_after[n] - npkts_before[n]);

		printf("SUM-BPS: %lu bps (%.2f Mbps)\n",
		       bps, (double)bps / 1000000);
		for (n = 0; n < gen.ncpus; n++)
			printv1("    CPU-BPS %02d: %lu bps\n", n,
				(nbytes_after[n] - nbytes_before[n]) * 8);

		printf("SUM-BPS-NVME: %lu Bps (%.2f MBps)\n",
		       bpsn, (double)bpsn / 1000000);
		printf("NO-SLOT: %lu\n", ns);
		for (n = 0; n < gen.ncpus; n++) {
			printv1("    CPU-NO-SLOT %02d: %lu no-slot\n", n,
				(no_slot_after[n] - no_slot_before[n]));
		}
		printf("\n");
	}

	return NULL;
}

int main(int argc, char **argv)
{
	int ret, ch, n, i, max_cpu;

	libpop_verbose_enable();

	ret = 0;

	max_cpu = count_online_cpus() > MAX_CPUS ?
		MAX_CPUS : count_online_cpus();

	/* initialize struct generator */
	memset(&gen, 0, sizeof(gen));
	gen.ncpus = 1;
	gen.batch = 1;
	gen.walk = WALK_MODE_SEQ;
	gen.lba_start = 0;
	//gen.lba_end = 0xe8e088b0;	/* XXX: Intel P4600 hard code */
	gen.lba_end = 0x40000;	/* 4 blocks (1slot) x 2048 slots x 32 rings */
	gen.verbose = 0;

	while ((ch = getopt(argc, argv, "p:u:i:n:b:w:s:e:I:FHT:v")) != -1) {
		switch (ch) {
		case 'p':
			if (strncmp(optarg, "hugepage", 8) == 0)
				gen.pci = NULL;
			else
				gen.pci = optarg;
			break;
		case 'u':
			gen.nvme = optarg;
			break;
		case 'i':
			gen.port = optarg;
			break;
		case 'n':
			gen.ncpus = atoi(optarg);
			if (gen.ncpus < 1 || gen.ncpus > max_cpu) {
				printf("invalid cpu num %s\n", optarg);
				return -1;
			}				
			break;
		case 'b':
			gen.batch = atoi(optarg);
			if (gen.batch < 1 || gen.batch > MAX_BATCH_SIZE) {
				printf("invalid batch size %s\n", optarg);
				return -1;
			}
			break;
		case 'w':
			if (strncmp(optarg, "seq", 3) == 0)
				gen.walk = WALK_MODE_SEQ;
			else if (strncmp(optarg, "random", 5) == 0)
				gen.walk = WALK_MODE_RANDOM;
			else {
				printf("invalid walk mode %s\n", optarg);
				return -1;
			}
			break;
		case 's':
			if (sscanf(optarg, "0x%lx", &gen.lba_start) < 1) {
				printf("invalid start lba %s\n", optarg);
				return -1;
			}
			break;
		case 'e':
			if (sscanf(optarg, "0x%lx", &gen.lba_end) < 1) {
				printf("invalid end lba %s\n", optarg);
				return -1;
			}
			break;
		case 'I':
			gen.interval = atoi(optarg) * 1000;
			break;
		case 'v':
			gen.verbose++;
			break;
		case 'F':
			gen.fake_packet = 1;
			break;
		case 'H':
			gen.hex_dump = 1;
			break;
		case 'T':
			gen.timeout = atoi(optarg);
			break;
		default:
			usage();
			return -1;
		}
	}

	if (!gen.nvme) {
		printf("-u nvme must be specified\n");
		goto err_out;
	}

	if (!gen.port) {
		printf("-i port must be spcified\n");
		goto err_out;
	}



	/* initialize rand */
	srand((unsigned)time(NULL));

	/* initialize pop mem */
	gen.mem = pop_mem_init(gen.pci, 0);
	if (!gen.mem) {
		perror("pop_mem_init");
		return -1;
	}

	/* open unvme */
	gen.unvme = unvme_open(gen.nvme);
	unvme_register_pop_mem(gen.mem);

	print_gen_info();

	/* set signal */
	if (signal(SIGINT, sig_handler) == SIG_ERR) {
		perror("signal");
		sig_handler(SIGINT);
	}

	/* initialize threads */
	for (n = 0; n < gen.ncpus; n++) {
		char errmsg[64];
		struct gen_thread *th = &gen_th[n];

		memset(th, 0, sizeof(*th));
		snprintf(th->nmport, MAX_NMPORT_NAME, "netmap:%s-%02d",
			 gen.port, n);
		th->cpu	= n;
		th->lba_start = (gen.lba_end - gen.lba_start)
			/ gen.ncpus * n;
		th->lba_end = (gen.lba_end - gen.lba_start)
			/ gen.ncpus * (n + 1);

		/* initialize netmap port on this queue */
		if (n == 0) {
			int flags;
			struct nm_desc base_nmd;
			memset(&base_nmd, 0, sizeof(base_nmd));
			ret  = nm_parse(gen.port, &base_nmd, errmsg);
			if (ret) {
				printf("nm_parse for %s failed: %s\n",
				       gen.port, errmsg);
				perror("nm_parse");
				sig_handler(SIGINT);
				goto close;
			}

			flags = NM_OPEN_IFNAME | NM_OPEN_ARG1 | NM_OPEN_ARG2 |
				NM_OPEN_ARG3 | NM_OPEN_RING_CFG;

			base_nmd.req.nr_flags &= ~NR_REG_MASK;
			base_nmd.req.nr_flags |= NR_REG_ONE_NIC;
			base_nmd.req.nr_ringid = 0;
			printf("nr_flags is 0x%x\n", base_nmd.req.nr_flags);

			th->nmd = nm_open(gen.port, NULL, flags, &base_nmd);

		} else {
			struct nm_desc nmd = *gen_th[0].nmd;
			nmd.req.nr_flags = gen_th[0].nmd->req.nr_flags &
				~NR_REG_MASK;
			nmd.req.nr_flags |= NR_REG_ONE_NIC;
			nmd.req.nr_ringid = n;
			printf("req port nr_name is %s\n", nmd.req.nr_name);
			printf("nr_flags is 0x%x\n", nmd.req.nr_flags);

			th->nmd = nm_open(gen.port, NULL,
					  NM_OPEN_NO_MMAP, &nmd);
		}

		if (!th->nmd) {
			printf("nm_open for %s failed on cpu %d\n",
			       gen.port, n);
			perror("nm_open");
			ret = -1;
			sig_handler(SIGINT);
			goto close;
		}
		printf("netmap fd is %d\n", th->nmd->fd);

	}
	/* spawn the thread */
	for (n = 0; n < gen.ncpus; n++)
	pthread_create(&gen_th[n].tid, NULL, thread_body, &gen_th[n]);

	/* spawn the couting thread */
	usleep(500000);
	pthread_create(&gen.tid_cnt, NULL, count_thread, NULL);

	/* sleep until timeout passed when it is specified */
	if (gen.timeout) {
		sleep(gen.timeout);
		sig_handler(SIGINT);
	}

close:
	for (i = 0; i < n; i++) {
		printf("join %d thread\n", i);
		pthread_join(gen_th[i].tid, NULL);
		printf("close nm_desc for %d\n", i);
		nm_close(gen_th[i].nmd);
		usleep(1000);
	}

	printf("close unvme\n");
	unvme_close(gen.unvme);
	printf("pop mem exit\n");
	pop_mem_exit(gen.mem);

	printf("join count thread\n");
	pthread_join(gen.tid_cnt, NULL);

	return ret;

err_out:
	return -1;
}

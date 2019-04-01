/* generator.c */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sched.h>
#include <pthread.h>

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

#define NM_BATCH_TO_NBLOCKS(b)	((b) << 11 >> 9)

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
	int	interval;	/* usec */
	int	verbose;	/* verbose level */

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
	printf("\n");
	printf("nblocks in a nvme cmd: %d (%d-byte)\n",
	       NM_BATCH_TO_NBLOCKS(gen.batch),
	       NM_BATCH_TO_NBLOCKS(gen.batch) * 512);
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
	       "    -v                   verbose mode\n"
		);
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
	unsigned long lba, batch, nblocks, b, head, nbytes, npkts;
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

		batch = nm_ring_space(ring) > gen.batch ? gen.batch :
			nm_ring_space(ring);
		if (batch == 0) {
			th->no_slot++;
			goto sync_out;
		}
		head = ring->head;

		/* execute a single read command for all packets 
		 * in this batch. */
		nblocks = NM_BATCH_TO_NBLOCKS(batch);
		iod = unvme_aread(gen.unvme, qid, pop_buf_data(buf),
				  lba, nblocks);
		lba = next_lba(lba, th->lba_start, th->lba_end, nblocks);

		/* fill the netmap_slot with the packet info in
		 * this pop buf region */
		for (b = 0; b < batch; b++) {
			void *pkt = pkts[b];
#define NM 1
#if NM
			struct netmap_slot *slot = &ring->slot[head];
			slot->flags |= NS_PHY_INDIRECT;
			slot->ptr = pop_virt_to_phys(gen.mem, pkt);
			slot->len = get_pktlen_from_desc(pkt, 2048);

			head = nm_ring_next(ring, head);
#endif

			npkts++;
			nbytes += get_pktlen_from_desc(pkt, 2048);

			printv2("CPU=%d LBA=0x%lx head=%lu pkt=%p\n",
				th->cpu, lba, head, pkt);

		}

		ret = unvme_apoll(iod, UNVME_TIMEOUT);
		if (ret != 0) {
			printf("unvme_apoll timeout on cpu %d\n", th->cpu);
			continue;
		}

#if NM
		/* ok, an nvme read command finished, and all netmap
		 * slots for this iteration are filled. Advance the
		 * cur and call ioctl() to xmit packets */
		ring->head = ring->cur = head;
		if (batch < gen.batch) {
			/* tell netmap that we need more slots */
			ring->cur = ring->tail;
		}

#endif
	sync_out:
		if (ioctl(th->nmd->fd, NIOCTXSYNC, NULL) < 0) {
			printf("TXSYNC error on cpu %d\n", th->cpu);
			perror("ioctl(TXSYNC)");
			break;
		}
		printv2("TXSYNC on cpu %d\n", th->cpu);

		th->npkts += npkts;
		th->nbytes += nbytes;
		th->nbytes_nvme += nblocks * 512;

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
		printf("NO=SLOT: %lu\n", ns);

		printf("\n");
	}

	pthread_exit(NULL);
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

	while ((ch = getopt(argc, argv, "p:u:i:n:b:w:s:e:I:v")) != -1) {
		switch (ch) {
		case 'p':
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

	print_gen_info();

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

	/* set signal */
	if (signal(SIGINT, sig_handler) == SIG_ERR) {
		perror("signal");
		sig_handler(SIGINT);
	}

	/* initialize threads */
	for (n = 0; n < gen.ncpus; n++) {
		char errmsg[64];
		struct nm_desc base_nmd;
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
		memset(&base_nmd, 0, sizeof(base_nmd));
		ret = nm_parse(th->nmport, &base_nmd, errmsg);
		if (ret < 0) {
			printf("nm_parse for %s failed: %s\n",
			       th->nmport, errmsg);
			perror("nm_parse");
			sig_handler(SIGINT);
			goto close;
		}

		base_nmd.req.nr_ringid = th->cpu;
		base_nmd.req.nr_flags = NR_REG_ONE_NIC;
		th->nmd = nm_open(th->nmport, NULL, NM_OPEN_IFNAME, &base_nmd);
		if (!th->nmd) {
			printf("nm_open for %s failed", th->nmport);
			perror("nm_open");
			ret = -1;
			sig_handler(SIGINT);
			goto close;
		}
	}
	/* spawn the thread */
	for (n = 0; n < gen.ncpus; n++)
	pthread_create(&gen_th[n].tid, NULL, thread_body, &gen_th[n]);

	/* spawn the couting thread */
	usleep(500000);
	pthread_create(&gen.tid_cnt, NULL, count_thread, NULL);

close:
	for (i = 0; i < n; i++) {
		pthread_join(gen_th[i].tid, NULL);
		nm_close(gen_th[i].nmd);
	}

	unvme_close(gen.unvme);
	pop_mem_exit(gen.mem);

	pthread_join(gen.tid_cnt, NULL);

	return ret;

err_out:
	return -1;
}

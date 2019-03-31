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
	int	verbose;	/* verbose level */

} gen;

/* structure describing a thread */
struct gen_thread {
	pthread_t	tid;	/* pthread tid */
	int	cpu;		/* cpu num on which this thread runs */
	char	nmport[MAX_NMPORT_NAME];	/* netmap:foo-NN */

	struct nm_desc	*nmd;	/* nm_desc no this thread */

	unsigned long	npkts;	/* number of packets TXed */
	unsigned long	nbytes;	/* number of bytes TXed */
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
	printf("=====================================\n");
}

void usage(void)
{
	printf("usage: generator\n"
	       "    -p pci               p2pmem slot, none means hugepage\n"
	       "    -u pci               nvme slot under unvme\n"
	       "    -i port              network interface name\n"
	       "    -n ncpus             number of cpus\n"
	       "    -b batch             batch size\n"
	       "    -w walk mode         seq or random\n"
	       "    -s start lba (hex)   start logical block address\n"
	       "    -e end lba (hex)     end logical block address\n"
	       "    -v                   verbose mode\n"
		);
}

unsigned long next_lba(unsigned long lba, unsigned long lba_end)
{
	switch (gen.walk) {
	case WALK_MODE_SEQ:
		/* XXX: netmap_slot is 2048 byte, compised of
		 * 4 blocks on nvme. */
		if ((lba + 4) < lba_end)
			return lba + 4;
		else
			return gen.lba_start;
		break;
	case WALK_MODE_RANDOM:
		/* align in 4 blocks */
		return (rand() % (lba_end >> 2)) << 2;
		break;
	}

	/* not reached */
	return 0;
}


void *thread_body(void *arg)
{
	int n, ret;
	struct gen_thread *th = arg;
	cpu_set_t target_cpu_set;
	pop_buf_t *buf[MAX_BATCH_SIZE];
	unvme_iod_t iod[MAX_BATCH_SIZE];
	unsigned long lba, batch, b, head;
	struct netmap_ring *ring = NETMAP_TXRING(th->nmd->nifp, th->cpu);



	/* pin this thread on the cpu */
	CPU_ZERO(&target_cpu_set);
	CPU_SET(th->cpu, &target_cpu_set);
	pthread_setaffinity_np(th->tid, sizeof(cpu_set_t), &target_cpu_set);

	/* allocate num of batch pop bufs */
	for (n = 0; n < gen.batch; n++) {
		buf[n] = pop_buf_alloc(gen.mem, 2048);
		pop_buf_put(buf[n], 2048);
	}

	/* initialize the start LBA in accordance with walk mode */
	lba = gen.lba_start + (gen.walk == WALK_MODE_RANDOM ? th->cpu * 4: 0);


	printf("thread on cpu %d, nmport %s, start\n", th->cpu, th->nmport);
	gettimeofday(&th->start, NULL);

	while (!caught_signal) {

		batch = nm_ring_space(ring) > gen.batch ? gen.batch :
			nm_ring_space(ring);

		head = ring->head;

		for (b = 0; b < batch; b++) {
			void *pkt = pop_buf_data(buf[b]);

			/* execute an nvme read cmd to a pop buf, and
			 * set the buf for a netmap tx slot. this can
			 * be done asynchronously. */
			iod[b] = unvme_aread(gen.unvme, th->cpu, pkt, lba, 4);
			lba = next_lba(lba, gen.lba_end);

			pop_nm_set_buf(&ring->slot[head], buf[b]);
			ring->slot[head].len = get_pktlen_from_desc(pkt, 2048);
			head = nm_ring_next(ring, head);

			th->npkts++;
			th->nbytes += get_pktlen_from_desc(pkt, 2048);

			printv2("LBA=0x%lx head=%lu pkt=%p\n", lba, head, pkt);
		}

		for (b = 0; b < batch; b++) {
			ret = unvme_apoll(iod[b], UNVME_TIMEOUT);
			if (ret != 0) {
				printf("unvme_apoll timeout on cpu %d\n",
				       th->cpu);
			}
		}

		/* ok, all nvme read commands finished, and all netmap
		 * slots for this iteration are filled. Advance the
		 * cur and call ioctl() to xmit packets */
		ring->head = ring->cur = head;
		ioctl(th->nmd->fd, NIOCTXSYNC, NULL);
		printv2("TXSYNC\n");
	}

	gettimeofday(&th->end, NULL);

	/* xmit pending packets */
	while (nm_tx_pending(ring)) {
		ioctl(th->nmd->fd, NIOCTXSYNC, NULL);
		usleep(1);
	}

	return NULL;
}

void *count_thread(void *arg)
{
	int n, cpu;
	cpu_set_t target_cpu_set;
	unsigned long pps, npkts_before[MAX_CPUS], npkts_after[MAX_CPUS];
	unsigned long bps, nbytes_before[MAX_CPUS], nbytes_after[MAX_CPUS];
	
	memset(npkts_before, 0, sizeof(unsigned long) * MAX_CPUS);
	memset(npkts_after, 0, sizeof(unsigned long) * MAX_CPUS);
	memset(nbytes_before, 0, sizeof(unsigned long) * MAX_CPUS);
	memset(nbytes_after, 0, sizeof(unsigned long) * MAX_CPUS);

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
		}

		sleep(1);

		for (n = 0; n < gen.ncpus; n++) {
			npkts_after[n] = gen_th[n].npkts;
			nbytes_after[n] = gen_th[n].nbytes;
		}

		/* totaling the counters */
		for (pps = 0, bps = 0, n = 0; n < gen.ncpus; n++) {
			pps += npkts_after[n] - npkts_before[n];
			bps += nbytes_after[n] - nbytes_before[n];
		}

		printf("SUM-PPS: %lu pps\n", pps);
		for (n = 0; n < gen.ncpus; n++)
			printv1("    CPU-PPS %02d: %lu pps\n", n,
				npkts_after[n] - npkts_before[n]);

		printf("SUM-BPS: %lu bps\n", bps);
		for (n = 0; n < gen.ncpus; n++)
			printv1("    CPU-BPS %02d: %lu bps\n", n,
				nbytes_after[n] - nbytes_before[n]);
	}

	pthread_exit(NULL);
}

int main(int argc, char **argv)
{
	int ret, ch, n, i, max_cpu;

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

	while ((ch = getopt(argc, argv, "p:u:i:n:b:w:s:e:v")) != -1) {
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
		
		snprintf(th->nmport, MAX_NMPORT_NAME, "netmap:%s-%02d",
			 gen.port, n);
		th->cpu	= n;
		th->npkts = 0;
		th->nbytes = 0;
		
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

		/* spawn the thread */
		pthread_create(&th->tid, NULL, thread_body, th);
	}


	/* spawn the couting thread */
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

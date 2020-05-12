/* nvgen.c */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
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

#include <unvme.h>

#include "pkt_desc.h"

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

#define MAX_CPU_NUM	32
#define NVGEN_MODE_TX	0
#define NVGEN_MODE_RX	1

#define MAX_BATCH_NUM	64

#define SLOT_NUM	256

#define MAX_NVBATCH_NUM	128
#define NVGEN_WALK_MODE_SEQ	0
#define	NVGEN_WALK_MODE_RANDOM	1
#define	NVGEN_WALK_MODE_SAME	2

struct nvgen {
	char	*pci;	/* memory*/
	int	mode;	/* NVGEN_MODE_TX or NVGEN_MODE_RX */
	int	ncpus;	/* number of cpus to be used	*/

	/* netmap */
	char	*port;	/* netmap port	*/
	int	batch;	/* batch size	*/

	/* unvme */
	char	*nvme;		/* PCIe slot for NVMe device */
	int	nvbatch;	/* batch size for NVMe commands */
	int	walk;		/* walk mode */
	unsigned long		lba_start, lba_end;	/* LBA */
	const unvme_ns_t	*unvme;	/* UNVMe context */
	

	float	interval;	/* report interval (usec) */
	int	timeout;

	pop_mem_t	*mem;	/* pop memory */
};

/* ring queue between netmap and unvme */
struct nvgen_ring {
	uint32_t	head;	/* write point	*/
	uint32_t	tail;	/* read point	*/
	uint32_t	mask;	/* bit mask for ring buffer */
};

struct nvgen_thread {
	struct nvgen	*gen;

	pthread_t	tid;
	int		cpu;

#ifdef LIBNETMAP
	struct nmport_d *nmd;
#else
	struct nm_desc	*nmd;	/* nm_desc for this thread	*/
#endif /* LIBNETMAP */

	unsigned long	lba_start, lba_end;	/* LBA range for this thread */

	/* ring between netmap and unvme */
	struct nvgen_ring	ring;

	/* packet buffer on pop mem. managed by ring */
	void		*buf;

	int		cancel;
	unsigned long	npkts;	/* packet counter	*/
	unsigned long	nbits;	/* bit counter	for Ethernet NIC */
	unsigned long	nbytes;	/* byte counter for NVMe */
};




void nvgen_ring_init(struct nvgen_ring *r)
{
	r->head = 0;
	r->tail = 0;
	r->mask = SLOT_NUM - 1;
}

static inline bool ring_empty(const struct nvgen_ring *r)
{
	return (r->head == r->tail);
}

static inline bool ring_full(const struct nvgen_ring *r)
{
	return (((r->head + 1) & r->mask) == r->tail);
}

static inline uint32_t ring_write_next(struct nvgen_ring *r, uint32_t head)
{
	return (head + 1) & r->mask;
}

static inline uint32_t ring_write_next_batch(struct nvgen_ring *r,
					     uint32_t head,
					     uint32_t batch)
{
	return (head + batch) & r->mask;
}

static inline uint32_t ring_read_next(struct nvgen_ring *r, uint32_t tail)
{
	return (tail + 1) & r->mask;
}


#define ring_read_avail(r) \
	((r)->head - (r)->tail) < 0 ?			\
	(r)->head - (r)->tail + SLOT_NUM :		\
	(r)->head - (r)->tail

/*
static inline uint32_t ring_read_avail(const struct nvgen_ring *r)
{
	int remain = r->head - r->tail;
	if (remain < 0)
		remain += SLOT_NUM;
	return remain;
}
*/

static uint32_t ring_write_avail(const struct nvgen_ring *r)
{
	int remain = r->tail - r->head;

	if (remain == 0)
		return SLOT_NUM;

	if (remain < 0)
		remain += SLOT_NUM - 1;

	return remain;
}

int nvgen_init_thread_body(struct nvgen_thread *th)
{
	pop_buf_t *pbuf;
	struct nvgen *gen = th->gen;

	th->lba_start = gen->lba_end / gen->ncpus * th->cpu;
	th->lba_end = gen->lba_end / gen->ncpus * (th->cpu + 1);

	printf("cpu=%d lba_start 0x%lx bla_end 0x%lx\n",
	       th->cpu, th->lba_start, th->lba_end);

	pbuf = pop_buf_alloc(gen->mem, 2048 * SLOT_NUM);
	if (!pbuf) {
		fprintf(stderr, "pop_buf_alloc() on cpu %d: %s\n",
			th->cpu, strerror(errno));
		return -1;
	}
	pop_buf_put(pbuf, 2048 * SLOT_NUM);
	th->buf = pop_buf_data(pbuf);

	nvgen_ring_init(&th->ring);
	
	return 0;
}

void *nvgen_sender_netmap_body(void *arg)
{
	unsigned int head, tail, loaded, space, batch, b;
	unsigned long npkts, nbits;
	double elapsed;
	struct timeval start, end;
	struct nvgen_thread *th = arg;
	struct nvgen *gen = th->gen;
	struct netmap_ring *ring;
	cpu_set_t target_cpu_set;
	void *pkts[SLOT_NUM];
	uintptr_t pkts_phy[SLOT_NUM];	/* phy addr of packets */
	int n;

	/* pin this thread on the cpu */
	CPU_ZERO(&target_cpu_set);
	CPU_SET(th->cpu, &target_cpu_set);
	pthread_setaffinity_np(th->tid, sizeof(cpu_set_t), &target_cpu_set);

	/* determine phy addr of packets on pop buf region */
	for (n = 0; n < SLOT_NUM; n++) {
		pkts[n] = th->buf + (2048 * n);
		pkts_phy[n] = pop_virt_to_phys(gen->mem, pkts[n]);
	}

	ring = NETMAP_TXRING(th->nmd->nifp, th->cpu);

	printf("start xmit loop on cpu %d, fd %d\n", th->cpu, th->nmd->fd);

	gettimeofday(&start, NULL);

	/* xmit loop */
	while (!th->cancel) {
		npkts = 0;
		nbits = 0;

		/* wait packets read from NVMe */
		loaded = ring_read_avail(&th->ring);
		if (loaded < 2)
			continue;
			
		if (ioctl(th->nmd->fd, NIOCTXSYNC, NULL) < 0) {
			fprintf(stderr, "ioctl error on cpu %d: %s\n",
				th->cpu, strerror(errno));
			goto out;
		}

		space = nm_ring_space(ring);
		if (!space)
			continue;

		/* XXX: we use 4k block NVMe, so handling multiple of 2
		 * packets is convenient */
		space &= ~0x0001;

		batch = space > gen->batch ? gen->batch : space;
		batch = batch > loaded ? loaded : batch;

		head = ring->head;
		tail = th->ring.tail;

		for (b = 0; b < batch; b++) {
			struct netmap_slot *slot = &ring->slot[head];
			slot->flags |= NS_PHY_INDIRECT;
			slot->ptr = pkts_phy[tail];
			slot->len = get_pktlen_from_desc(pkts[tail], 2048);

			npkts++;
			nbits += (slot->len << 3);

			/* advance nvgen_ring and netmap_ring */
			tail = ring_read_next(&th->ring, tail);
			head = nm_ring_next(ring, head);
		}

		ring->head = ring->cur = head;
		/* XXX: xmit all stored packets here */
		/*
		while (nm_tx_pending(ring)) {
			ioctl(th->nmd->fd, NIOCTXSYNC, NULL);
			sched_yield();
		}
		*/

		th->ring.tail = tail;
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

unsigned long next_lba(int walk, unsigned long lba,
		       unsigned long lba_start, unsigned long lba_end,
		       unsigned long nblocks)
{
	switch (walk) {
	case NVGEN_WALK_MODE_SEQ:
		if (lba + (nblocks << 1) < lba_end)
			return lba + nblocks;
		else
			return lba_start;
		break;

	case NVGEN_WALK_MODE_RANDOM:
		return rand() % (lba_end - lba_start - nblocks) + lba_start;

	case NVGEN_WALK_MODE_SAME:
		return lba;
	}

	/* not reached */
	return 0;
}


void *nvgen_sender_unvme_body(void *arg)
{
	struct nvgen_thread *th = arg;
	struct nvgen *gen = th->gen;

	unsigned int lba, space, batch, b, head, nblocks = 0;
	unvme_iod_t iod[MAX_NVBATCH_NUM];
	void *slots[SLOT_NUM];
	cpu_set_t target_cpu_set;
	int n, ret, cpu;
	
	/* pin this thread on the cpu */
	cpu = th->cpu + (count_online_cpus() >> 1);
	CPU_ZERO(&target_cpu_set);
	CPU_SET(cpu, &target_cpu_set);
	pthread_setaffinity_np(th->tid, sizeof(cpu_set_t), &target_cpu_set);

	for (n = 0; n < SLOT_NUM; n++) {
		slots[n] = th->buf + (2048 * n);
	}

	lba = th->lba_start;

	printf("start unvme loop qid %d on cpu %d\n", th->cpu, cpu);

	while (!th->cancel) {
		
		space = ring_write_avail(&th->ring);
		if (space < 2)
			continue;
		       
		/* XXX: we use 4k block NVMe, so handling multiple of 2
		 * packets is convenient */
		space &= ~0x0001;
		batch = space >> 1 > gen->nvbatch ? gen->nvbatch : space >> 1;

		/*
		printf("head=%u tail=%u space=%u batch=%u\n",
		       th->ring.head, th->ring.tail, space, batch);
		*/

		if (gen->walk == NVGEN_WALK_MODE_SEQ) {
			nblocks = batch;
			batch = 1;
		} else if (gen->walk == NVGEN_WALK_MODE_RANDOM) {
			nblocks = 1;
			//batch = batch;
		} else {
			fprintf(stderr, "invalid walk %d\n", gen->walk);
			break;
		}

		head = th->ring.head;
		for (b = 0; b < batch; b++) { /* slot is 2k, block is 4k */
			iod[b] = unvme_aread(gen->unvme, th->cpu,
					     slots[head], lba, nblocks);

			lba = next_lba(gen->walk, lba,
				       th->lba_start, th->lba_end, nblocks);

			head = ring_write_next_batch(&th->ring, head,
						     (nblocks << 1));
			/* slot is 2k, but block is 4k. advance ring
			 * with nblock * 2*/
		}

		for (b = 0; b < batch; b++) {
			ret = unvme_apoll(iod[b], UNVME_TIMEOUT);
			if (ret == 0) {
				th->nbytes += nblocks * 4096;
			}
		}

		th->ring.head = head;
	}

	return NULL;
}

void *nvgen_receiver_body(void *arg)
{
	unsigned long npkts, nbits, head;
	double elapsed;
	struct timeval start, end;
	struct pop_nm_rxring *prxring;
	struct nvgen_thread *th = arg;
	struct nvgen *gen = th->gen;
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
	struct nvgen_thread *ths = arg;
	struct nvgen *gen = ths[0].gen;
	unsigned long npkts_b[MAX_CPU_NUM], npkts_a[MAX_CPU_NUM];
	unsigned long nbits_b[MAX_CPU_NUM], nbits_a[MAX_CPU_NUM];
	unsigned long nbytes_b[MAX_CPU_NUM], nbytes_a[MAX_CPU_NUM];
	double pps, bps, byteps, elapsed;
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
			nbytes_b[n] = ths[n].nbytes;
		}

		gettimeofday(&b, NULL);
		usleep(gen->interval);
		gettimeofday(&a, NULL);

		for (n = 0; n < gen->ncpus; n++) {
			npkts_a[n] = ths[n].npkts;
			nbits_a[n] = ths[n].nbits;
			nbytes_a[n] = ths[n].nbytes;
		}

		
		/* totaling the counters */
		for (pps = 0, bps = 0, byteps = 0, n = 0;
		     n < gen->ncpus; n++) {
			pps += npkts_a[n] - npkts_b[n];
			bps += nbits_a[n] - nbits_b[n];
			byteps += nbytes_a[n] - nbytes_b[n];
		}
		     
		/* elapsed tie is usec */
		elapsed  =  a.tv_sec * 1000000 + a.tv_usec;
		elapsed -= (b.tv_sec * 1000000 + b.tv_usec);
		elapsed /= 1000000;	/* usec -> sec */

		if (caught_signal)
			break;

		printf("[INTERIM] "
		       "%.2f pps, %.2f Mpps, "
		       "%.2f bps, %.2f Mbps, "
		       "%.2f Bps, %.2f MBps\n",
		       pps / elapsed, pps / elapsed / 1000000,
		       bps / elapsed, bps / elapsed / 1000000,
		       byteps / elapsed, byteps / elapsed / 1000000);

		/*
		printf("head=%u tail=%u\n",
		       ths[0].ring.head, ths[0].ring.tail);
		*/
		       
	}
	
	/* stop sender threads */
	printf("\n");
	for (n = 0; n < gen->ncpus; n++)
		ths[n].cancel = 1;

	return NULL;
}

void print_nvgen_info(struct nvgen *gen)
{
	printf("================ nvgen ================\n");
	printf("netmap port (-p):    %s\n", gen->port);
	printf("pci (-P):            %s\n", gen->pci ? gen->pci : "hugepage");
	printf("unvme device (-u):   %s\n", gen->nvme);
	printf("mode (-m):           %s\n",
	       gen->mode == NVGEN_MODE_TX ? "tx" : "rx");
	printf("ncpus (-n):          %d\n", gen->ncpus);
	printf("batch (-b):          %d\n", gen->batch);
	printf("nvme end lba (-e)    0x%lx\n", gen->lba_end);
	printf("nvme batch (-B):     %d\n", gen->nvbatch);
	printf("nvme walk mode (-w): %s\n",
	       gen->walk == NVGEN_WALK_MODE_SEQ ? "seq" :
	       gen->walk == NVGEN_WALK_MODE_RANDOM ? "random": "invalid");
	printf("interval (-i):       %f\n", gen->interval / 1000000);
	printf("timeout (-t):        %d\n", gen->timeout);

	printf("=======================================\n");
}

void usage(void) {
	printf("\nusage: nmge\n"
	       "\n"
	       "    -p port           netmap port\n"
	       "    -P pci            pop memory slot or 'hugepage'\n"
	       "    -u pci            pcie slot for nvme device\n"
	       "    -m tx/rx          direction (now, tx only)\n"
	       "\n"
	       "    -n ncpus          number of CPUs to be used\n"
	       "    -b batch          batch size for netmap\n"
	       "\n"
	       "    -e lba            end lba on nvme\n"
	       "    -B bacth          batch size for unvme\n"
	       "    -w walk           walk mode (seq or random)"
	       "\n"
	       "    -i interval       report interval\n"
	       "    -t timeout        timeout to stop\n"
		);
}


int main(int argc, char **argv)
{
	struct nvgen_thread ths[MAX_CPU_NUM];
	struct nvgen gen;
	pthread_t ctid;
	int ch, n;

	srand((unsigned)time(NULL));

	memset(&gen, 0, sizeof(gen));
	gen.mode = NVGEN_MODE_TX;
	gen.ncpus = 1;
	gen.batch = 1;
	gen.lba_end = 0xF0000;
	gen.nvbatch = 1;
	gen.interval = 1000000;
	gen.timeout = 0;

	while ((ch = getopt(argc, argv, "p:P:u:m:n:b:e:B:w:i:t:h")) != -1) {
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
		case 'u':
			gen.nvme = optarg;
			break;
		case 'm':
			if (strncmp(optarg, "tx", 2) == 0)
				gen.mode = NVGEN_MODE_TX;
			else if (strncmp(optarg, "rx", 2) == 0)
				gen.mode = NVGEN_MODE_RX;
			else {
				fprintf(stderr, "invalid mode %s\n", optarg);
				return -1;
			}
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
				fprintf(stderr, "too large batch (> %d)\n",
					MAX_BATCH_NUM);
				return -1;
			}
			break;
		case 'e':
			if (sscanf(optarg, "0x%lx", &gen.lba_end) < 1) {
				printf("invalid end lba: %s\n", optarg);
				return -1;
			}
			break;
		case 'B':
			gen.nvbatch = atoi(optarg);
			if (gen.nvbatch > MAX_NVBATCH_NUM) {
				fprintf(stderr, "too large nvbatch (> %d)\n",
					MAX_NVBATCH_NUM);
				return -1;
			}
			break;
		case 'w':
			if (strncmp("seq", optarg, 3) == 0)
				gen.walk = NVGEN_WALK_MODE_SEQ;
			else if (strncmp("random", optarg, 6) == 0)
				gen.walk = NVGEN_WALK_MODE_RANDOM;
			else {
				fprintf(stderr, "invalid walk mode: %s\n",
					optarg);
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
	if (!gen.nvme) {
		fprintf(stderr, "-u unvme device must be specified\n");
		return -1;
	}

	if (gen.ncpus * 2 > count_online_cpus()) {
		fprintf(stderr, "ncpus must be < %d\n", count_online_cpus());
		return -1;
	}

	/* rx needs all cpus to check all queues (skimped work) */
	if (gen.mode == NVGEN_MODE_RX)
		gen.ncpus = count_online_cpus();

	print_nvgen_info(&gen);

	/* initialize libpop and unvme */
	gen.mem = pop_mem_init(gen.pci, 0);
	if (!gen.mem) {
		fprintf(stderr, "pop_mem_init(%s): %s\n",
			gen.pci, strerror(errno));
		return -1;
	}

	gen.unvme = unvme_open(gen.nvme);
	unvme_register_pop_mem(gen.mem);

	if (signal(SIGINT, sig_handler) == SIG_ERR) {
		perror("signal");
		return -1;
	}

	
	/* initialize netmap ports  */
	for (n = 0; n < gen.ncpus; n++) {
		struct nvgen_thread *th = &ths[n];

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
		case NVGEN_MODE_TX:
			nvgen_init_thread_body(&ths[n]);
			pthread_create(&ths[n].tid, NULL,
				       nvgen_sender_netmap_body, &ths[n]);
			pthread_create(&ths[n].tid, NULL,
				       nvgen_sender_unvme_body, &ths[n]);
			break;
		case NVGEN_MODE_RX:
			pthread_create(&ths[n].tid, NULL,
				       nvgen_receiver_body, &ths[n]);
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
	

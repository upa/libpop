/* bench-nvme.c */

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

#include <unvme.h>
#include <unvme_nvme.h>

static int verbose_level = 0;
static int caught_signal = 0;

#define printv(fmt, ...)				\
	do {						\
		if (verbose_level) {			\
			printf(fmt, __VA_ARGS__);	\
		}					\
	} while (0)					\


enum {
	WALK_RANDOM	= 0,
	WALK_SEQ	= 1,
	WALK_SAME	= 2,
};
const char *walk_str[] = {
	"random", "seq", "same",
};

#define MAX_BATCH_SIZE	64
#define MAX_CPU_NUM	32

struct bench_param {
	int	mode;	/* NVME_CMD_WRITE or NVME_CMD_READ 	*/
	int	walk;	/* WALK_* 				*/
	int	size;	/* size of each i/o 			*/
	int	batch;	/* batch size 				*/
	int	ncpus;	/* number of used cpus			*/
	int	time;	/* benchmark time 			*/
	char	*nvme;	/* pci slot number of nvme device	*/
	char	*p2p;	/* pci slot number of p2pmem device	*/
	unsigned long lba_end;	/* end logical block address	*/

	const unvme_ns_t	*ns;	/* nvme namespace	*/
	pop_mem_t		*mem;	/* boogiepop memory	*/
	pthread_t		rtid;	/* report thread	*/
};
struct bench_param p;

struct bench_thread {
	pthread_t	tid;
	unsigned long	lba_start, lba_end;	/* start/end lba */

	int 		cpu;	/* cpu num this thread runs 	*/
	unsigned long 	count;	/* number of i/o		*/
	unsigned long	bytes;	/* number of bytes		*/



	struct timeval	start;	/* benchmark start time */
	struct timeval	end;	/* benchmark end time*/
};



void print_p(struct bench_param p) {
	printf("==== Benchmark Configuration ====\n");
	printf("mode:       %s\n", p.mode == NVME_CMD_READ ? "read" : "write");
	printf("walk:       %s\n", walk_str[p.walk]);
	printf("size:       %d\n", p.size);
	printf("batch:      %d\n", p.batch);
	printf("device:     %s\n", p.nvme);
	printf("mem:        %s\n", p.p2p ? p.p2p : "hugepage");
	printf("ncpus:      %d\n", p.ncpus);
	printf("end lba:    0x%lx\n", p.lba_end);
	printf("time:       %d\n", p.time);
	printf("=================================\n");
}

void usage(void) {
	printf("usage:\n"
	       "    -m: mode, read or write\n"
	       "    -w: walk mode, random, seq, or same\n"
	       "    -s: message size\n"
	       "    -b: batch size\n"
	       "    -n: number of cpus to be used\n"
	       "    -t: benchmark time (sec)\n"
	       "    -e: end lba (hex)\n"
	       "    -u: PCI slot of target nvme device\n"
	       "    -p: PCI slot of p2pmem\n");
}

int count_online_cpus(void)
{
	cpu_set_t cpu_set;

	if (sched_getaffinity(0, sizeof(cpu_set_t), &cpu_set) == 0)
		return CPU_COUNT(&cpu_set);

	return -1;
}

unsigned long get_usec_elapsed(struct timeval start, struct timeval end)
{
	unsigned long usec;
	if (end.tv_usec < start.tv_usec) {
		end.tv_usec += 1000000;
		end.tv_sec -= 1;
	}

	usec = (end.tv_sec - start.tv_sec) * 1000000 +
		end.tv_usec - start.tv_usec;

	return usec;
}
	
void print_result(struct bench_thread th)
{
	double elapsed;

	elapsed = (double)get_usec_elapsed(th.start, th.end);
	printf("[FINAL] qid %d "
	       "%.2lf iops, %.2lf kiops, %.2lf Bps, %.2lf MBps\n",
	       th.cpu,
	       th.count / elapsed * 1000000,
	       th.count / elapsed * 1000000 / 1000,
	       th.bytes / elapsed * 1000000,
	       (th.bytes / elapsed) * 1000000 / (1024 * 1024));
}

void print_interval_result(struct bench_thread *th)
{
	int n;
	unsigned long count_start, count_end, bytes_start, bytes_end;
	double elapsed;
	struct timeval start, end;

	count_start	= 0;
	count_end	= 0;
	bytes_start	= 0;
	bytes_end	= 0;

	gettimeofday(&start, NULL);
	for (n = 0; n < p.ncpus; n++) {
		count_start += th[n].count;
		bytes_start += th[n].bytes;
	}

	sleep (1);

	gettimeofday(&end, NULL);
	for (n = 0; n < p.ncpus; n++) {
		count_end += th[n].count;
		bytes_end += th[n].bytes;
	}

	elapsed = (double) get_usec_elapsed(start, end);
	printf("[INTERIM] "
	       "%.2lf iops, %.2lf kiops, %.2lf Bps, %.2lf MBps\n",
	       (count_end - count_start) / elapsed * 1000000,
	       (count_end - count_start) / elapsed * 1000000 / 1000,
	       (bytes_end - bytes_start) / elapsed * 1000000,
	       (bytes_end - bytes_start) / elapsed * 1000000 / (1024 * 1024));
}

void *report_interval(void *arg)
{
	struct bench_thread *th = arg;	/* pointer for th[MAX_CPU_NUM] */
	cpu_set_t target_cpu_set;
	
	/* set self on last cpu */
	CPU_ZERO(&target_cpu_set);
	CPU_SET(count_online_cpus() - 1, &target_cpu_set);
	pthread_setaffinity_np(p.rtid, sizeof(cpu_set_t), &target_cpu_set);

	while (!caught_signal) {
		print_interval_result(th);
	}

	return NULL;
}

unsigned long next_lba(unsigned long lba,
		       unsigned long lba_start, unsigned long lba_end,
		       unsigned long nblocks)
{
	switch (p.walk) {
	case WALK_RANDOM:
		return rand() % (lba_end - lba_start - nblocks) + lba_start;

	case WALK_SAME:
		return lba;

	case WALK_SEQ:
		if (lba + (nblocks << 1) < lba_end)
			return lba + nblocks;
		else
			return lba_start;
		break;
	}

	/* not reached */
	return 0;
}

unvme_iod_t bench(int mode, int qid, void *buf,
	   unsigned long lba, unsigned int nblocks) {

	if (mode == NVME_CMD_READ) {
		return unvme_aread(p.ns, qid, buf, lba, nblocks);
	}
	return unvme_awrite(p.ns, qid, buf, lba, nblocks);
}

void * bench_start(void *arg)
{
	struct bench_thread *th = arg;
	cpu_set_t target_cpu_set;
	int b, ret, qid = th->cpu;
	unsigned int n;
	pop_buf_t *bufs[MAX_BATCH_SIZE];	
	unvme_iod_t iod[MAX_BATCH_SIZE];
	unsigned long lba, nblocks;

	/* pin this thread on the specified cpu */
	CPU_ZERO(&target_cpu_set);
	CPU_SET(th->cpu, &target_cpu_set);
	pthread_setaffinity_np(th->tid, sizeof(cpu_set_t), &target_cpu_set);
	
	/* number of blocks for one i/o iteration */
	nblocks = p.size % p.ns->blocksize ?
		(p.size >> p.ns->blockshift) + 1 : p.size >> p.ns->blockshift;

	for (n = 0; n < p.batch; n++) {
		bufs[n] = pop_buf_alloc(p.mem, nblocks << p.ns->blockshift);
		pop_buf_put(bufs[n], nblocks << p.ns->blockshift);
	}

	lba = th->lba_start;

	printf("start on queue %d, slba=%#lx, nblocks=%lu, batch=%d\n",
	       qid, lba, nblocks, p.batch);

	gettimeofday(&th->start, NULL);
	while (!caught_signal) {

		for (b = 0; b < p.batch; b++) {
			printv("batch %d on qid %d. lba=%#lx nblocks=%lu\n",
			       b, qid, lba, nblocks);
			iod[b] = bench(p.mode, qid, pop_buf_data(bufs[b]),
				       lba, nblocks);
			lba = next_lba(lba, th->lba_start, th->lba_end, nblocks);
		}

		for (b = 0; b < p.batch; b++) {
			ret = unvme_apoll(iod[b], UNVME_TIMEOUT);
			if (ret == 0) {
				th->bytes += p.size;
				th->count += 1;
			} else {
				printf("poll timeout on cpu %d\n", th->cpu);
			}
		}
	}
	gettimeofday(&th->end, NULL);

	return NULL;
}


void sig_handler(int sig)
{
	if (sig == SIGINT)
		caught_signal = 1;
}


int main(int argc, char **argv)
{
	int ch, n;
	struct bench_thread th[MAX_CPU_NUM];

	memset(th, 0, sizeof(th));
	memset(&p, 0, sizeof(p));
	p.mode	= NVME_CMD_READ;
	p.walk	= WALK_RANDOM;
	p.size	= 64;
	p.batch = 1;
	p.nvme	= NULL;
	p.p2p	= NULL;
	p.ncpus = 1;
	p.time	= 1;
	p.lba_end = 0xe8e088b0;   /* SSDPEDKE020T7 hard code */

	while ((ch = getopt(argc, argv, "m:w:s:b:u:p:n:e:t:v")) != -1) {
		switch (ch) {
		case 'm':
			/* mode, read or write */
			if (strncmp(optarg, "read", 4) == 0)
				p.mode = NVME_CMD_READ;
			else if (strncmp(optarg, "write", 5) == 0)
				p.mode = NVME_CMD_WRITE;
			else {
				printf("invalid mode: %s\n", optarg);
				usage();
				return -1;
			}
			break;
			
		case 'w':
			/* walk mode */
			if (strncmp(optarg, "random", 6) == 0)
				p.walk = WALK_RANDOM;
			else if (strncmp(optarg, "seq", 3) == 0)
				p.walk = WALK_SEQ;
			else if (strncmp(optarg, "same", 4) == 0)
				p.walk = WALK_SAME;
			else {
				printf("invalid walk mode: %s\n", optarg);
				usage();
				return -1;
			}
			break;

		case 's':
			p.size = atoi(optarg);
			if (p.size < 0) {
				printf("invalid size: %s\n", optarg);
				return -1;
			}
			break;

		case 'b':
			p.batch = atoi(optarg);
			if (p.batch < 0 || p.batch > MAX_BATCH_SIZE) {
				printf("invalid batch size: %s\n", optarg);
				return -1;
			}
			break;

		case 'n':
			p.ncpus = atoi(optarg);
			if (p.ncpus < 1 || p.ncpus > count_online_cpus()) {
				printf("invalid cpu number: %s\n", optarg);
				return -1;
			}
			break;

		case 'e':
			if (sscanf(optarg, "0x%lx", &p.lba_end) < 1) {
				printf("invalid end lba: %s\n", optarg);
				return -1;
			}
			break;

		case 't':
			p.time = atoi(optarg);
			if (p.time < 0) {
				printf("invalid time: %s\n", optarg);
			}
			break;

		case 'u':
			p.nvme = optarg;
			break;

		case 'p':
			p.p2p = optarg;
			break;

		case 'v':
			verbose_level++;
			break;

		default:
			usage();
			return -1;
		}
	}


	print_p(p);

	srand((unsigned)time(NULL));


	if (!p.nvme) {
		printf("-u nvme device must be specified\n");
		return -1;
	}
	p.ns = unvme_open(p.nvme);
	
	p.mem = pop_mem_init(p.p2p, 0);
	unvme_register_pop_mem(p.mem);

	printf("start benchmarking\n");
	for (n = 0; n < p.ncpus; n++) {
		th[n].cpu = n;
		th[n].lba_start = p.lba_end / p.ncpus * n;
		th[n].lba_end = p.lba_end / p.ncpus * (n + 1);
		pthread_create(&th[n].tid, NULL, bench_start, &th[n]);
	}

	pthread_create(&p.rtid, NULL, report_interval, th);

	sleep(p.time);
	caught_signal = 1;

	for (n = 0; n < p.ncpus; n++)
		pthread_join(th[n].tid, NULL);
	pthread_join(p.rtid, NULL);

	printf("close nvme %s\n", p.nvme);
	unvme_close(p.ns);
	pop_mem_exit(p.mem);

	for (n = 0; n < p.ncpus; n++)
		print_result(th[n]);

	return 0;
}

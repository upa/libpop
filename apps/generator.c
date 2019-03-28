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
	
} gen;

/* structure describing a thread */
struct gen_thread {
	pthread_t	tid;	/* pthread tid */
	int	cpu;		/* cpu num on which this thread runs */
	char	nmport[MAX_NMPORT_NAME];	/* netmap:foo-NN */

	struct nm_desc	*nmd;	/* nm_desc no this thread */

	unsigned long	npkts;	/* number of packets TXed */
	unsigned long	nbytes;	/* number of bytes TXe */
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
	       "    -p pci          p2pmem slot, none means hugepage\n"
	       "    -u pci          nvme slot under unvme\n"
	       "    -i port         network interface name\n"
	       "    -n ncpus        number of cpus\n"
	       "    -b batch        batch size\n"
	       "    -w walk mode    seq or random\n"
	       "    -s start lba    start logical block address\n"
	       "    -e end lba      end logical block address\n"
		);
}


void *thread_body(void *arg)
{
	struct gen_thread *th = arg;

	printf("thread on cpu %d, nmport %s\n", th->cpu, th->nmport);

	return NULL;
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
	gen.lba_end = 0xe8e088b0;	/* XXX: Intel P4600 hard code */


	while ((ch = getopt(argc, argv, "p:u:i:n:b:w:s:e:")) != -1) {
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
			gen.lba_start = atol(optarg);
			break;
		case 'e':
			gen.lba_end = atol(optarg);
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

	/* initialize pop mem */
	gen.mem = pop_mem_init(gen.pci, 0);
	if (!gen.mem) {
		perror("pop_mem_init");
		return -1;
	}

	/* open unvme */
	gen.unvme = unvme_open(gen.nvme);
	unvme_register_pop_mem(gen.mem);

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
			goto close;
		}

		base_nmd.req.nr_ringid = th->cpu;
		base_nmd.req.nr_flags = NR_REG_ONE_NIC;
		th->nmd = nm_open(th->nmport, NULL, NM_OPEN_IFNAME, &base_nmd);
		if (!th->nmd) {
			printf("nm_open for %s failed", th->nmport);
			perror("nm_open");
			ret = -1;
			goto close;
		}

		/* spawn the thread */
		pthread_create(&th->tid, NULL, thread_body, th);
	}


close:
	caught_signal = 1;
	for (i = 0; i < n; i++) {
		pthread_join(gen_th[i].tid, NULL);
		nm_close(gen_th[i].nmd);
	}

	unvme_close(gen.unvme);
	pop_mem_exit(gen.mem);

	return ret;

err_out:
	return -1;
}
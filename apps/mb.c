#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

#include <libpop.h>


inline unsigned long long rdtsc() {
	unsigned long long ret;
	__asm__ volatile ("rdtsc" : "=A" (ret));
	return ret;
}


void usage(void)
{
	printf("mb, memory bench usage:\n"
	       "    -n nwrite     number of memory write ops in a loop\n"
	       "    -l nloop      number of loops\n"
	       "    -s size       size of a memory block for a write op\n"
	       "    -p pci        p2pmem slot, or hugepage\n"
	       "    -d direction  read or write\n"
		);
}

int main(int argc, char **argv)
{
	int ch, n, i, ret = 0;
	int nwrite, nloop, size;
	int dir;	/* 1 is write, 0 is read */
	char *mem, *src;
	void *addr, *target;
	pop_mem_t *pmem;
	pop_buf_t *pbuf;
	unsigned long long start, end;
	double *elapsed;

#define DIR_WRITE 1
#define DIR_READ 0
#define IS_DIR_WRITE(dir) (dir)
#define IS_DIR_READ(dir) (!(IS_DIR_WRITE(dir)))

	/* initialize */
	nwrite = 100;
	nloop = 100;
	size = 64;
	mem = NULL;
	dir = DIR_WRITE;

	while ((ch = getopt(argc, argv, "n:l:s:p:d:")) != -1) {
		switch (ch) {
		case 'n':
			nwrite = atoi(optarg);
			break;
		case 'l':
			nloop = atoi(optarg);
			break;
		case 's':
			size = atoi(optarg);
			break;
		case 'p':
			if (strncmp(optarg, "hugepage", 8) == 0)
				mem = NULL;
			else
				mem = optarg;
			break;
		case 'd':
			if (strncmp(optarg, "write", 5) == 0)
				dir = DIR_WRITE;
			else if (strncmp(optarg, "read", 4) == 0)
				dir = DIR_READ;
			else {
				fprintf(stderr, "invalid direction '%s'\n",
					optarg);
				return -1;
			}
			break;
		default:
			usage();
			return -1;
		}
	}

	printf("nwrite     %d\n", nwrite);
	printf("nloop      %d\n", nloop);
	printf("size       %d\n", size);
	printf("mem        %s\n", mem ? mem : "hugepage");
	printf("direction  %s\n", IS_DIR_WRITE(dir) ? "write" : "read");

	/* allocate memory */
	pmem = pop_mem_init(mem, 0);
	if (!pmem) {
		perror("pop_mem_init");
		ret = -1;
		goto out;
	}
	
	pbuf = pop_buf_alloc(pmem, size * nwrite);
	if (!pbuf) {
		perror("pop_buf_alloc");
		ret = -1;
		goto pop_mem_exit_out;
	}

	pop_buf_put(pbuf, size * nwrite);
	addr = pop_buf_data(pbuf);



	/* allocate source of copy and timestamps */
	src = calloc(nwrite, sizeof(char));
	memset(src, 1, nwrite * sizeof(char));

	elapsed = calloc(nloop, sizeof(*elapsed));

	/* start operation */
	for (n = 0; n < nloop; n++) {
		start = rdtsc();
		for (i = 0; i < nwrite; i++) {
			target = addr + nwrite * size;
			if (IS_DIR_WRITE(dir)) {
				memcpy(target, src, size);
				if (!mem) {
					__builtin___clear_cache(target,
								target + size);
				}
			} else {
				if (!mem) {
					__builtin___clear_cache(target,
								target + size);
				}
				memcpy(src, target, size);
			}
		}
		end = rdtsc();
		elapsed[n] = (double)(end - start) / (double)nloop;
	}

	/* sort */
	for (n = 0; n < nloop; n++) {
		for (i = nloop - 1; i > n; i--) {
			if (elapsed[i - 1] > elapsed[i]) {
				double tmp = elapsed[i];
				elapsed[i] = elapsed[i - 1];
				elapsed[i - 1] = tmp;
			}
		}
	}

	/* calculate and print output */
	/* all samples */
	double min = elapsed[0], max = elapsed[0];
	double sum = 0, avg = 0, dev = 0;

	for (n = 0; n < nloop; n++) {
		if (elapsed[n] < min)
			min = elapsed[n];
		if (elapsed[n] > max)
			max = elapsed[n];
		sum += elapsed[n];
	}

	avg = sum / nloop;

	for (n = 0; n < nloop; n++)
		dev += (elapsed[n] - avg) * (elapsed[n] - avg);
	dev = sqrt(dev / nloop);


	/* 9x% samples */
	double percentile = 0.99;
	int nloop9 = nloop * percentile;
	double min9 = elapsed[0], max9 = elapsed[0];
	double sum9 = 0, avg9 = 0, dev9 = 0;

	for (n = 0; n < nloop9; n++) {
		if (elapsed[n] < min)
			min9 = elapsed[n];
		if (elapsed[n] > max)
			max9 = elapsed[n];
		sum9 += elapsed[n];
	}

	avg9 = sum9 / nloop9;

	for (n = 0; n < nloop9; n++)
		dev9 += (elapsed[n] - avg9) * (elapsed[n] - avg9);
	dev9 = sqrt(dev9 / nloop9);


	printf("all samples, %d:\n", nloop);
	printf("min=%f max=%f avg=%f dev=%f\n", min, max, avg, dev);
	       
	printf("%.0f%% samples, %d:\n", percentile * 100, nloop9);
	printf("min=%f max=%f avg=%f dev=%f\n", min9, max9, avg9, dev9);
	       


pop_mem_exit_out:
	pop_mem_exit(pmem);
out:
	return ret;
}

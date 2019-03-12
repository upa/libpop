#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include <libpop.h>

void usage(void) {

	printf("usage: mem, testing pop_mem_t\n"
	       "    -b pci    PCI bus slot\n");
}

pop_mem_t *test_mem_init_will_success(char *dev, int size)
{
	pop_mem_t *mem;

	mem = pop_mem_init(dev, size);
	if (!mem)
		perror("init");

	assert(mem);
	printf("%lu-byte allocated on %s\n", pop_mem_size(mem),
	       (dev) ? dev : "hugepage");

	usleep(100000);
	return mem;
}

pop_mem_t *test_mem_init_will_fail(char *dev, int size)
{
	pop_mem_t *mem;

	mem = pop_mem_init(dev, size);
	if (!mem)
		perror("init fail");

	assert(mem);
	usleep(100000);
	return mem;
}

void test_mem_exit(pop_mem_t *mem) {
	int ret = pop_mem_exit(mem);
	if (ret != 0)
		perror("exit");
	assert(ret == 0);
}

int main(int argc, char **argv)
{
	int ch;
	char *pci = NULL;
	pop_mem_t *mem, *mem2;

	libpop_verbose_enable();

	while ((ch = getopt(argc, argv, "b:")) != -1) {

		switch (ch) {
		case 'b':
			pci = optarg;
			break;
		default:
			usage();
			return 1;
		}
	}

	if (!pci) {
		printf("-b option is required\n");
		return -1;
	}

	/* p2pmem on NoLoad */

	printf("\n= create p2pmem on %s: success\n", pci);
	mem = test_mem_init_will_success(pci, 0);
	test_mem_exit(mem);

	printf("\n= create 1MB p2pmem on %s: success\n", pci);
	mem = test_mem_init_will_success(pci, 1024 * 1024);
	test_mem_exit(mem);

	printf("\n= create p2pmem on %s twice: success\n", pci);
	printf("1st\n");
	mem = test_mem_init_will_success(pci, 0);
	printf("2nd\n");
	mem2 = test_mem_init_will_success(pci, 0);
	test_mem_exit(mem);


	/* hugepage */

	printf("\n= create mem on hugepage: success\n");
	mem = test_mem_init_will_success(NULL, 0);
	test_mem_exit(mem);

	printf("\n= create 1MB mem on hugepage: success\n");
	mem = test_mem_init_will_success(NULL, 1024 * 1024);
	test_mem_exit(mem);

	printf("\n= create mem on hugepage twice: success\n");
	printf("1st\n");
	mem = test_mem_init_will_success(NULL, 0);
	printf("2nd\n");
	mem2 = test_mem_init_will_success(NULL, 0);
	test_mem_exit(mem2);

	return 0;
}

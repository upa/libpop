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

void test_mem_init_will_success(pop_mem_t *mem, char *dev)
{
	int ret;
	ret = pop_mem_init(mem, dev);
	if (ret != 0)
		perror("init");

	assert(ret == 0);

	usleep(100000);
}

void test_mem_init_will_fail(pop_mem_t *mem, char *dev)
{
	int ret;

	ret = pop_mem_init(mem, dev);
	if (ret != 0)
		perror("init fail");

	assert(ret < 0);

	usleep(100000);
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
	pop_mem_t mem, mem2;

	libpop_verbose_enable();

	memset(&mem, 0, sizeof(mem));
	memset(&mem2, 0, sizeof(mem2));

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
	test_mem_init_will_success(&mem, pci);
	test_mem_exit(&mem);

	printf("\n= create p2pmem on %s twice: success\n", pci);
	printf("1st\n");
	test_mem_init_will_success(&mem, pci);
	printf("2nd\n");
	test_mem_init_will_success(&mem2, pci);
	test_mem_exit(&mem);


	/* hugepage */

	printf("\n= create mem on hugepage: success\n");
	test_mem_init_will_success(&mem, NULL);
	test_mem_exit(&mem);

	printf("\n= create mem on hugepage twice: success\n");
	printf("1st\n");
	test_mem_init_will_success(&mem, NULL);
	printf("2nd\n");
	test_mem_init_will_success(&mem2, NULL);
	test_mem_exit(&mem);

	return 0;
}

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

void test_mem_init_will_success(pop_mem_t *mem, char *dev, size_t size)
{
	int ret;
	ret = pop_mem_init(mem, dev, size);
	if (ret != 0)
		perror("init");

	assert(ret == 0);

	usleep(100000);
}

void test_mem_init_will_fail(pop_mem_t *mem, char *dev, size_t size)
{
	int ret;

	ret = pop_mem_init(mem, dev, size);
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
	size_t byte;

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

	/* p2pmem on NoLoad */

	byte = 4096;
	printf("\n= create %lu-byte p2pmem on %s: success\n", byte, pci);
	test_mem_init_will_success(&mem, pci, byte);
	test_mem_exit(&mem);

	byte = 65536;
	printf("\n= create %lu-byte p2pmem on %s: success\n", byte, pci);
	test_mem_init_will_success(&mem, pci, byte);
	test_mem_exit(&mem);

	byte = 4097;
	printf("\n= create %lu-byte p2pmem on %s: fail\n", byte, pci);
	test_mem_init_will_fail(&mem, pci, byte);

	byte = 4096;
	printf("\n= create %lu-byte p2pmem on %s twice: success\n", byte, pci);
	printf("1st\n");
	test_mem_init_will_success(&mem, pci, byte);
	printf("2nd\n");
	test_mem_init_will_success(&mem2, pci, byte);
	test_mem_exit(&mem);


	/* hugepage */

	byte = 4096;
	printf("\n= create %lu-byte mem on hugepage: success\n", byte);
	test_mem_init_will_success(&mem, NULL, byte);
	test_mem_exit(&mem);

	byte = 65536;
	printf("\n= create %lu-byte mem on hugepage: success\n", byte);
	test_mem_init_will_success(&mem, NULL, byte);
	test_mem_exit(&mem);

	byte = 4097;
	printf("\n= create %lu-byte mem on hugepage: fail\n", byte);
	test_mem_init_will_fail(&mem, NULL, byte);
	test_mem_exit(&mem);

	byte = 4096;
	printf("\n= create %lu-byte mem on hugepage twice: success\n", byte);
	test_mem_init_will_success(&mem, NULL, byte);
	test_mem_init_will_success(&mem2, NULL, byte);
	test_mem_exit(&mem);

	return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include <libpop.h>

void usage(void) {

	printf("usage: ctx, testing pop_ctx_t\n"
	       "    -b pci    PCI bus slot\n");
}

void test_ctx_init_will_success(pop_ctx_t *ctx, char *dev, size_t size)
{
	int ret;
	ret = pop_ctx_init(ctx, dev, size);
	if (ret != 0)
		perror("init");

	assert(ret == 0);

	usleep(100000);
}

void test_ctx_init_will_fail(pop_ctx_t *ctx, char *dev, size_t size)
{
	int ret;

	ret = pop_ctx_init(ctx, dev, size);
	if (ret != 0)
		perror("init fail");

	assert(ret < 0);

	usleep(100000);
}

void test_ctx_exit(pop_ctx_t *ctx) {
	int ret = pop_ctx_exit(ctx);
	if (ret != 0)
		perror("exit");
	assert(ret == 0);
}

int main(int argc, char **argv)
{
	int ch;
	char *pci = NULL;
	pop_ctx_t ctx, ctx2;
	size_t byte;

	memset(&ctx, 0, sizeof(ctx));
	memset(&ctx2, 0, sizeof(ctx2));

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
	test_ctx_init_will_success(&ctx, pci, byte);
	test_ctx_exit(&ctx);

	byte = 65536;
	printf("\n= create %lu-byte p2pmem on %s: success\n", byte, pci);
	test_ctx_init_will_success(&ctx, pci, byte);
	test_ctx_exit(&ctx);

	byte = 4097;
	printf("\n= create %lu-byte p2pmem on %s: fail\n", byte, pci);
	test_ctx_init_will_fail(&ctx, pci, byte);

	byte = 4096;
	printf("\n= create %lu-byte p2pmem on %s twice: fail\n", byte, pci);
	test_ctx_init_will_success(&ctx, pci, byte);
	test_ctx_init_will_fail(&ctx2, pci, byte);
	test_ctx_exit(&ctx);


	/* hugepage */

	byte = 4096;
	printf("\n= create %lu-byte mem on hugepage: success\n", byte);
	test_ctx_init_will_success(&ctx, NULL, byte);
	test_ctx_exit(&ctx);

	byte = 65536;
	printf("\n= create %lu-byte mem on hugepage: success\n", byte);
	test_ctx_init_will_success(&ctx, NULL, byte);
	test_ctx_exit(&ctx);

	byte = 4097;
	printf("\n= create %lu-byte mem on hugepage: fail\n", byte);
	test_ctx_init_will_fail(&ctx, NULL, byte);
	test_ctx_exit(&ctx);

	byte = 4096;
	printf("\n= create %lu-byte mem on hugepage twice: success\n", byte);
	test_ctx_init_will_success(&ctx, NULL, byte);
	test_ctx_init_will_success(&ctx2, NULL, byte);
	test_ctx_exit(&ctx);

	return 0;
}

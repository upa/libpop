#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include <libpop.h>
#include <unvme.h>

enum {
	UNVME_READ	= 1,
	UNVME_WRITE	= 2,
};


void hexdump(void *buf, int len)
{
	int n;
	unsigned char *p = buf;

	printf("Hex dump\n");

	for (n = 0; n < len; n++) {
		printf("%02x", p[n]);

		if ((n + 1) % 2 == 0)
			printf(" ");
		if ((n + 1) % 32 == 0)
			printf("\n");
	}
	printf("\n\n");
}


void usage(void)
{
	printf("usage: test_unvme_open\n"
	       "    -b pci        PCI bus for p2pmem\n"
	       "    -u pci        NVMe under UNVMe\n"
	       "    -s slba       start Logical Block Address\n"
	       "    -n nblocks    number of blocks to read/write\n"
	       "    -c cmd        read or write\n"
	       "    -d data       data to be write in HEX\n"
	       "    -q queue      queue number\n"
	       "    -r repeat     repeat the cmd as async\n"
	       "    -H            no hex dump\n"
		);
}

int main(int argc, char **argv)
{
	int ch, ret, n;
	char *pci = NULL;
	char *nvme = NULL;
	u64 slba = 1;
	int nblocks = 1;
	int cmd = UNVME_READ;
	char data[4096 * 8];
	int nohex = 0;
	int data_exist = 0;

	int q = 0;
	int r = 0;

	memset(data, 0, sizeof(data));
	libpop_verbose_enable();

	while ((ch = getopt(argc, argv, "b:u:s:n:c:d:q:r:H")) != -1) {
		
		switch (ch) {
		case 'b':
			pci = optarg;
			break;
		case 'u':
			nvme = optarg;
			break;
		case 's':
			slba = atoi(optarg);
			break;
		case 'n':
			nblocks = atoi(optarg);
			break;
		case 'c':
			if (strncmp(optarg, "read", 4) == 0)
				cmd = UNVME_READ;
			else if (strncmp(optarg, "write", 5) == 0)
				cmd = UNVME_WRITE;
			else {
				fprintf(stderr, "invalid cmd %s\n", optarg);
				return -1;
			}
			break;
		case 'd':
			strncpy(data, optarg, sizeof(data));
			data_exist = 1;
			break;
		case 'q':
			q = atoi(optarg);
			break;
		case 'r':
			r = atoi(optarg);
			break;
		case 'H':
			nohex = 1;
			break;
		default:
			usage();
			return -1;
		}
	}

	const unvme_ns_t *unvme;
	pop_mem_t *mem;
	pop_buf_t *pbuf;
	char *buf;



	/* open unvme */
	if (!nvme) {
		fprintf(stderr, "specify -u, pci slot for unvme device\n");
		return -1;
	}

	printf("strat unvme_open()\n");
	unvme = unvme_open(nvme);
	if (!unvme)
		perror("unvme_open");
	assert(unvme);

	printf("block size is %d\n", unvme->blocksize);

	/* allocate pop mem */
	mem= pop_mem_init(pci, unvme->blocksize * nblocks);
	if (mem)
		perror("pop_mem_init");
	assert(mem);

	printf("register pop mem to unvme\n");
	unvme_register_pop_mem(mem);

	printf("alloc pbuf\n");
	pbuf = pop_buf_alloc(mem, unvme->blocksize * nblocks);
	pop_buf_put(pbuf, unvme->blocksize * nblocks);
	assert(pbuf);
	buf = pop_buf_data(pbuf);

	/* execute nvme command */
	printf("start to execute command\n");
	printf("slba     %lx\n", slba);
	printf("nblocks  %u\n", nblocks);
	printf("buf      %p\n", buf);
	printf("paddr    0x%lx\n", pop_buf_paddr(pbuf));
	printf("queue    %d\n", q);
	printf("repeat   %d\n", r);

	switch (cmd) {
	case UNVME_READ:
		if (!r) {
			ret = unvme_read(unvme, q, buf, slba, nblocks);
			printf("unvme_read returns %d\n", ret);
		} else {
			for (n = 0; n < r; n++)
				unvme_aread(unvme, q, buf, slba, nblocks);
		}

		if (!nohex) {
			printf("dump 256-byte of buf\n");
			hexdump(buf, 256);
		}
		break;

	case UNVME_WRITE:
		if (data_exist)
			strncpy(buf, data, nblocks * unvme->blocksize);

		if (!r) {
			ret = unvme_write(unvme, q, buf, slba, nblocks);
			printf("unvme_write returns %d\n", ret);
		} else {
			for (n = 0; n < r; n++)
				unvme_awrite(unvme, q, buf, slba, nblocks);
		}

		if (!nohex) {
			printf("dump 256-byte of buf\n");
			hexdump(buf, 256);
		}
		break;

	default:
		fprintf(stderr, "stderr invalid cmd\n");
		ret = -1;
		break;
	}

	unvme_close(unvme);
	pop_mem_exit(mem);

	return ret;
}

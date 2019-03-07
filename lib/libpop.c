/*
 * libpop.c: A library for Peripheral-to-Peripheral communication.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <x86_64-linux-gnu/sys/user.h>

#include <libpop.h>

/* useful macro */

#define PROGNAME	"libpop"
#define DEVPOP		"/dev/pop/pop"

/* print success (green) */
#define pr_s(fmt, ...) \
	fprintf(stderr,							\
		"\x1b[1m\x1b[32m" PROGNAME ":%d:%s(): " fmt		\
		"\x1b[0m\n",						\
		__LINE__, __func__, ##__VA_ARGS__)

/* print error (red) */
#define pr_e(fmt, ...) \
	fprintf(stderr,							\
		"\x1b[1m\x1b[31m" PROGNAME ":%d:%s(): " fmt		\
		"\x1b[0m\n",						\
		__LINE__, __func__, ##__VA_ARGS__)

#define pr_vs(fmt, ...)					\
	if (ctx->verbose) { pr_s(fmt, ##__VA_ARGS__); }

#define pr_ve(fmt, ...)					\
	if (ctx->verbose) { pr_e(fmt, ##__VA_ARGS__); }


int pop_ctx_init(pop_ctx_t *ctx, char *dev, size_t size)
{
	/*
	 * register dev and its p2pmem through /dev/pop/pop
	 */

	int ret, fd ,flags;
	int verbose = ctx->verbose;
	char popdev[32];

	memset(ctx, 0, sizeof(*ctx));
	ctx->size	= size;
	ctx->num_pages	= size >> PAGE_SHIFT;
	ctx->verbose	= verbose;
	
	if (dev == NULL) {
		/* hugepage */
		strncpy(ctx->devname, "hugepage", POP_PCI_DEVNAME_MAX);
		flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED | MAP_HUGETLB;
		ctx->fd = -1;
	} else {
		/* pop device. register it to /dev/pop/pop */
		strncpy(ctx->devname, dev, POP_PCI_DEVNAME_MAX);
		ret = sscanf(dev, "%x:%x:%x.%x",
			     &ctx->reg.domain, &ctx->reg.bus,
			     &ctx->reg.slot, &ctx->reg.func);
		if (ret < 4) {
			ctx->reg.domain = 0;
			ret = sscanf(dev, "%x:%x.%x",
				     &ctx->reg.bus,
				     &ctx->reg.slot, &ctx->reg.func);
		}
		if (ret < 3) {
			pr_ve("invalid pci slot %s\n", dev);
			errno = EINVAL;
			return -1;
		}

		ctx->reg.size = size;

		fd = open(DEVPOP, O_RDWR);
		if (fd < 0) {
			pr_ve("failed to open %s\n", DEVPOP);
			return -1;
		}

		ret = ioctl(fd, POP_P2PMEM_REG, &ctx->reg);
		if (ret != 0) {
			pr_ve("failed to register p2pmem on %s\n", dev);
			close(fd);
			return -1;
		}
		close(fd);


		/* open /dev/pop/PCI_DEV for mmap() */
		snprintf(popdev, sizeof(popdev), "/dev/pop/%04x:%02x:%02x.%x",
			 ctx->reg.domain, ctx->reg.bus,
			 ctx->reg.slot, ctx->reg.func);
		ctx->fd = open(popdev, O_RDWR);
		if (ctx->fd < 0) {
			pr_ve("failed to open %s\n", popdev);
			return -1;
		}

		flags = MAP_LOCKED | MAP_SHARED;
		
	}
	
	/* XXX: handle offset */
	ctx->mem = mmap(0, size, PROT_READ | PROT_WRITE, flags, ctx->fd, 0);
	if (ctx->mem == MAP_FAILED) {
		pr_ve("failed to mmap on %s\n", ctx->devname);
		if (ctx->fd != -1)
			close(ctx->fd);
	}

	return 0;
}


int pop_ctx_exit(pop_ctx_t *ctx)
{
	/* unregister dev and its p2pmem through /dev/pop/pop */
	
	int ret;

	if (ctx->fd == -1) {
		/* hugepage */
		ret = munmap(ctx->mem, ctx->size);
		if (ret != 0) {
			pr_ve("failed to munmap on %s\n", ctx->devname);
			return -1;
		}
	} else {
		/* pop device. unregister it through /dev/pop/pop */
		ret = ioctl(ctx->fd, POP_P2PMEM_UNREG, &ctx->reg);
		if (ret != 0) {
			pr_ve("failed to unregister %s\n", ctx->devname);
			return -1;
		}
		return close(ctx->fd);
	}

	return 0;
}


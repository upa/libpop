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


/* prototypes for internal uses */
static uintptr_t virt_to_phys(pop_ctx_t *ctx, void *addr);


/* context operations  */

int pop_ctx_init(pop_ctx_t *ctx, char *dev, size_t size)
{
	/*
	 * register dev and its p2pmem through /dev/pop/pop
	 */

	int ret, fd ,flags;
	int verbose = ctx->verbose;
	char popdev[32];

	/* validation */
	if (size % (1 << PAGE_SHIFT) || size < (1 << PAGE_SHIFT)) {
		pr_ve("size must be power of %d", 1 << PAGE_SHIFT);
		return -1;
	}

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
			pr_ve("invalid pci slot %s", dev);
			errno = EINVAL;
			return -1;
		}

		ctx->reg.size = size;

		fd = open(DEVPOP, O_RDWR);
		if (fd < 0) {
			pr_ve("failed to open %s", DEVPOP);
			return -1;
		}

		ret = ioctl(fd, POP_P2PMEM_REG, &ctx->reg);
		if (ret != 0) {
			pr_ve("failed to register p2pmem on %s", dev);
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
			pr_ve("failed to open %s", popdev);
			return -1;
		}

		flags = MAP_LOCKED | MAP_SHARED;
		
	}
	
	/* XXX: handle offset */
	ctx->mem = mmap(0, ctx->size, PROT_READ | PROT_WRITE,
			flags, ctx->fd, 0);
	if (ctx->mem == MAP_FAILED) {
		pr_ve("failed to mmap on %s", ctx->devname);
		if (ctx->fd != -1)
			close(ctx->fd);
		return -1;
	}

	return 0;
}


int pop_ctx_exit(pop_ctx_t *ctx)
{
	/* unregister dev and its p2pmem through /dev/pop/pop */
	
	int ret, fd;

	if (ctx->fd == -1) {
		/* hugepage */
#if 0
		/* XXX: should munmap, but it fails... */
		ret = munmap(ctx->mem, ctx->size);
		if (ret != 0) {
			pr_ve("failed to munmap on %s", ctx->devname);
			return -1;
		}
#endif
	} else {
		/* pop device. unregister it through /dev/pop/pop */

		fd = open(DEVPOP, O_RDWR);
		if (fd < 0) {
			pr_ve("failed to open %s", DEVPOP);
			return -1;
		}

		ret = ioctl(fd, POP_P2PMEM_UNREG, &ctx->reg);
		if (ret != 0) {
			pr_ve("failed to unregister %s", ctx->devname);
			return -1;
		}
		return close(ctx->fd);
	}

	return 0;
}


/* pop_buf operations */

pop_buf_t *pop_buf_alloc(pop_ctx_t *ctx, size_t size)
{
	pop_buf_t *pbuf;

	/* XXX: should lock! */

	/* validation */
	if (size % (1 << PAGE_SHIFT) || size < (1 << PAGE_SHIFT)) {
		pr_ve("size must be power of %d", 1 << PAGE_SHIFT);
		return NULL;
	}
	    
	if ((ctx->num_pages - ctx->alloced_pages) < (size >> PAGE_SHIFT)) {
		pr_ve("no page available on %s", ctx->devname);
		errno = ENOBUFS;
		return NULL;
	}

	pbuf = malloc(sizeof(*pbuf));
	if (!pbuf) {
		pr_ve("failed to allocate pop_buf structure");
		return NULL;
	}

	memset(pbuf, 0, sizeof(*pbuf));
	pbuf->ctx	= ctx;
	pbuf->vaddr	= ctx->mem + ((ctx->alloced_pages << PAGE_SHIFT));
	pbuf->paddr	= virt_to_phys(ctx, pbuf->vaddr);
	pbuf->size	= size;
	pbuf->offset	= 0;
	pbuf->length	= 0;

	ctx->alloced_pages += size >> PAGE_SHIFT;

	return pbuf;
}

void pop_buf_free(pop_buf_t *pbuf)
{
	/* ToDo: hehe... */

	free(pbuf);
}

inline void *pop_buf_data(pop_buf_t *pbuf)
{
	return pbuf->vaddr + pbuf->offset;
}

inline void *pop_buf_put(pop_buf_t *pbuf, size_t len)
{
	pop_ctx_t *ctx = pbuf->ctx;

	if (pbuf->offset + pbuf->length + len > pbuf->size) {
		pr_ve("failed to put: size=%lu off=%lu length=%lu putlen=%lu",
		      pbuf->size, pbuf->offset, pbuf->length, len);
		return NULL;
	}

	pbuf->length += len;
	return pop_buf_data(pbuf);
}

inline void *pop_buf_trim(pop_buf_t *pbuf, size_t len)
{
	pop_ctx_t *ctx = pbuf->ctx;

	if (pbuf->length < len) {
		pr_ve("failed to trim: size=%lu off=%lu length=%lu putlen=%lu",
		      pbuf->size, pbuf->offset, pbuf->length, len);
		return NULL;
	}

	pbuf->length -= len;
	return pop_buf_data(pbuf);
}

inline void *pop_buf_pull(pop_buf_t *pbuf, size_t len)
{
	pop_ctx_t *ctx = pbuf->ctx;

	if (pbuf->length < len) {
		pr_ve("failed to pull: size=%lu off=%lu length=%lu putlen=%lu",
		      pbuf->size, pbuf->offset, pbuf->length, len);
		return NULL;
	}

	pbuf->length -= len;
	pbuf->offset += len;
	return pop_buf_data(pbuf);
}

inline void *pop_buf_push(pop_buf_t *pbuf, size_t len)
{
	pop_ctx_t *ctx = pbuf->ctx;

	if (pbuf->offset < len) {
		pr_ve("failed to push: size=%lu off=%lu length=%lu putlen=%lu",
		      pbuf->size, pbuf->offset, pbuf->length, len);
		return NULL;
	}

	pbuf->length += len;
	pbuf->offset -= len;
	return pop_buf_data(pbuf);
}

inline size_t pop_buf_len(pop_buf_t *pbuf)
{
	return pbuf->length - pbuf->offset;
}

/* for debaug use */
void print_pop_buf(pop_buf_t *pbuf)
{
	fprintf(stderr, "ctx:              %p\n", pbuf->ctx);
	fprintf(stderr, " - devname:       %s\n", pbuf->ctx->devname);
	fprintf(stderr, " - alloced_pages: %lu\n", pbuf->ctx->alloced_pages);
	fprintf(stderr, "vaddr:            %p\n", pbuf->vaddr);
	fprintf(stderr, "paddr:            0x%lx\n", pbuf->paddr);
	fprintf(stderr, "size:             %lu\n", pbuf->size);
	fprintf(stderr, "offset:           %lu\n", pbuf->offset);
	fprintf(stderr, "length:           %lu\n", pbuf->length);
}

/* internal uses */

inline uintptr_t pop_buf_paddr(pop_buf_t *pbuf)
{
	return pbuf->paddr + pbuf->offset;
}


static uintptr_t virt_to_phys(pop_ctx_t *ctx, void *addr)
{
	int fd;
	long pagesize;
	off_t ret;
	ssize_t rc;
	uintptr_t entry = 0;

	fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0) {
		pr_ve("open /proc/self/pagemap: %s", strerror(errno));
		return 0;
	}

	pagesize = sysconf(_SC_PAGESIZE);

	ret = lseek(fd, (uintptr_t)addr / pagesize * sizeof(uintptr_t),
		    SEEK_SET);
	if (ret < 0) {
		pr_ve("lseek for /proc/self/pagemap: %s", strerror(errno));
		goto err_out;
	}

	rc = read(fd, &entry, sizeof(entry));
	if (rc < 1 || entry == 0) {
		pr_ve("read for /proc/self/pagemap: %s", strerror(errno));
		goto err_out;
	}

	close(fd);

	return (entry & 0x7fffffffffffffULL) * pagesize +
		((uintptr_t)addr) % pagesize;

err_out:
	close(fd);
	return 0;
}

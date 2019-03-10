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
#include <libpop_util.h>

#define PROGNAME	"libpop"
#define DEVPOP		"/dev/pop/pop"

int libpop_verbose = 0;	/* global in libpop */

void libpop_verbose_enable(void) {
	libpop_verbose = 1;
}

void libpop_verbose_disable(void) {
	libpop_verbose = 0;
}

/* prototypes for internal uses */

#define NR_HUGEPAGES \
	"/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages"
#define HUGEPAGE_SIZE (2 * 1024 * 1024)
	

static int get_nr_hugepages(void)
{
	int fd;
	char buf[16];

	fd = open(NR_HUGEPAGES, O_RDONLY);
	if (fd < 0) {
		pr_ve("failed to open %s", NR_HUGEPAGES);
		return -1;
	}

	read(fd, buf, sizeof(buf));
	return atoi(buf);
}

/* memory operations  */

int pop_mem_init(pop_mem_t *mem, char *dev)
{
	/*
	 * register dev and its p2pmem through /dev/pop/pop
	 */

	int ret, fd ,flags;
	char popdev[32];

	/* validation */
	memset(mem, 0, sizeof(*mem));

	if (dev == NULL) {
		/* allocate all hugepages  */
		int nr_pages;

		strncpy(mem->devname, "hugepage", POP_PCI_DEVNAME_MAX);

		nr_pages  = get_nr_hugepages();
		if (nr_pages < 0) {
			pr_ve("failed to get num of hugepages");
			return -1;
		}

		flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED | MAP_HUGETLB;
		printf("hugepage num is %d\n", nr_pages);
		mem->size = 2 * 1024 * 1024 * (nr_pages / 4) ;
		mem->num_pages = mem->size >> PAGE_SHIFT; /* 4kb align */
		mem->fd = -1;
	} else {
		/* pop device. register it through /dev/pop/pop */
		strncpy(mem->devname, dev, POP_PCI_DEVNAME_MAX);
		ret = sscanf(dev, "%x:%x:%x.%x",
			     &mem->reg.domain, &mem->reg.bus,
			     &mem->reg.slot, &mem->reg.func);
		if (ret < 4) {
			mem->reg.domain = 0;
			ret = sscanf(dev, "%x:%x.%x",
				     &mem->reg.bus,
				     &mem->reg.slot, &mem->reg.func);
		}
		if (ret < 3) {
			pr_ve("invalid pci slot %s", dev);
			errno = EINVAL;
			return -1;
		}

		fd = open(DEVPOP, O_RDWR);
		if (fd < 0) {
			pr_ve("failed to open %s", DEVPOP);
			return -1;
		}

		ret = ioctl(fd, POP_P2PMEM_REG, &mem->reg);
		if (ret != 0) {
			pr_ve("failed to register p2pmem on %s", dev);
			close(fd);
			return -1;
		}
		close(fd);

		/* open /dev/pop/PCI_DEV for mmap() */
		snprintf(popdev, sizeof(popdev), "/dev/pop/%04x:%02x:%02x.%x",
			 mem->reg.domain, mem->reg.bus,
			 mem->reg.slot, mem->reg.func);
		mem->fd = open(popdev, O_RDWR);
		if (mem->fd < 0) {
			pr_ve("failed to open %s", popdev);
			return -1;
		}

		flags = MAP_LOCKED | MAP_SHARED;
		mem->size = mem->reg.size;		
		mem->num_pages = mem->size >> PAGE_SHIFT;
	}
	
	/* XXX: handle offset */
	mem->mem = mmap(0, mem->size, PROT_READ | PROT_WRITE,
			flags, mem->fd, 0);
	if (mem->mem == MAP_FAILED) {
		pr_ve("failed to mmap on %s", mem->devname);
		if (mem->fd != -1)
			close(mem->fd);
		return -1;
	}

	pr_vs("%lu-byte mmaped on %s", mem->size, mem->devname);

	return 0;
}


int pop_mem_exit(pop_mem_t *mem)
{
	/* unregister dev and its p2pmem through /dev/pop/pop */
	
	int ret, fd;

	if (mem->fd == -1) {
		/* hugepage */
#if 0
		/* XXX: should munmap, but it fails... */
		ret = munmap(mem->mem, mem->size);
		if (ret != 0) {
			pr_ve("failed to munmap on %s", mem->devname);
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

		ret = ioctl(fd, POP_P2PMEM_UNREG, &mem->reg);
		if (ret != 0) {
			pr_ve("failed to unregister %s", mem->devname);
			return -1;
		}
		return close(mem->fd);
	}

	return 0;
}


/* pop_buf operations */

pop_buf_t *pop_buf_alloc(pop_mem_t *mem, size_t size)
{
	pop_buf_t *pbuf;
	size_t nr_pages;

	/* XXX: should lock! */

	for (nr_pages = 0; (nr_pages << PAGE_SHIFT) < size; nr_pages++);

	if ((mem->num_pages - mem->alloced_pages) < nr_pages) {
		pr_ve("no page available on %s", mem->devname);
		errno = ENOBUFS;
		return NULL;
	}

	pbuf = malloc(sizeof(*pbuf));
	if (!pbuf) {
		pr_ve("failed to allocate pop_buf structure");
		return NULL;
	}

	memset(pbuf, 0, sizeof(*pbuf));
	pbuf->mem	= mem;
	pbuf->vaddr	= mem->mem + ((mem->alloced_pages << PAGE_SHIFT));
	pbuf->paddr	= virt_to_phys(pbuf->vaddr);
	pbuf->size	= nr_pages << PAGE_SHIFT;
	pbuf->offset	= 0;
	pbuf->length	= 0;

	mem->alloced_pages += nr_pages;

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

inline uintptr_t pop_buf_paddr(pop_buf_t *pbuf)
{
	return pbuf->paddr + pbuf->offset;
}

inline void *pop_buf_put(pop_buf_t *pbuf, size_t len)
{
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
	fprintf(stderr, "mem:              %p\n", pbuf->mem);
	fprintf(stderr, " - devname:       %s\n", pbuf->mem->devname);
	fprintf(stderr, " - alloced_pages: %lu\n", pbuf->mem->alloced_pages);
	fprintf(stderr, "vaddr:            %p\n", pbuf->vaddr);
	fprintf(stderr, "paddr:            0x%lx\n", pbuf->paddr);
	fprintf(stderr, "size:             %lu\n", pbuf->size);
	fprintf(stderr, "offset:           %lu\n", pbuf->offset);
	fprintf(stderr, "length:           %lu\n", pbuf->length);
}




uintptr_t virt_to_phys(void *addr)
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

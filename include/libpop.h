/*
 * libpop.h: A library for Peripheral-to-Peripheral communication.
 */

#ifndef _LIBPOP_H_
#define _LIBPOP_H_

#ifndef __KERNEL__
#include <stdio.h>
#include <stdint.h>	/* uintptr_t */
#endif /* __KERNEL__ */


/*
 * ioctl for /dev/pop/pop. It is only shared with kernel
 */

struct pop_p2pmem_reg {

	/* PCIe slot for target device */
	int	domain;
	int	bus;
	int	slot;
	int	func;
};

#define POP_P2PMEM_REG		_IOW('i', 1, struct pop_p2pmem_reg)
#define POP_P2PMEM_UNREG	_IOW('i', 2, struct pop_p2pmem_reg)



#ifndef __KERNEL__	/* start userland definition here */

/*
 * libpop
 */

/* util */
void libpop_verbose_enable(void);
void libpop_verbose_disable(void);

/* structure describing pop p2pmem context */
#define POP_PCI_DEVNAME_MAX	16
typedef struct pop_mem {
	int	fd;			/* fd for ioctl and mmap	*/
	char	devname[POP_PCI_DEVNAME_MAX];	/* '\0' means hugepage	*/
	struct pop_p2pmem_reg reg;	/* reg for ioctl		*/

	void	*mem;			/* mmaped region		*/
	size_t	size;			/* size of allocated region	*/
	size_t	num_pages;		/* # of pages 		*/
	size_t	alloced_pages;       	/* # of allocated pages	*/
} pop_mem_t;


/*
 * pop_mem_init()
 *
 * Register p2pmem pci device or hugepage into libpop memory
 * context. On success, zero is returned. On error, -1 is returned,
 * and errno is set appropriately.
 *
 * mem:  libpop memory context.
 * dev:  string for PCI slot num, or NULL means hugepages.
 * size: size of allocated memory (must be power of page size).
 */
int pop_mem_init(pop_mem_t *mem, char *dev, size_t size);
int pop_mem_exit(pop_mem_t *mem);




/* structure describing pop buffer on p2pmem */
typedef struct pop_buf {
	pop_mem_t	*mem;	/* parent pop context  */

	void		*vaddr;	/* virtual address on mmap region	*/
	uintptr_t	paddr;	/* physical addres of the vaddr		*/
	size_t		size;	/* size of allocated mmap region	*/

	size_t		offset;	/* offset of data	*/
	size_t		length;	/* length of data	*/

	/* driver specific parameters */
	uint64_t	lba;	/* Logical Address Block on NVMe*/
	int		ret;	/* pop_write/read ret value for this buf */
} pop_buf_t;

/* operating pop_buf like sk_buff */
pop_buf_t *pop_buf_alloc(pop_mem_t *mem, size_t size);
void pop_buf_free(pop_buf_t *pbuf);

void *pop_buf_data(pop_buf_t *pbuf);
size_t pop_buf_len(pop_buf_t *pbuf);
uintptr_t pop_buf_paddr(pop_buf_t *pbuf);

void *pop_buf_put(pop_buf_t *pbuf, size_t len);
void *pop_buf_trim(pop_buf_t *pbuf, size_t len);
void *pop_buf_pull(pop_buf_t *pbuf, size_t len);
void *pop_buf_push(pop_buf_t *pbuf, size_t len);


/* debug use */
void print_pop_buf(pop_buf_t *pbuf);




/* driver and i/o operations */

#define POP_DRIVER_TYPE_NETMAP	1
#define	POP_DRIVER_TYPE_UNVME	2

/* describing underlay driver */
typedef struct pop_driver pop_driver_t;
struct pop_driver {

	int type;

	int (*pop_driver_write)(pop_driver_t *drv, pop_buf_t **pbufs,
				int nbufs, int qid);
	int (*pop_driver_read)(pop_driver_t *drv, pop_buf_t **pbufs,
			       int nbufs, int qid);
	int (*pop_driver_poll)(pop_driver_t *drv, int qid);

	void *data;

};

int pop_driver_init(pop_driver_t *drv, int type, void *arg);
int pop_driver_exit(pop_driver_t *drv);

/*
 * pop_read/write()
 *
 * read/write data in pbuf from/to underlay driver. Retrusn value is
 * number of pbufs for read/write. Thus, ret < nbufs means underlay
 * driver has no more available data (read) or buffer (read). If any
 * error occurs, they return -1 and errno is set appropriately
 *
 * drv:	  driver structure describing underlay driver.
 * pbufs: an array of pop_buf_t.
 * nbufs: number of the array of pop_buf_t *.
 * qid:   queue id on underlay driver, e.g., NIC ring or NVMe SQ on CPU.
 */
int pop_read(pop_driver_t *drv, pop_buf_t **pbufs, int nbufs, int qid);
int pop_write(pop_driver_t *drv, pop_buf_t **pbufs, int nbufs, int qid);
int pop_poll(pop_driver_t *drv, int qid); /* currently only for netmap */

#endif /* __KERNEL__ */
#endif /* _LIBPOP_H_ */

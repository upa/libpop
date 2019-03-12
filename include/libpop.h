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

	/* parameters that kernel returns */
	size_t	size;
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

	void		*mem;		/* mmaped region		*/
	uintptr_t	paddr;		/* physical addr of mem		*/

	size_t	size;			/* size of allocated region	*/
	size_t	num_pages;		/* # of pages this mem has	*/
	size_t	alloced_pages;       	/* # of allocated pages	from this */
} pop_mem_t;


/*
 * pop_mem_init()
 *
 * Register p2pmem pci device or hugepage into libpop memory
 * context. On success, pop_mem_t* is returned. On error, NULL is
 * returned, and errno is set appropriately.
 *
 * dev:  string for PCI slot num, or NULL means hugepages.
 * size: size of allocated memory in byte, 0 means all
 */
pop_mem_t *pop_mem_init(char *dev, size_t size);
int pop_mem_exit(pop_mem_t *mem);

size_t pop_mem_size(pop_mem_t *mem);


/* structure describing pop buffer on p2pmem */
typedef struct pop_buf {
	pop_mem_t	*mem;	/* parent pop context  */

	void		*vaddr;	/* virtual address on mmap region	*/
	uintptr_t	paddr;	/* physical addres of the vaddr		*/
	size_t		size;	/* allocated size for this pop_buf	*/

	size_t		offset;	/* offset of data	*/
	size_t		length;	/* length of data	*/
} pop_buf_t;

/* operating pop_buf like sk_buff */
pop_buf_t *pop_buf_alloc(pop_mem_t *mem, size_t size);
void pop_buf_free(pop_buf_t *pbuf);

void *pop_buf_data(pop_buf_t *pbuf);
size_t pop_buf_len(pop_buf_t *pbuf);
uintptr_t pop_buf_paddr(pop_buf_t *pbuf);
uintptr_t pop_virt_to_phys(pop_mem_t *mem, void *vaddr);

void *pop_buf_put(pop_buf_t *pbuf, size_t len);
void *pop_buf_trim(pop_buf_t *pbuf, size_t len);
void *pop_buf_pull(pop_buf_t *pbuf, size_t len);
void *pop_buf_push(pop_buf_t *pbuf, size_t len);



/* debug use */
void print_pop_buf(pop_buf_t *pbuf);
uintptr_t virt_to_phys(void *addr);


/***  netmap helper functions ****/
#include <net/if.h>
#include <net/netmap.h>

/*** For RX packets through netmap to p2p memory ***/

struct pop_nm_rxring {
	pop_buf_t		*pbuf;	/* p2pmem for this rxring */
	struct netmap_ring	*ring;
};

/* pop_nm_rxring_init: correlating netmap rx ring with p2p memory */
struct pop_nm_rxring *pop_nm_rxring_init(int fd, struct netmap_ring *ring,
					 pop_mem_t *mem);
void pop_nm_rxring_exit(struct pop_nm_rxring *prxring);

/* pop_nm_rx_ring_buf: obtain packet buffer from rxring correlated to
 * the p2p memory. note that 'idx' is not buf_idx in
 * netmap_slot. index for the ring (usually, ring->head).
 */
void *pop_nm_rxring_buf(struct pop_nm_rxring *prxring, unsigned int idx);

/* pop_nm_ring: obtain netmap_ring from pop_nm_rxring */
struct netmap_ring *pop_nm_ring(struct pop_nm_rxring *prxring);


/*** For TX packets through netmap from p2p memory ***/

/* pop_nm_set_buf: set p2p memory to specified netmap slot for TX */
void pop_nm_set_buf(struct netmap_slot *slot, pop_buf_t *pbuf);




#endif /* __KERNEL__ */
#endif /* _LIBPOP_H_ */

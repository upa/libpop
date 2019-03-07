/*
 * libpop.h: A library for Peripheral-to-Peripheral communication.
 */

#ifndef _LIBPOP_H_
#define _LIBPOP_H_


/*
 * ioctl for /dev/pop/pop.
 */

struct pop_p2pmem_reg {

	/* PCIe slot for target device */
	int	domain;
	int	bus;
	int	slot;
	int	func;

	/* information for p2pmem region */
	size_t	size;
};

#define POP_P2PMEM_REG		_IOW('i', 1, struct pop_p2pmem_reg)
#define POP_P2PMEM_UNREG	_IOW('i', 2, struct pop_p2pmem_reg)



/*
 * libpop
 */

/* structure describing pop p2pmem context */
#define POP_PCI_DEVNAME_MAX	16
struct pop_ctx {
	int	fd;			/* fd for ioctl and mmap	*/
	char	devname[POP_PCI_DEVNAME_MAX];	/* '\0' means hugepage	*/
	struct pop_p2pmem_reg reg;	/* reg for ioctl		*/

	void	*mem;			/* mmaped region		*/
	size_t	size;			/* size of allocated region	*/
	size_t	num_pages;		/* # of pages 		*/
	size_t	allocated_pages;       	/* # of allocated pages	*/

	/* options */
	int	verbose;		/* verbose mode */
};
typedef struct pop_ctx pop_ctx_t;

/*
 * pop_ctx_init()
 *
 * Register p2pmem pci device or hugepage into libpop context. On
 * success, zero is returned. On error, -1 is returned, and errno is
 * set appropriately.
 *
 * ctx: libpop context.
 * dev: string for PCI slot num, or NULL means hugepages
 * size: size of allocated memory (must be power of page size)
 */
int pop_ctx_init(pop_ctx_t *ctx, char *dev, size_t size);
int pop_ctx_exit(pop_ctx_t *ctx);

int pop_ctx_verbose(pop_ctx_t *ctx, int level);


/* structure describing pop buffer on p2pmem */
struct pop_buf {
	pop_ctx_t	*pctx;	/* parent context  */

	void		*vaddr;	/* virtual address on mmap region	*/
	uintptr_t	paddr;	/* physical addres of the vaddr		*/
	size_t		size;	/* size of allocated mmap region	*/

	size_t		offset;	/* offset of data	*/
	size_t		length;	/* length of data	*/
};
typedef struct pop_buf pop_buf_t;

/* handling pop_buf like sk_buff */
pop_buf_t *pop_buf_alloc(pop_ctx_t *pctx, size_t size);
void *pop_buf_free(pop_buf_t *pbuf);

void *pop_buf_data(pop_buf_t *pbuf);
void *pop_buf_put(pop_buf_t *pbuf, size_t len);
void *pop_buf_pull(pop_buf_t *pbuf, size_t len);
void *pop_buf_push(pop_buf_t *pbuf, size_t len);
void *pop_buf_trim(pop_buf_t *pbuf, size_t len);




#endif /* LIBPOP_H_ */

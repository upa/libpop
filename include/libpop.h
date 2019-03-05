/*
 * libpop.h: Library for Peripheral-to-Peripheral communication.
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

#define POP_P2PMEM_REG		_IOR('i', 1, struct pop_p2pmem_reg)
#define POP_P2PMEM_UNREG	_IOR('i', 2, struct pop_p2pmem_reg)


#endif /* LIBPOP_H_ */

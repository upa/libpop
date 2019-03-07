/* netmap.h: libpop netmap driver */

#ifndef _LIBPOP_NETMAP_H_
#define _LIBPOP_NETMAP_H_

#include <libpop.h>

#ifdef POP_DRIVER_NETMAP

int pop_driver_netmap_init(pop_driver_t *drv, void *arg);
int pop_driver_netmap_exit(pop_driver_t *drv);

#endif /* POP_DRIVER_NETMAP */

#endif /* _LIBPOP_NETMAP_H_ */



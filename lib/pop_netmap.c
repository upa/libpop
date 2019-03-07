/* netmap.c: libpop netmap driver */

#include <libpop.h>
#include <libpop_util.h>
#define PROGNAME        "libpop-netmap"

#ifdef POP_DRIVER_NETMAP

#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>



/* describing netmap driver, pop_driver_t->driver_data */
struct pop_driver_netmap {
	char devname[IFNAMSIZ];
	struct nm_desc *d;
};

static int pop_driver_netmap_write(pop_driver_t *drv, pop_buf_t *pbufs,
				   int nbufs, int qid)
{
	return 0;
}

static int pop_driver_netmap_read(pop_driver_t *drv, pop_buf_t *pbufs,
				  int nbufs, int qid)
{
	return 0;
}

int pop_driver_netmap_init(pop_driver_t *drv, void *arg)
{
	char *devname = arg;
	struct pop_driver_netmap *nm;

	nm = malloc(sizeof(*nm));
	if (!nm) {
		pr_ve("failed to allocate memory for netmap driver");
		return -1;
	}

	memset(nm, 0, sizeof(*nm));
	strncpy(nm->devname, devname, IFNAMSIZ);
	nm->d = nm_open(devname, NULL, 0, NULL);
	if (!nm->d) {
		pr_ve("failed to nm_open %s", devname);
		return -1;
	}

	drv->type = POP_DRIVER_TYPE_NETMAP;
	drv->pop_driver_write = pop_driver_netmap_write;
	drv->pop_driver_read = pop_driver_netmap_read;
	drv->data = nm;

	pr_vs("open netmap driver for %s", nm->devname);

	return 0;
}

int pop_driver_netmap_exit(pop_driver_t *drv)
{
	struct pop_driver_netmap *nm;

	if (drv->type != POP_DRIVER_TYPE_NETMAP) {
		pr_ve("invalid driver type %d", drv->type);
		return -EINVAL;
	}
		
	nm = drv->data;
	nm_close(nm->d);

	pr_vs("close netmap driver for %s", nm->devname);

	return 0;
}

#endif /* POP_DRIVER_NETMAP */

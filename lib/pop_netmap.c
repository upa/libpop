/* netmap.c: libpop netmap driver */


#define _GNU_SOURCE
#include <sched.h>
#include <poll.h>

#include <libpop.h>
#include <libpop_util.h>
#define PROGNAME        "libpop-netmap"

#ifdef POP_DRIVER_NETMAP

#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>


static int count_online_cpus(void)
{
        cpu_set_t cpu_set;

        if (sched_getaffinity(0, sizeof(cpu_set_t), &cpu_set) == 0)
                return CPU_COUNT(&cpu_set);

        return -1;
}

/* describing netmap driver, pop_driver_t->data */
struct pop_driver_netmap {
	char devname[IFNAMSIZ];
	struct nm_desc **d;	/* array of *nm_desc for all CPUs */

	int ncpus;		/* number of cpus */
};

static int pop_driver_netmap_read(pop_driver_t *drv, pop_buf_t *pbufs,
				  int nbufs, int qid)
{
	int n, ret;
	unsigned int cur, budget;
	struct netmap_ring *ring;
	struct pop_driver_netmap *nm = drv->data;

	/* validation */
	if (qid > nm->ncpus - 1) {
		pr_ve("invalid qid %d, ncpus is %d", qid, nm->ncpus);
		errno = -EINVAL;
		return -1;
	}

	ring = NETMAP_RXRING(nm->d[qid]->nifp, qid);
	if (nm_ring_empty(ring))
		return 0;	/* no new received packet */

	budget = (nm_ring_space(ring) > nbufs) ? nbufs : nm_ring_space(ring);

	for (n = 0; n < budget; n++) {
		cur = ring->cur;
		ring->slot[cur].len = pop_buf_len(&pbufs[n]);
		ring->slot[cur].ptr = pop_buf_paddr(&pbufs[n]);
		ring->slot[cur].flags |= NS_PHY_INDIRECT;	/* XXX */

		cur = nm_ring_next(ring, cur);
		ring->head = ring->cur = cur;
	}

	ret = ioctl(nm->d[qid]->fd, NIOCRXSYNC, NULL);
	if (ret != 0) {
		pr_ve("NIOCRXCYN failed");
		return -1;
	}

	return 0;
}

static int pop_driver_netmap_write(pop_driver_t *drv, pop_buf_t *pbufs,
				   int nbufs, int qid)
{
	int n, ret;
	unsigned int cur, budget;
	struct netmap_ring *ring;
	struct pop_driver_netmap *nm = drv->data;

	/* validation */
	if (qid > nm->ncpus - 1) {
		pr_ve("invalid qid %d, ncpus is %d", qid, nm->ncpus);
		errno = -EINVAL;
		return -1;
	}

	ring = NETMAP_TXRING(nm->d[qid]->nifp, qid);
	if (nm_ring_empty(ring))
		return 0;	/* no txring buffer available */

	budget = (nm_ring_space(ring) > nbufs) ? nbufs : nm_ring_space(ring);

	for (n = 0; n < budget; n++) {
		cur = ring->cur;
		ring->slot[cur].len = pop_buf_len(&pbufs[n]);
		ring->slot[cur].ptr = pop_buf_paddr(&pbufs[n]);
		ring->slot[cur].flags |= NS_PHY_INDIRECT;	/* XXX */

		cur = nm_ring_next(ring, cur);
		ring->head = ring->cur = cur;
	}

	ret = ioctl(nm->d[qid]->fd, NIOCTXSYNC, NULL);
	if (ret != 0) {
		pr_ve("NIOCTXSYNC failed");
		return -1;
	}

	return n;
}

static int pop_driver_netmap_poll(pop_driver_t *drv, int qid)
{
	struct pollfd x[1];
	struct pop_driver_netmap *nm = drv->data;

	/* validation */
	if (qid > nm->ncpus - 1) {
		pr_ve("invalid qid %d, ncpus is %d", qid, nm->ncpus);
		errno = -EINVAL;
		return -1;
	}

	x[0].fd = nm->d[qid]->fd;
	x[0].events = POLLIN;

	return poll(x, 1, -1);
}

int pop_driver_netmap_init(pop_driver_t *drv, void *arg)
{
	int i;
	char *devname = arg;
	struct pop_driver_netmap *nm;

	nm = malloc(sizeof(*nm));
	if (!nm) {
		pr_ve("failed to allocate memory for netmap driver");
		return -1;
	}

	memset(nm, 0, sizeof(*nm));
	strncpy(nm->devname, devname, IFNAMSIZ);

	/* create netmap descriptor for all CPUs */
	nm->ncpus = count_online_cpus();
	nm->d = calloc(nm->ncpus, sizeof(struct nm_desc*));
	if (!nm->d) {
		pr_ve("failed to allocate memory for netmap descriptors");
		return -1;
	}

	for (i = 0; i < nm->ncpus; i++) {

		struct nm_desc nmd;
		struct netmap_ring *ring;

		memset(&nmd, 0, sizeof(struct nm_desc));
		nmd.req.nr_flags |= NR_REG_ONE_NIC;
		nmd.req.nr_ringid = i;
		strncpy(nmd.req.nr_name, devname, IFNAMSIZ);

		nm->d[i] = nm_open(devname, NULL, 0, &nmd);
		if (!nm->d[i]) {
			pr_ve("failed open nm_desc on ringid %d for %s",
			      i, devname);
			return -1;
		}

		/* XXX:
		 * Round up RX ring. It is necessary for NS_PHY_INDRECT.
		 */
		ring = NETMAP_RXRING(nm->d[i]->nifp, i);
		ring->cur = ring->num_slots - 1;
		ring->head = ring->num_slots - 1;
		ioctl(nm->d[i]->fd, NIOCRXSYNC, NULL);

		//pr_vs("%s: ringid=%d fd=%d", devname, i, nm->d[i]->fd);
	}

	drv->type = POP_DRIVER_TYPE_NETMAP;
	drv->pop_driver_write = pop_driver_netmap_write;
	drv->pop_driver_read = pop_driver_netmap_read;
	drv->pop_driver_poll = pop_driver_netmap_poll;
	drv->data = nm;

	pr_vs("open netmap driver for %s", nm->devname);

	return 0;
}

int pop_driver_netmap_exit(pop_driver_t *drv)
{
	int i;
	struct pop_driver_netmap *nm;

	if (drv->type != POP_DRIVER_TYPE_NETMAP) {
		pr_ve("invalid driver type %d", drv->type);
		return -EINVAL;
	}
		
	nm = drv->data;

	/* close all nm_desc */
	for (i = 0; i < nm->ncpus; i++)
		nm_close(nm->d[i]);

	pr_vs("close netmap driver for %s", nm->devname);

	return 0;
}

#endif /* POP_DRIVER_NETMAP */

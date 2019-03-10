/* pop_netmap.c */


#include <stdlib.h>
#include <sys/ioctl.h>

#define PROGNAME "libpop-netmap"

#include <libpop.h>
#include <libpop_util.h>

#include <net/netmap_user.h>

struct pop_nm_rxring *pop_nm_rxring_init(int fd, struct netmap_ring *ring,
					 pop_mem_t *mem)
{
	unsigned int idx, size;
	pop_buf_t *pbuf;
	struct pop_nm_rxring *prxring;
	struct netmap_slot *slot;

	size = ring->num_slots * ring->nr_buf_size;
	pbuf = pop_buf_alloc(mem, size);
	pop_buf_put(pbuf, size);
	if (!pbuf) {
		pr_ve("failed to allocate pbuf for rxring %u", ring->ringid);
		return NULL;
	}

	for (idx = 0; idx < ring->num_slots; idx++) {
		slot = &ring->slot[idx];
		slot->flags |= NS_PHY_INDIRECT;
		slot->ptr = pop_buf_paddr(pbuf) + ring->nr_buf_size * idx;
	}

	/* XXX
	 *
	 * Flush the rx rings by invloking netma_ring_reinit. netmap
	 * native drivers allocate netmap krings and fill descriptor
	 * rings with the packet buffers in the netmap
	 * krings. However, the buffers exist on DRAM. Here makes cur
	 * and head go around the ring. As a result, the physical
	 * addresses in the ptr fields are going to be filled in the
	 * descriptor rings.
	 */
	ring->cur = ring->num_slots - 1;
	ring->head = ring->num_slots -1;
	ioctl(fd, NIOCRXSYNC, NULL);

	prxring = malloc(sizeof(*prxring));
	if (!prxring)
		return NULL;

	prxring->pbuf = pbuf;
	prxring->ring = ring;

	/* flush empty slots */
	while(!nm_ring_empty(ring)) {
		idx = nm_ring_next(ring, ring->head);
		ring->head = ring->cur = idx;
	}
	ioctl(fd, NIOCRXSYNC, NULL);

	return prxring;
}

void pop_nm_rxring_exit(struct pop_nm_rxring *prxring)
{
	unsigned int idx;
	struct netmap_slot *slot;
	struct netmap_ring *ring = pop_nm_ring(prxring);

	for (idx = 0; idx < ring->num_slots; idx++) {
		slot = &ring->slot[idx];
		slot->flags &= ~NS_PHY_INDIRECT;
		slot->ptr = 0;
	}

	pop_buf_free(prxring->pbuf);
	free(prxring);
}

inline void *pop_nm_rxring_buf(struct pop_nm_rxring *prxring, uint32_t idx)
{
	struct netmap_ring *ring = prxring->ring;
	return (pop_buf_data(prxring->pbuf) + (ring->nr_buf_size * idx));
		
}

inline struct netmap_ring *pop_nm_ring(struct pop_nm_rxring *prxring)
{
	return prxring->ring;
}


void pop_nm_set_buf(struct netmap_slot *slot, pop_buf_t *pbuf)
{
	slot->flags |= NS_PHY_INDIRECT;
	slot->ptr = pop_buf_paddr(pbuf);
	slot->len = pop_buf_len(pbuf);
}


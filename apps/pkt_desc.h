/* pkt_desc.h */

#ifndef _PKT_DESC_H_
#define _PKT_DESC_H_


/* struct pkt_desc is placed on the end of the packet buffer on a
 * pop_buf */
struct pkt_desc {
	unsigned int length;
} __attribute__((__packed__));


#define get_pkt_desc(buf, len)						\
	((struct pkt_desc *)(buf + (len - (sizeof(struct pkt_desc)))))

#define get_pktlen_from_desc(buf, len)		\
	(get_pkt_desc(buf, len))->length


#endif /* _PKT_DESC_H_ */

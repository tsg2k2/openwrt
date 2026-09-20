// SPDX-License-Identifier: GPL-2.0+
/*
 * net/dsa/tag_rtl_otto.c - DSA tag driver for Realtek Otto switches
 *
 * External-CPU-over-SerDes variant for the RTL9303 wired to an IPQ8072 via a
 * physical 10GBase-R link (CR1000A).  Unlike the native Otto SoC NIC path
 * (rtl838x_eth.c), which receives the switch's CPU tag out-of-band in the DMA
 * descriptor's cpu_tag[] array, this topology has the RTL9303 serialise the
 * CPU tag INLINE into the frame on the silicon CPU port (port 27).
 *
 * Inline tag is enabled by designating port 27 as the silicon CPU port:
 *   MAC_L2_CPU_PORT_CTRL (0xc70c) bit0 CPU_PORT = 1
 * The tag EtherType comes from MAC_L2_CPU_TAG_ID_CTRL (0xc710), default 0x8899.
 *
 * On-wire format:
 *
 *   +--------+--------+----------- 20-byte tag -----------+------+-----
 *   | MAC DA | MAC SA | 0x8899 | proto | rsn | cpu_tag[8] | Type | payload
 *   +--------+--------+--------+-------+-----+------------+------+-----
 *                       (2)      (1)    (1)    (16 bytes)
 *
 *   proto = 0x04, rsn = reason (0 = forwarded).
 *   cpu_tag[0..7] are 8 big-endian u16, byte-identical to the rtl930x DMA
 *   descriptor format in rtl838x_eth.c (rteth_93xx_{create_tx,decode}_tag):
 *     RX (switch->CPU): source_port = (cpu_tag[0] >> 8) & 0x3f
 *     TX (CPU->switch): cpu_tag[0]=0x8000 marker, cpu_tag[1]=FWD_PHYSICAL|
 *                       IGNORE_STP, cpu_tag[4..7]=64-bit dest port mask.
 *
 * Frames that arrive without the 0x8899 tag (e.g. before the switch is
 * configured, or special untagged traffic) fall through to the lowest user
 * port so they are not silently dropped.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/etherdevice.h>

#include "tag.h"

#define RTL_OTTO_NAME			"rtl_otto"

#define RTL_OTTO_ETHERTYPE		0x8899	/* ETH_P_REALTEK */
#define RTL_OTTO_PROTO			0x04	/* RTL9303 CPU tag protocol */

/* Total inline header inserted before the encapsulated EtherType:
 * 2 (EtherType) + 1 (proto) + 1 (reason) + 16 (cpu_tag[0..7]).
 */
#define RTL_OTTO_TAG_LEN		20
#define RTL_OTTO_CPU_TAG_WORDS		8	/* cpu_tag[0..7] */

/* tag16[] word indices (big-endian on the wire) */
#define RTL_OTTO_W_ETYPE		0	/* 0x8899 */
#define RTL_OTTO_W_PROTO_RSN		1	/* proto<<8 | reason */
#define RTL_OTTO_W_CPUTAG0		2	/* cpu_tag[0] .. cpu_tag[7] */

#define RTL_OTTO_PROTO_MASK		GENMASK(15, 8)
#define RTL_OTTO_REASON_MASK		GENMASK(7, 0)
#define RTL_OTTO_REASON_FORWARD		0

/* cpu_tag[0]: RX source port lives in bits [13:8] */
#define RTL_OTTO_RX_SRC_PORT_MASK	GENMASK(13, 8)

/* cpu_tag[1] TX fields (see rtl838x_eth.h RTL93XX_CPU_TAG1_*) */
#define RTL_OTTO_TX_MARK		0x8000	/* cpu_tag[0] marker bit */
#define RTL_OTTO_TX_FWD_MASK		GENMASK(11, 8)
#define RTL_OTTO_TX_FWD_PHYSICAL	1
#define RTL_OTTO_TX_IGNORE_STP		BIT(2)

/*
 * Tagged TX (CPU->switch forced per-port egress).
 *
 * REQUIRED whenever the silicon CPU port is designated (MAC_L2_CPU_PORT_CTRL
 * @ 0xc70c = 1, see rtl930x_spi.c).  In that mode the switch treats frames
 * ingressing the CPU port (port 27) as CPU-tagged and reads the destination
 * port from the tag's dp_info mask.  With raw/untagged egress the switch
 * cannot resolve a destination for UNICAST frames, so router->client unicast
 * is silently dropped while broadcast/flood (e.g. ARP) still reaches all ports
 * (observed on HW: ARP resolves, unicast ICMP fails).  Tagging egress with the
 * correct dp mask fixes this.  Set to 0 only if c70c is left at 0 (no wire tag).
 */
#define RTL_OTTO_TX_TAGGED		1

#if RTL_OTTO_TX_TAGGED
static struct sk_buff *rtl_otto_xmit(struct sk_buff *skb,
				     struct net_device *dev)
{
	struct dsa_port *dp = dsa_user_to_port(dev);
	u64 portmask = BIT_ULL(dp->index);
	__be16 *tag16;

	skb_push(skb, RTL_OTTO_TAG_LEN);
	dsa_alloc_etype_header(skb, RTL_OTTO_TAG_LEN);
	tag16 = dsa_etype_header_pos_tx(skb);

	tag16[RTL_OTTO_W_ETYPE]     = htons(RTL_OTTO_ETHERTYPE);
	tag16[RTL_OTTO_W_PROTO_RSN] = htons(FIELD_PREP(RTL_OTTO_PROTO_MASK,
						       RTL_OTTO_PROTO));

	/* cpu_tag[0]: marker; cpu_tag[1]: forward to the physical dest port
	 * mask, ignore STP so CPU-injected frames egress unconditionally.
	 */
	tag16[RTL_OTTO_W_CPUTAG0 + 0] = htons(RTL_OTTO_TX_MARK);
	tag16[RTL_OTTO_W_CPUTAG0 + 1] =
		htons(FIELD_PREP(RTL_OTTO_TX_FWD_MASK, RTL_OTTO_TX_FWD_PHYSICAL) |
		      RTL_OTTO_TX_IGNORE_STP);
	tag16[RTL_OTTO_W_CPUTAG0 + 2] = 0;
	tag16[RTL_OTTO_W_CPUTAG0 + 3] = 0;
	/* cpu_tag[4..7]: 64-bit destination port mask, 16 bits per word */
	tag16[RTL_OTTO_W_CPUTAG0 + 4] = htons((portmask >> 48) & 0xffff);
	tag16[RTL_OTTO_W_CPUTAG0 + 5] = htons((portmask >> 32) & 0xffff);
	tag16[RTL_OTTO_W_CPUTAG0 + 6] = htons((portmask >> 16) & 0xffff);
	tag16[RTL_OTTO_W_CPUTAG0 + 7] = htons(portmask & 0xffff);

	return skb;
}
#else
static struct sk_buff *rtl_otto_xmit(struct sk_buff *skb,
				     struct net_device *dev)
{
	/*
	 * Raw egress: the RTL9303 forwards CPU-port ingress by destination MAC
	 * (HW L2 FDB).  No tag is appended.  Verified to work with c70c=1.
	 */
	return skb;
}
#endif

static struct sk_buff *rtl_otto_rcv(struct sk_buff *skb, struct net_device *dev)
{
	struct net_device *user_dev;
	__be16 *tag16;
	u16 cpu_tag0;
	u8 reason;
	int source_port;
	int port;

	/* The EtherType sits at dsa_etype_header_pos_rx() == skb->data - 2,
	 * which is always present (eth_type_trans already pulled the L2 hdr).
	 */
	tag16 = dsa_etype_header_pos_rx(skb);

	if (ntohs(tag16[RTL_OTTO_W_ETYPE]) == RTL_OTTO_ETHERTYPE) {
		if (unlikely(!pskb_may_pull(skb, RTL_OTTO_TAG_LEN)))
			return NULL;

		/* re-read after a potential pskb_may_pull reallocation */
		tag16 = dsa_etype_header_pos_rx(skb);

		cpu_tag0 = ntohs(tag16[RTL_OTTO_W_CPUTAG0]);
		source_port = FIELD_GET(RTL_OTTO_RX_SRC_PORT_MASK, cpu_tag0);
		reason = FIELD_GET(RTL_OTTO_REASON_MASK,
				   ntohs(tag16[RTL_OTTO_W_PROTO_RSN]));

		skb->dev = dsa_conduit_find_user(dev, 0, source_port);
		if (!skb->dev)
			return NULL;

		if (reason == RTL_OTTO_REASON_FORWARD)
			dsa_default_offload_fwd_mark(skb);

		skb_pull_rcsum(skb, RTL_OTTO_TAG_LEN);
		dsa_strip_etype_header(skb, RTL_OTTO_TAG_LEN);

		return skb;
	}

	/*
	 * No CPU tag (switch not yet configured for inline tagging, or special
	 * untagged frame).  Deliver to the first configured user port so the
	 * frame reaches the stack.  Source-port identification is lost.
	 */
	for (port = 0; port < 64; port++) {
		user_dev = dsa_conduit_find_user(dev, 0, port);
		if (user_dev) {
			skb->dev = user_dev;
			return skb;
		}
	}

	return NULL;
}

static const struct dsa_device_ops rtl_otto_netdev_ops = {
	.name		= RTL_OTTO_NAME,
	.proto		= DSA_TAG_PROTO_RTL_OTTO,
	.xmit		= rtl_otto_xmit,
	.rcv		= rtl_otto_rcv,
	.needed_headroom = RTL_OTTO_TX_TAGGED ? RTL_OTTO_TAG_LEN : 0,
};

MODULE_DESCRIPTION("DSA tag driver for Realtek Otto switches (RTL83xx/RTL93xx)");
MODULE_LICENSE("GPL");
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_RTL_OTTO, RTL_OTTO_NAME);

module_dsa_tag_driver(rtl_otto_netdev_ops);

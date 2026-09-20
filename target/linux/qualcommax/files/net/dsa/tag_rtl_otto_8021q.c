// SPDX-License-Identifier: GPL-2.0+
/*
 * net/dsa/tag_rtl_otto_8021q.c - 802.1Q-based DSA tagging for Realtek Otto
 *
 * Alternative to the proprietary inline CPU tag (tag_rtl_otto.c) for the
 * external-CPU-over-SerDes topology (CR1000A: RTL9303 wired to an IPQ8072):
 * the conduit link carries standard 802.1Q frames using the kernel-reserved
 * tag_8021q VIDs, which the IPQ PPE parser understands - enabling RSS
 * hashing, checksum metadata and hardware flow offload for the LAN leg.
 *
 * The switch driver disables the silicon CPU-port designation in this mode
 * (no inline 0x8899 tag) and installs the tag_8021q VLANs: each user port's
 * standalone VID has the port (untagged, force-PVID ingress) and the CPU
 * port (tagged) as members, so CPU-bound frames arrive tagged with their
 * source port's VID and directed TX is confined by VLAN membership.
 */

#include <linux/dsa/8021q.h>

#include "tag.h"
#include "tag_8021q.h"

#define RTL_OTTO_8021Q_NAME "rtl_otto-8021q"

static struct sk_buff *rtl_otto_8021q_xmit(struct sk_buff *skb,
					   struct net_device *netdev)
{
	struct dsa_port *dp = dsa_user_to_port(netdev);
	u16 queue_mapping = skb_get_queue_mapping(skb);
	struct net_device *br = dsa_port_bridge_dev_get(dp);
	u16 tx_vid;
	u8 pcp;

	/* dsa_tag_8021q_bridge_join() deletes a port's standalone VLAN when
	 * it joins a bridge, so directed TX (no offload_fwd_mark, e.g. the
	 * nf_flow fast path transmitting on the user netdev) must use the
	 * bridge VID as well: with the standalone VID the frame traverses a
	 * VLAN with no table entry and reaches the user port with the tag
	 * unstripped.  Delivery via the bridge VID stays precise through
	 * the FDB lookup ("imprecise TX" in sja1105 terms only for unknown
	 * destinations).
	 */
	if (br) {
		if (br_vlan_enabled(br))
			return skb;

		tx_vid = dsa_tag_8021q_bridge_vid(dsa_port_bridge_num_get(dp));
	} else {
		tx_vid = dsa_tag_8021q_standalone_vid(dp);
	}

	pcp = netdev_txq_to_tc(netdev, queue_mapping);

	return dsa_8021q_xmit(skb, netdev, ETH_P_8021Q,
			      ((pcp << VLAN_PRIO_SHIFT) | tx_vid));
}

static struct sk_buff *rtl_otto_8021q_rcv(struct sk_buff *skb,
					  struct net_device *netdev)
{
	int src_port = -1, switch_id = -1, vbid = -1, vid = -1;

	dsa_8021q_rcv(skb, &src_port, &switch_id, &vbid, &vid);

	skb->dev = dsa_tag_8021q_find_user(netdev, src_port, switch_id,
					   vid, vbid);
	if (!skb->dev) {
		net_warn_ratelimited("%s: dropped frame: src_port=%d switch_id=%d vid=%d vbid=%d\n",
				     netdev->name, src_port, switch_id,
				     vid, vbid);
		return NULL;
	}

	dsa_default_offload_fwd_mark(skb);

	return skb;
}

static const struct dsa_device_ops rtl_otto_8021q_netdev_ops = {
	.name			= RTL_OTTO_8021Q_NAME,
	.proto			= DSA_TAG_PROTO_RTL_OTTO_8021Q,
	.xmit			= rtl_otto_8021q_xmit,
	.rcv			= rtl_otto_8021q_rcv,
	.needed_headroom	= VLAN_HLEN,
	.promisc_on_conduit	= true,
};

MODULE_DESCRIPTION("DSA tag driver for Realtek Otto switches using 802.1Q");
MODULE_LICENSE("GPL");
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_RTL_OTTO_8021Q, RTL_OTTO_8021Q_NAME);

module_dsa_tag_driver(rtl_otto_8021q_netdev_ops);

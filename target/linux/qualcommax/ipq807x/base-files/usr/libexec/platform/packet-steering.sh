#!/bin/sh
# Per-board packet steering policy (overrides the generic
# /usr/libexec/network/packet-steering.uc spreader).
#
# Called by /etc/init.d/packet_steering with the value of
# network.globals.packet_steering ("0" disables).

. /lib/functions.sh

case "$(board_name)" in
verizon,cr1000a)
	# All RPS stays off, whatever the global option says. With the
	# 802.1Q switch tagger (boot default, dsa-tag-protocol DTS
	# property) the PPE RSS-hashes LAN ingress and the EDMA Rx rings
	# are pinned one per CPU from the device tree
	# (qcom,rxdesc-ring-cpus). Hardware spreading beats software RPS:
	# stacking RPS on the spread rings measured 1.4-1.6 vs
	# 2.7-3.4 Gbit/s multi-stream NAT on the software path, and the
	# generic spreader is what the LuCI "Packet Steering" checkbox
	# would otherwise apply. The old inline-tag (OTTO) layout needed
	# lan RPS over CPU0+CPU1 instead; if the tagger is ever flipped
	# back, set lan rx queues to mask 3 here.
	for q in /sys/class/net/*/queues/rx-*/rps_cpus; do
		[ -e "$q" ] && echo 0 > "$q"
	done
	;;
*)
	exec /usr/libexec/network/packet-steering.uc "$1"
	;;
esac

exit 0

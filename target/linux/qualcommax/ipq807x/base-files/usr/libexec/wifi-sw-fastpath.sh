#!/bin/sh
# Software fastpath flowtable for WLAN interfaces.
#
# With flow_offloading_hw=1, fw4 builds a single hardware flowtable and
# keeps wlan netdevs out of it (they cannot be hardware-offloaded), so
# wifi traffic would take the full netfilter slowpath. This script
# maintains a second, software-only flowtable holding the wlan devices
# plus the wan uplink. Its forward rule runs just before fw4's, so wifi
# flows are claimed for the kernel software fastpath while wired flows
# fall through to the hardware table: 'flow add' only accepts a
# connection whose resolved ingress devices have hooks in the rule's
# flowtable, which makes the two tables self-classifying.
#
# Runs from a fw4 script include and from a net hotplug hook; safe to
# re-run at any time (the table is re-rendered in one nft transaction,
# which keeps the previous state on failure).

TABLE="wifi_sw_fastpath"

exec 1000>/var/lock/wifi-sw-fastpath.lock
flock 1000

drop_table() {
	nft delete table inet $TABLE 2>/dev/null
	exit 0
}

[ "$(uci -q get firewall.@defaults[0].flow_offloading_hw)" = "1" ] || drop_table

devs=""
for path in /sys/class/net/*; do
	grep -qx 'DEVTYPE=wlan' "$path/uevent" 2>/dev/null || continue
	devs="$devs${devs:+, }\"${path##*/}\""
done

[ -n "$devs" ] || drop_table

. /lib/functions/network.sh
network_get_device wandev wan || wandev=wan

nft -f - <<EOF
table inet $TABLE
delete table inet $TABLE
table inet $TABLE {
	flowtable ft {
		hook ingress priority filter
		devices = { $devs, "$wandev" }
		counter
	}

	chain forward {
		type filter hook forward priority filter - 1; policy accept;
		meta l4proto { tcp, udp } flow add @ft comment "wifi software fastpath"
	}
}
EOF

#!/bin/sh
# Dump RTL9303 registers via /dev/spidev1.0 for before/after-usrApp comparison.
#
# Usage (on the router):
#   ./dump-rtl9303-regs.sh > /tmp/before.txt
#   /lib/rtl/init.sh                       # runs OEM usrApp init for ~30s
#   ./dump-rtl9303-regs.sh > /tmp/after.txt
#   diff /tmp/before.txt /tmp/after.txt
#
# Output format: one register per line, fixed-width so `diff` is clean.
#   0xADDR  NAME                                    = 0xVALUE
#
# Requires: spidev_test (package spidev-test) and /dev/spidev1.0.
# Driver should be loaded with skip_serdes_init=1 if you want to capture the
# pre-init state of SerDes registers.

SPIDEV=${SPIDEV:-/dev/spidev1.0}
SPEED=${SPEED:-12000000}
TMPIN=/tmp/.rtl_spi.in.$$
TMPOUT=/tmp/.rtl_spi.out.$$
trap 'rm -f "$TMPIN" "$TMPOUT"' EXIT INT TERM

if [ ! -e "$SPIDEV" ]; then
    echo "ERROR: $SPIDEV not found — load rtl930x-spi.ko first" >&2
    exit 1
fi
if ! command -v spidev_test >/dev/null 2>&1; then
    echo "ERROR: spidev_test not found — install spidev-test" >&2
    exit 1
fi

# read_reg32 <hex_addr_no_0x>
# Stdout: 8 uppercase hex digits (the 32-bit register value).
read_reg32() {
    local addr ah al o_ah o_al
    addr=$((0x$1))
    ah=$(( (addr >> 8) & 0xff ))
    al=$((  addr       & 0xff ))
    o_ah=$(printf '\\%03o' "$ah")
    o_al=$(printf '\\%03o' "$al")
    # Frame: [0x03][ah][al][dummy][D3][D2][D1][D0]
    printf "\003${o_ah}${o_al}\000\000\000\000\000" > "$TMPIN"
    spidev_test -D "$SPIDEV" -s "$SPEED" -i "$TMPIN" -o "$TMPOUT" >/dev/null 2>&1
    # Extract bytes 4..7 from TMPOUT as 8 uppercase hex digits.
    # Skip `od` (often absent in busybox); use dd+hexdump which busybox has.
    dd if="$TMPOUT" bs=1 skip=4 count=4 2>/dev/null \
        | hexdump -v -e '/1 "%02X"'
}

# write_reg32 <hex_addr_no_0x> <hex_val_no_0x>
write_reg32() {
    local addr val ah al d3 d2 d1 d0
    addr=$((0x$1))
    val=$((0x$2))
    ah=$(( (addr >> 8) & 0xff ))
    al=$((  addr       & 0xff ))
    d3=$(( (val  >> 24) & 0xff ))
    d2=$(( (val  >> 16) & 0xff ))
    d1=$(( (val  >>  8) & 0xff ))
    d0=$((  val         & 0xff ))
    printf "\002$(printf '\\%03o' $ah)$(printf '\\%03o' $al)\000$(printf '\\%03o' $d3)$(printf '\\%03o' $d2)$(printf '\\%03o' $d1)$(printf '\\%03o' $d0)" > "$TMPIN"
    spidev_test -D "$SPIDEV" -s "$SPEED" -i "$TMPIN" >/dev/null 2>&1
}

emit() {
    # emit <hex_addr_no_0x> <NAME>
    local val
    val=$(read_reg32 "$1")
    printf "0x%-4s  %-44s = 0x%s\n" "$1" "$2" "$val"
}

emit_section() { echo; echo "### $* ###"; }

# Indirect table access (RTL9300_TBL_0 at 0xB340 / data 0xB344)
#   cmd = BIT(17) execute | (type << 12) | (idx & 0xfff)
# Poll BIT(17) until clear, then read N data words from 0xB344+i*4.
tbl0_read() {
    # tbl0_read <type_dec> <idx_dec> <ndata_dec>
    local type idx n cmd busy_word tries i addr
    type=$1; idx=$2; n=$3
    cmd=$(( (1 << 17) | ((type & 0xf) << 12) | (idx & 0xfff) ))
    write_reg32 B340 "$(printf '%x' "$cmd")"
    # Poll execute bit
    tries=50
    while [ $tries -gt 0 ]; do
        busy_word=$(read_reg32 B340)
        # check BIT(17)
        if [ $(( (0x$busy_word >> 17) & 1 )) -eq 0 ]; then
            break
        fi
        tries=$((tries - 1))
    done
    i=0
    while [ $i -lt "$n" ]; do
        addr=$(printf '%04X' $((0xB344 + i*4)))
        read_reg32 "$addr"
        i=$((i + 1))
    done
}

emit_tbl0() {
    # emit_tbl0 <type> <idx> <ndata> <NAME>
    local out i word
    out=$(tbl0_read "$1" "$2" "$3")
    i=0
    for word in $out; do
        printf "0xB344+%d  %-39s = 0x%s\n" "$((i*4))" "$4[$i]" "$word"
        i=$((i + 1))
    done
}

# Per-port MIB counter address (RTL930X_STAT_PORT_MIB_CNTR layout):
#   addr_low = 0x0664 + (port + 1) * 0x100 - 4 - offset
# 64-bit values: high32 is at addr_low - 4.
mib_addr_lo() {
    # mib_addr_lo <port> <offset_hex_no_0x>
    local port off end
    port=$1
    off=$((0x$2))
    end=$((0x0664 + (port + 1) * 0x100))
    printf '%04X' $((end - 4 - off))
}

# Read a 32-bit MIB counter (size 1).
emit_mib_l() {
    # emit_mib_l <port> <offset_hex> <name>
    local addr val
    addr=$(mib_addr_lo "$1" "$2")
    val=$(read_reg32 "$addr")
    printf "p%-2d  off 0x%-3s @ 0x%s  %-26s = 0x%s\n" "$1" "$2" "$addr" "$3" "$val"
}

# Read a 64-bit MIB counter (size 2): high at addr-4, low at addr.
emit_mib_q() {
    # emit_mib_q <port> <offset_hex> <name>
    local lo_addr hi_addr lo hi
    lo_addr=$(mib_addr_lo "$1" "$2")
    hi_addr=$(printf '%04X' $((0x$lo_addr - 4)))
    lo=$(read_reg32 "$lo_addr")
    hi=$(read_reg32 "$hi_addr")
    printf "p%-2d  off 0x%-3s @ 0x%s  %-26s = 0x%s%s\n" \
        "$1" "$2" "$lo_addr" "$3" "$hi" "$lo"
}

# SerDes indirect read via 0x03B0 (CTRL) / 0x03B4 (DATA).
#   CTRL = sds<<2 | page<<7 | reg<<13 | BUSY(0)
#   for read: WRITE bit (BIT(1)) is 0.
sds_read() {
    # sds_read <sds> <page> <reg>
    local sds page reg cmd busy data tries
    sds=$1; page=$2; reg=$3
    cmd=$(( ((reg & 0x1f) << 13) | ((page & 0x3f) << 7) | ((sds & 0x1f) << 2) | 1 ))
    write_reg32 03B0 "$(printf '%x' "$cmd")"
    tries=50
    while [ $tries -gt 0 ]; do
        busy=$(read_reg32 03B0)
        if [ $(( 0x$busy & 1 )) -eq 0 ]; then
            break
        fi
        tries=$((tries - 1))
    done
    data=$(read_reg32 03B4)
    printf "%s" "${data}"  # 32-bit but only low 16 are valid
}

emit_sds() {
    # emit_sds <sds> <page_hex> <reg_hex> <NAME>
    local val
    val=$(sds_read "$1" "$((0x$2))" "$((0x$3))")
    # show only low 16 bits
    local low16
    low16=$(printf '%04X' $((0x$val & 0xffff)) )
    printf "SDS%d p0x%s r0x%s  %-32s = 0x%s\n" "$1" "$2" "$3" "$4" "$low16"
}

############################################################
# MAIN DUMP
############################################################

echo "# RTL9303 register dump"
echo "# date:   $(date '+%Y-%m-%dT%H:%M:%S%z')"
echo "# uname:  $(uname -a 2>/dev/null)"
echo "# spidev: $SPIDEV @ ${SPEED} Hz"

# --sweep-all : ranges where chip-level config + CPU-tag config likely live.
# Pass `sweep` as first arg to do the wide sweep instead of named-only dump.
# Slow (~10 minutes), but it's the cleanest way to find unknown registers.
if [ "${1:-}" = "sweep" ]; then
    echo "# MODE: sweep (full sweep of chip-config + MAC/SMI/L2 ranges)"
    # Each line: 0xADDR = 0xVALUE  — sortable, diffable, no labels
    sweep_range() {
        local start=$1 end=$2 label=$3
        echo
        echo "### sweep $label : 0x$(printf '%04X' $start)..0x$(printf '%04X' $end) ###"
        local a=$start
        while [ $a -lt $end ]; do
            local hex
            hex=$(printf '%04X' "$a")
            local val
            val=$(read_reg32 "$hex")
            printf "0x%s = 0x%s\n" "$hex" "$val"
            a=$((a + 4))
        done
    }
    # Likely-relevant chip-level ranges (skip per-port stat blocks 0x0664..0x3260, huge)
    sweep_range $((0x0000)) $((0x0400)) "chip_globals_and_serdes"
    # Per-port MAC ctrl block: 29 ports × 0x40 stride starting at 0x3260
    # (covers MAC_PORT_CTRL, MAC_L2_PORT_CTRL, MAC_L2_PORT_MAX_LEN_CTRL,
    # EEE_CTRL, EEEP_PORT_CTRL, SPG_PORT_IPG_CTRL, ...)
    sweep_range $((0x3200)) $((0x3A00)) "mac_per_port_ctrl_block"
    sweep_range $((0x8000)) $((0x9000)) "vlan_st_ctrl"
    sweep_range $((0x9000)) $((0xA000)) "l2_forwarding_flood"
    sweep_range $((0xA000)) $((0xA400)) "trk_stk_meter_atk"
    sweep_range $((0xC600)) $((0xD000)) "mac_smi_cpu_dma_led"
    echo
    echo "# sweep done"
    exit 0
fi

emit_section Chip identity / debug
emit 0000 CHIP_INFO
emit 0004 MODEL_NAME_INFO
emit 0008 CHIP_DEBUG_INFO
emit 000C CHIP_REV

emit_section MAC SerDes mode select
emit 0194 MAC_SERDES_MODE_SEL_0_3
emit 02A0 MAC_SERDES_MODE_SEL_4_7

emit_section SerDes indirect access registers
emit 03B0 SDS_ACCESS_CTRL
emit 03B4 SDS_ACCESS_DATA

emit_section MAC global
emit CB10 MAC_LINK_STS
emit CB14 MAC_LINK_SPD_STS
emit CB18 MAC_LINK_DUP_STS
emit CB28 MAC_TX_PAUSE_STS
emit CB2C MAC_RX_PAUSE_STS
emit CB34 MAC_EEE_ABLTY
emit CA90 SMI_POLL_CTRL

emit_section "CPU port designation (from usrApp.c regTbl)"
emit C70C MAC_L2_CPU_PORT_CTRL          # bit 0 = CPU_PORT (1=p27 / 0=p28), bit 1 = INTF_P28_SEL
emit CE00 DMA_IF_PKT_CTRL               # internal-DMA path config (informational)

emit_section "MAC force mode (port 0..28)"
for p in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28; do
    a=$(printf '%04X' $((0xCA1C + p*4)))
    emit "$a" "MAC_FORCE_MODE_p${p}"
done

emit_section "MAC port ctrl (0x3260 + port<<6) — selected ports"
for p in 0 8 20 24 25 27 28; do
    a=$(printf '%04X' $((0x3260 + p*64)))
    emit "$a" "MAC_PORT_CTRL_p${p}"
done

emit_section "MAC L2 port ctrl (0x3268 + port<<6) — selected ports"
for p in 0 8 20 24 25 27 28; do
    a=$(printf '%04X' $((0x3268 + p*64)))
    emit "$a" "MAC_L2_PORT_CTRL_p${p}"
done

emit_section L2 forwarding / flood / SABLK / DABLK
emit 905C L2_PORT_SABLK_CTRL
emit 9060 L2_PORT_DABLK_CTRL
emit 9064 L2_UNKN_UC_FLD_PMSK
emit 9068 L2_BC_FLD_PMSK
emit 906C L2_UNKN_MC_FLD_PMSK_maybe
emit 8FD8 L2_CTRL
emit 8FDC L2_AGE_CTRL
emit 8FE0 L2_PORT_AGE_CTRL
emit 9404 L2_TBL_FLUSH_CTRL
emit 9408 L2_TBL_FLUSH_CTRL_2
emit 909C L2_LRN_CONSTRT_CTRL
emit 90A4 L2_LRN_PORT_CONSTRT_CTRL
emit 8FEC L2_PORT_SALRN_0
emit 8FF0 L2_PORT_SALRN_1
emit 8FF4 L2_PORT_NEW_SA_FWD_0
emit 8FF8 L2_PORT_NEW_SA_FWD_1
emit 8FFC L2_PORT_NEW_SA_FWD_2

emit_section STP / VLAN global
emit 8798 ST_CTRL
emit 82D4 VLAN_CTRL
emit 834C VLAN_PORT_FWD
emit 83C0 VLAN_PORT_IGR_FLTR_0
emit 83C4 VLAN_PORT_IGR_FLTR_1
emit 83C8 VLAN_PORT_EGR_FLTR_0
emit 83CC VLAN_PORT_EGR_FLTR_1

emit_section "VLAN per-port PB_VLAN (PVID, 0x82D8 + port*4)"
for p in 0 8 20 24 25 27 28; do
    a=$(printf '%04X' $((0x82D8 + p*4)))
    emit "$a" "PB_VLAN_p${p}"
done

emit_section SMI / MDIO master
emit CA00 SMI_GLB_CTRL
emit CA04 SMI_MAC_TYPE_CTRL
emit CA80 SMI_unknown_0xca80
emit CB70 SMI_ACCESS_PHY_CTRL_0
emit CB74 SMI_ACCESS_PHY_CTRL_1
emit CB78 SMI_ACCESS_PHY_CTRL_2
emit CB7C SMI_ACCESS_PHY_CTRL_3
emit CBB4 SMI_10G_POLL_REG0
emit CBB8 SMI_10G_POLL_REG9
emit CBBC SMI_10G_POLL_REG10

emit_section "Indirect table controllers (snapshot)"
emit B320 TBL_ACCESS_L2_CTRL
emit B324 TBL_ACCESS_L2_METHOD_CTRL
emit B340 TBL_ACCESS_CTRL_0
emit B344 TBL_ACCESS_DATA_0_0
emit B348 TBL_ACCESS_DATA_0_1
emit B34C TBL_ACCESS_DATA_0_2
emit B350 TBL_ACCESS_DATA_0_3
emit B3A0 TBL_ACCESS_CTRL_1
emit B3A4 TBL_ACCESS_DATA_1_0
emit CE04 TBL_ACCESS_CTRL_2
emit CE08 TBL_ACCESS_DATA_2_0

emit_section Interrupts
emit C628 IMR_GLB
emit C62C IMR_PORT_LINK_STS_CHG
emit C658 ISR_GLB
emit C660 ISR_PORT_LINK_STS_CHG

emit_section Statistics control
emit 3240 STAT_RST
emit 3244 STAT_PORT_RST
emit 3248 STAT_CTRL

emit_section "MIB counters — key ports (RX/TX bytes & pkts, discards)"
for p in 8 20 24 25 27 28; do
    echo "# --- port $p ---"
    emit_mib_q "$p" f8 ifInOctets
    emit_mib_q "$p" f0 ifOutOctets
    emit_mib_q "$p" e8 ifInUcastPkts
    emit_mib_q "$p" e0 ifInMcastPkts
    emit_mib_q "$p" d8 ifInBcastPkts
    emit_mib_q "$p" d0 ifOutUcastPkts
    emit_mib_q "$p" c8 ifOutMcastPkts
    emit_mib_q "$p" c0 ifOutBcastPkts
    emit_mib_l "$p" bc ifOutDiscards
    emit_mib_l "$p" 90 dropEvents
done

emit_section LED global
emit CC00 LED_GLB_CTRL

emit_section "VLAN member (TBL_0 type 0)"
emit_tbl0 0 0 4 VLAN0_member
emit_tbl0 0 1 4 VLAN1_member

emit_section "VLAN UNTAG (TBL_0 type 2)"
emit_tbl0 2 0 1 VLAN0_untag
emit_tbl0 2 1 1 VLAN1_untag

emit_section "MSTI / STP state (TBL_0 type 4)"
emit_tbl0 4 0 2 MSTI0_state

emit_section "Traffic forwarding (TBL_0 type 6) — selected ports"
for p in 8 20 24 25 27 28; do
    emit_tbl0 6 "$p" 1 "TrafFwd_src_p${p}"
done

emit_section "SerDes 3 (port 8, AQR113C 10G) — key regs"
emit_sds 3 00 02 sds3_p00_r02
emit_sds 3 1F 09 sds3_p1F_r09_ip_mode
emit_sds 3 1F 14 sds3_p1F_r14_clk_status
emit_sds 3 20 00 sds3_p20_r00_pd
emit_sds 3 20 12 sds3_p20_r12_pll
emit_sds 3 2E 15 sds3_p2E_r15_rx_reset

emit_section "SerDes 5 (port 20, RTL8221B 2.5G) — key regs"
emit_sds 5 00 02 sds5_p00_r02
emit_sds 5 1F 09 sds5_p1F_r09_ip_mode
emit_sds 5 1F 14 sds5_p1F_r14_clk_status
emit_sds 5 20 00 sds5_p20_r00_pd
emit_sds 5 20 12 sds5_p20_r12_pll
emit_sds 5 2E 15 sds5_p2E_r15_rx_reset

emit_section "SerDes 6 (port 24, RTL8221B 2.5G) — key regs"
emit_sds 6 00 02 sds6_p00_r02
emit_sds 6 1F 09 sds6_p1F_r09_ip_mode
emit_sds 6 1F 14 sds6_p1F_r14_clk_status
emit_sds 6 20 00 sds6_p20_r00_pd
emit_sds 6 20 12 sds6_p20_r12_pll
emit_sds 6 2E 15 sds6_p2E_r15_rx_reset

emit_section "SerDes 7 (port 28, CPU 10GBase-R) — key regs"
emit_sds 7 00 02 sds7_p00_r02
emit_sds 7 1F 09 sds7_p1F_r09_ip_mode
emit_sds 7 1F 14 sds7_p1F_r14_clk_status
emit_sds 7 20 00 sds7_p20_r00_pd
emit_sds 7 20 12 sds7_p20_r12_pll
emit_sds 7 2E 15 sds7_p2E_r15_rx_reset

echo
echo "# dump complete"

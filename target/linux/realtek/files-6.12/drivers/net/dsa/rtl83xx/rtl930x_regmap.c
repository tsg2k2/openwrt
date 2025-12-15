// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL930x regmap-based driver
 * 
 * This is a regmap-converted version of the original RTL930x driver,
 * replacing direct register access with regmap API calls for better
 * abstraction and debugging capabilities.
 */

#include <linux/regmap.h>
#include <asm/mach-rtl838x/mach-rtl83xx.h>
#include <linux/etherdevice.h>
#include <linux/inetdevice.h>

#include "rtl83xx.h"

#define RTL930X_VLAN_PORT_TAG_STS_INTERNAL			0x0
#define RTL930X_VLAN_PORT_TAG_STS_UNTAG				0x1
#define RTL930X_VLAN_PORT_TAG_STS_TAGGED			0x2
#define RTL930X_VLAN_PORT_TAG_STS_PRIORITY_TAGGED		0x3

#define RTL930X_VLAN_PORT_TAG_STS_CTRL_BASE			0xCE24
/* port 0-28 */
#define RTL930X_VLAN_PORT_TAG_STS_CTRL(port) \
	(RTL930X_VLAN_PORT_TAG_STS_CTRL_BASE + (port << 2))
#define RTL930X_VLAN_PORT_TAG_STS_CTRL_EGR_OTAG_STS_MASK	GENMASK(7, 6)
#define RTL930X_VLAN_PORT_TAG_STS_CTRL_EGR_ITAG_STS_MASK	GENMASK(5, 4)
#define RTL930X_VLAN_PORT_TAG_STS_CTRL_EGR_P_OTAG_KEEP_MASK	GENMASK(3, 3)
#define RTL930X_VLAN_PORT_TAG_STS_CTRL_EGR_P_ITAG_KEEP_MASK	GENMASK(2, 2)
#define RTL930X_VLAN_PORT_TAG_STS_CTRL_IGR_P_OTAG_KEEP_MASK	GENMASK(1, 1)
#define RTL930X_VLAN_PORT_TAG_STS_CTRL_IGR_P_ITAG_KEEP_MASK	GENMASK(0, 0)

#define RTL930X_LED_GLB_ACTIVE_LOW				BIT(22)

#define RTL930X_LED_SETX_0_CTRL(x) (RTL930X_LED_SET0_0_CTRL - (x * 8))
#define RTL930X_LED_SETX_1_CTRL(x) (RTL930X_LED_SETX_0_CTRL(x) - 4)

/* get register for given set and led in the set */
#define RTL930X_LED_SETX_LEDY(x, y) (RTL930X_LED_SETX_0_CTRL(x) - 4 * (y / 2))

/* get shift for given led in any set */
#define RTL930X_LED_SET_LEDX_SHIFT(x) (16 * (x % 2))

/* Global regmap instance for RTL930x */
static struct regmap *rtl930x_regmap;
static bool rtl930x_use_spi = false;

/* Regmap configuration for RTL930x MMIO */
static const struct regmap_config rtl930x_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0xFFFF,
	.cache_type = REGCACHE_NONE, /* No caching for hardware registers */
	.use_single_read = true,
	.use_single_write = true,
};

/* Regmap read/write wrapper functions */
static inline int rtl930x_regmap_read(unsigned int reg, unsigned int *val)
{
	if (!rtl930x_regmap)
		return -ENODEV;
	return regmap_read(rtl930x_regmap, reg, val);
}

static inline int rtl930x_regmap_write(unsigned int reg, unsigned int val)
{
	if (!rtl930x_regmap)
		return -ENODEV;
	return regmap_write(rtl930x_regmap, reg, val);
}

static inline int rtl930x_regmap_update_bits(unsigned int reg, unsigned int mask, unsigned int val)
{
	if (!rtl930x_regmap)
		return -ENODEV;
	return regmap_update_bits(rtl930x_regmap, reg, mask, val);
}

/* Regmap-based register access functions */
static inline u32 rtl930x_r32(u32 reg)
{
	unsigned int val;
	int ret = rtl930x_regmap_read(reg, &val);
	if (ret) {
		pr_err("RTL930x regmap read failed for reg 0x%x: %d\n", reg, ret);
		return 0;
	}
	return val;
}

static inline void rtl930x_w32(u32 val, u32 reg)
{
	int ret = rtl930x_regmap_write(reg, val);
	if (ret)
		pr_err("RTL930x regmap write failed for reg 0x%x: %d\n", reg, ret);
}

static inline void rtl930x_w32_mask(u32 clear, u32 set, u32 reg)
{
	int ret = rtl930x_regmap_update_bits(reg, clear, set);
	if (ret)
		pr_err("RTL930x regmap update_bits failed for reg 0x%x: %d\n", reg, ret);
}

/* Definition of the RTL930X-specific template field IDs as used in the PIE */
enum template_field_id {
	TEMPLATE_FIELD_SPM0 = 0,		/* Source portmask ports 0-15 */
	TEMPLATE_FIELD_SPM1 = 1,		/* Source portmask ports 16-31 */
	TEMPLATE_FIELD_DMAC0 = 2,		/* Destination MAC [15:0] */
	TEMPLATE_FIELD_DMAC1 = 3,		/* Destination MAC [31:16] */
	TEMPLATE_FIELD_DMAC2 = 4,		/* Destination MAC [47:32] */
	TEMPLATE_FIELD_SMAC0 = 5,		/* Source MAC [15:0] */
	TEMPLATE_FIELD_SMAC1 = 6,		/* Source MAC [31:16] */
	TEMPLATE_FIELD_SMAC2 = 7,		/* Source MAC [47:32] */
	TEMPLATE_FIELD_ETHERTYPE = 8,		/* Ethernet frame type field */
	TEMPLATE_FIELD_OTAG = 9,
	TEMPLATE_FIELD_ITAG = 10,
	TEMPLATE_FIELD_SIP0 = 11,
	TEMPLATE_FIELD_SIP1 = 12,
	TEMPLATE_FIELD_DIP0 = 13,
	TEMPLATE_FIELD_DIP1 = 14,
	TEMPLATE_FIELD_IP_TOS_PROTO = 15,
	TEMPLATE_FIELD_L4_SPORT = 16,
	TEMPLATE_FIELD_L4_DPORT = 17,
	TEMPLATE_FIELD_L34_HEADER = 18,
	TEMPLATE_FIELD_TCP_INFO = 19,
	TEMPLATE_FIELD_FIELD_SELECTOR_VALID = 20,
	TEMPLATE_FIELD_FIELD_SELECTOR_0 = 21,
	TEMPLATE_FIELD_FIELD_SELECTOR_1 = 22,
	TEMPLATE_FIELD_FIELD_SELECTOR_2 = 23,
	TEMPLATE_FIELD_FIELD_SELECTOR_3 = 24,
	TEMPLATE_FIELD_FIELD_SELECTOR_4 = 25,
	TEMPLATE_FIELD_FIELD_SELECTOR_5 = 26,
	TEMPLATE_FIELD_SIP2 = 27,
	TEMPLATE_FIELD_SIP3 = 28,
	TEMPLATE_FIELD_SIP4 = 29,
	TEMPLATE_FIELD_SIP5 = 30,
	TEMPLATE_FIELD_SIP6 = 31,
	TEMPLATE_FIELD_SIP7 = 32,
	TEMPLATE_FIELD_DIP2 = 33,
	TEMPLATE_FIELD_DIP3 = 34,
	TEMPLATE_FIELD_DIP4 = 35,
	TEMPLATE_FIELD_DIP5 = 36,
	TEMPLATE_FIELD_DIP6 = 37,
	TEMPLATE_FIELD_DIP7 = 38,
	TEMPLATE_FIELD_PKT_INFO = 39,
	TEMPLATE_FIELD_FLOW_LABEL = 40,
	TEMPLATE_FIELD_DSAP_SSAP = 41,
	TEMPLATE_FIELD_SNAP_OUI = 42,
	TEMPLATE_FIELD_FWD_VID = 43,
	TEMPLATE_FIELD_RANGE_CHK = 44,
	TEMPLATE_FIELD_VLAN_GMSK = 45,		/* VLAN Group Mask/IP range check */
	TEMPLATE_FIELD_DLP = 46,
	TEMPLATE_FIELD_META_DATA = 47,
	TEMPLATE_FIELD_SRC_FWD_VID = 48,
	TEMPLATE_FIELD_SLP = 49,
};

/* The meaning of TEMPLATE_FIELD_VLAN depends on phase and the configuration in
 * RTL930X_PIE_CTRL. We use always the same definition and map to the inner VLAN tag:
 */
#define TEMPLATE_FIELD_VLAN TEMPLATE_FIELD_ITAG

/* Number of fixed templates predefined in the RTL9300 SoC */
#define N_FIXED_TEMPLATES 5
/* RTL9300 specific predefined templates */
static enum template_field_id fixed_templates[N_FIXED_TEMPLATES][N_FIXED_FIELDS] = {
	{
	  TEMPLATE_FIELD_DMAC0, TEMPLATE_FIELD_DMAC1, TEMPLATE_FIELD_DMAC2,
	  TEMPLATE_FIELD_SMAC0, TEMPLATE_FIELD_SMAC1, TEMPLATE_FIELD_SMAC2,
	  TEMPLATE_FIELD_VLAN, TEMPLATE_FIELD_IP_TOS_PROTO, TEMPLATE_FIELD_DSAP_SSAP,
	  TEMPLATE_FIELD_ETHERTYPE, TEMPLATE_FIELD_SPM0, TEMPLATE_FIELD_SPM1
	}, {
	  TEMPLATE_FIELD_SIP0, TEMPLATE_FIELD_SIP1, TEMPLATE_FIELD_DIP0,
	  TEMPLATE_FIELD_DIP1, TEMPLATE_FIELD_IP_TOS_PROTO, TEMPLATE_FIELD_TCP_INFO,
	  TEMPLATE_FIELD_L4_SPORT, TEMPLATE_FIELD_L4_DPORT, TEMPLATE_FIELD_VLAN,
	  TEMPLATE_FIELD_RANGE_CHK, TEMPLATE_FIELD_SPM0, TEMPLATE_FIELD_SPM1
	}, {
	  TEMPLATE_FIELD_DMAC0, TEMPLATE_FIELD_DMAC1, TEMPLATE_FIELD_DMAC2,
	  TEMPLATE_FIELD_VLAN, TEMPLATE_FIELD_ETHERTYPE, TEMPLATE_FIELD_IP_TOS_PROTO,
	  TEMPLATE_FIELD_SIP0, TEMPLATE_FIELD_SIP1, TEMPLATE_FIELD_DIP0,
	  TEMPLATE_FIELD_DIP1, TEMPLATE_FIELD_L4_SPORT, TEMPLATE_FIELD_L4_DPORT
	}, {
	  TEMPLATE_FIELD_DIP0, TEMPLATE_FIELD_DIP1, TEMPLATE_FIELD_DIP2,
	  TEMPLATE_FIELD_DIP3, TEMPLATE_FIELD_DIP4, TEMPLATE_FIELD_DIP5,
	  TEMPLATE_FIELD_DIP6, TEMPLATE_FIELD_DIP7, TEMPLATE_FIELD_IP_TOS_PROTO,
	  TEMPLATE_FIELD_TCP_INFO, TEMPLATE_FIELD_L4_SPORT, TEMPLATE_FIELD_L4_DPORT
	}, {
	  TEMPLATE_FIELD_SIP0, TEMPLATE_FIELD_SIP1, TEMPLATE_FIELD_SIP2,
	  TEMPLATE_FIELD_SIP3, TEMPLATE_FIELD_SIP4, TEMPLATE_FIELD_SIP5,
	  TEMPLATE_FIELD_SIP6, TEMPLATE_FIELD_SIP7, TEMPLATE_FIELD_VLAN,
	  TEMPLATE_FIELD_RANGE_CHK, TEMPLATE_FIELD_SPM1, TEMPLATE_FIELD_SPM1
	},
};

void rtl930x_print_matrix(void)
{
	struct table_reg *r = rtl_table_get(RTL9300_TBL_0, 6);

	for (int i = 0; i < 29; i++) {
		rtl_table_read(r, i);
		pr_debug("> %08x\n", rtl930x_r32(rtl_table_data(r, 0)));
	}
	rtl_table_release(r);
}

inline void rtl930x_exec_tbl0_cmd(u32 cmd)
{
	rtl930x_w32(cmd, RTL930X_TBL_ACCESS_CTRL_0);
	do { } while (rtl930x_r32(RTL930X_TBL_ACCESS_CTRL_0) & (1 << 17));
}

inline void rtl930x_exec_tbl1_cmd(u32 cmd)
{
	rtl930x_w32(cmd, RTL930X_TBL_ACCESS_CTRL_1);
	do { } while (rtl930x_r32(RTL930X_TBL_ACCESS_CTRL_1) & (1 << 17));
}

inline int rtl930x_tbl_access_data_0(int i)
{
	return RTL930X_TBL_ACCESS_DATA_0(i);
}

static inline int rtl930x_l2_port_new_salrn(int p)
{
	return RTL930X_L2_PORT_SALRN(p);
}

static inline int rtl930x_l2_port_new_sa_fwd(int p)
{
	/* TODO: The definition of the fields changed, because of the master-cpu in a stack */
	return RTL930X_L2_PORT_NEW_SA_FWD(p);
}

/* Initialize regmap for RTL930x MMIO */
int rtl930x_regmap_init(struct device *dev, void __iomem *base)
{
	struct regmap_mmio_context *ctx;
	
	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
		
	ctx->regs = base;
	ctx->val_bytes = 4;
	ctx->reg_bytes = 4;
	ctx->pad_bytes = 0;
	
	rtl930x_regmap = devm_regmap_init_mmio(dev, base, &rtl930x_regmap_config);
	if (IS_ERR(rtl930x_regmap)) {
		dev_err(dev, "Failed to initialize RTL930x MMIO regmap: %ld\n", PTR_ERR(rtl930x_regmap));
		return PTR_ERR(rtl930x_regmap);
	}
	
	rtl930x_use_spi = false;
	dev_info(dev, "RTL930x MMIO regmap initialized successfully\n");
	return 0;
}

/* Initialize regmap for RTL930x SPI */
int rtl930x_regmap_init_spi(struct device *dev, struct regmap *spi_regmap)
{
	if (!spi_regmap) {
		dev_err(dev, "Invalid SPI regmap\n");
		return -EINVAL;
	}
	
	rtl930x_regmap = spi_regmap;
	rtl930x_use_spi = true;
	
	dev_info(dev, "RTL930x SPI regmap initialized successfully\n");
	return 0;
}

/* Get current regmap instance */
struct regmap *rtl930x_get_regmap(void)
{
	return rtl930x_regmap;
}

/* Check if using SPI interface */
bool rtl930x_is_spi_mode(void)
{
	return rtl930x_use_spi;
}

/* Cleanup regmap */
void rtl930x_regmap_exit(void)
{
	rtl930x_regmap = NULL;
	rtl930x_use_spi = false;
}
static int rtldsa_930x_get_mirror_config(struct rtldsa_mirror_config *config,
					 int group, int port)
{
	config->ctrl = RTL930X_MIR_CTRL + group * 4;
	config->spm = RTL930X_MIR_SPM_CTRL + group * 4;
	config->dpm = RTL930X_MIR_DPM_CTRL + group * 4;

	/* Enable mirroring to destination port */
	config->val = BIT(0);
	config->val |= port << 9;

	/* mirror mode: let mirrored packets follow TX settings of
	 * mirroring port
	 */
	config->val |= BIT(5);

	/* direction of traffic to be mirrored when a packet
	 * hits both SPM and DPM ports: prefer egress
	 */
	config->val |= BIT(4);

	return 0;
}

static int rtldsa_930x_port_rate_police_add(struct dsa_switch *ds, int port,
					    const struct flow_action_entry *act,
					    bool ingress)
{
	u32 burst;
	u64 rate;
	u32 addr;

	/* rate has unit 16000 bit */
	rate = div_u64(act->police.rate_bytes_ps, 2000);
	rate = min_t(u64, rate, RTL93XX_BANDWIDTH_CTRL_RATE_MAX);
	rate |= RTL93XX_BANDWIDTH_CTRL_ENABLE;

	if (ingress)
		addr = RTL930X_BANDWIDTH_CTRL_INGRESS(port);
	else
		addr = RTL930X_BANDWIDTH_CTRL_EGRESS(port);

	if (ingress) {
		burst = min_t(u32, act->police.burst, RTL930X_BANDWIDTH_CTRL_INGRESS_BURST_MAX);

		/* the linux kernel only provides a single burst value. But the
		 * realtek HW needs two. And to get flow control correctly
		 * working, the realtek default ratio of 1:2 seems to work
		 * reasonable well
		 */
		rtl930x_w32(burst, RTL930X_BANDWIDTH_CTRL_INGRESS_BURST_HIGH_ON(port));
		rtl930x_w32(burst / 2, RTL930X_BANDWIDTH_CTRL_INGRESS_BURST_HIGH_OFF(port));

		/* Enable ingress bandwidth flow control to improve TCP throughput and avoid
		 * the drops behavior of the RTL930x ingress rate limiter which seem to not
		 * play well with any congestion control algorithm
		 */
		rtl930x_w32_mask(0, RTL930X_INGRESS_FC_CTRL_EN(port),
			    RTL930X_INGRESS_FC_CTRL(port));
	} else {
		burst = min_t(u32, act->police.burst, RTL930X_BANDWIDTH_CTRL_MAX_BURST);

		rtl930x_w32(burst, addr + 4);
	}

	rtl930x_w32(rate, addr);

	return 0;
}

static int rtldsa_930x_port_rate_police_del(struct dsa_switch *ds, int port,
					    struct flow_cls_offload *cls,
					    bool ingress)
{
	u32 addr;

	if (ingress)
		addr = RTL930X_BANDWIDTH_CTRL_INGRESS(port);
	else
		addr = RTL930X_BANDWIDTH_CTRL_EGRESS(port);

	rtl930x_w32_mask(RTL93XX_BANDWIDTH_CTRL_ENABLE, 0, addr);

	if (ingress)
		rtl930x_w32_mask(RTL930X_INGRESS_FC_CTRL_EN(port), 0,
			    RTL930X_INGRESS_FC_CTRL(port));

	return 0;
}

static inline int rtl930x_trk_mbr_ctr(int group)
{
	return RTL930X_TRK_MBR_CTRL + (group << 2);
}

static void rtl930x_vlan_tables_read(u32 vlan, struct rtl838x_vlan_info *info)
{
	u32 v, w;
	/* Read VLAN table (1) via register 0 */
	struct table_reg *r = rtl_table_get(RTL9300_TBL_0, 1);

	rtl_table_read(r, vlan);
	v = rtl930x_r32(rtl_table_data(r, 0));
	w = rtl930x_r32(rtl_table_data(r, 1));
	pr_debug("VLAN_READ %d: %08x %08x\n", vlan, v, w);
	rtl_table_release(r);

	info->member_ports = v >> 3;
	info->profile_id = (w >> 24) & 7;
	info->hash_mc_fid = !!(w & BIT(27));
	info->hash_uc_fid = !!(w & BIT(28));
	info->fid = ((v & 0x7) << 3) | ((w >> 29) & 0x7);

	/* Read UNTAG table via table register 2 */
	r = rtl_table_get(RTL9300_TBL_2, 0);
	rtl_table_read(r, vlan);
	v = rtl930x_r32(rtl_table_data(r, 0));
	rtl_table_release(r);

	info->untagged_ports = v >> 3;
}

static void rtl930x_vlan_set_tagged(u32 vlan, struct rtl838x_vlan_info *info)
{
	u32 v, w;
	/* Access VLAN table (1) via register 0 */
	struct table_reg *r = rtl_table_get(RTL9300_TBL_0, 1);

	v = info->member_ports << 3;
	v |= ((u32)info->fid) >> 3;

	w = ((u32)info->fid) << 29;
	w |= info->hash_mc_fid ? BIT(27) : 0;
	w |= info->hash_uc_fid ? BIT(28) : 0;
	w |= info->profile_id << 24;

	rtl930x_w32(v, rtl_table_data(r, 0));
	rtl930x_w32(w, rtl_table_data(r, 1));

	rtl_table_write(r, vlan);
	rtl_table_release(r);
}

void rtl930x_vlan_profile_dump(int profile)
{
	u32 p[5];

	if (profile < 0 || profile > 7)
		return;

	p[0] = rtl930x_r32(RTL930X_VLAN_PROFILE_SET(profile));
	p[1] = rtl930x_r32(RTL930X_VLAN_PROFILE_SET(profile) + 4);
	p[2] = rtl930x_r32(RTL930X_VLAN_PROFILE_SET(profile) + 8) & 0x1FFFFFFF;
	p[3] = rtl930x_r32(RTL930X_VLAN_PROFILE_SET(profile) + 12) & 0x1FFFFFFF;
	p[4] = rtl930x_r32(RTL930X_VLAN_PROFILE_SET(profile) + 16) & 0x1FFFFFFF;

	pr_debug("VLAN %d: L2 learn: %d; Unknown MC PMasks: L2 %0x, IPv4 %0x, IPv6: %0x",
		 profile, p[0] & (3 << 21), p[2], p[3], p[4]);
	pr_debug("  Routing enabled: IPv4 UC %c, IPv6 UC %c, IPv4 MC %c, IPv6 MC %c\n",
		 p[0] & BIT(17) ? 'y' : 'n', p[0] & BIT(16) ? 'y' : 'n',
		 p[0] & BIT(13) ? 'y' : 'n', p[0] & BIT(12) ? 'y' : 'n');
	pr_debug("  Bridge enabled: IPv4 MC %c, IPv6 MC %c,\n",
		 p[0] & BIT(15) ? 'y' : 'n', p[0] & BIT(14) ? 'y' : 'n');
	pr_debug("VLAN profile %d: raw %08x %08x %08x %08x %08x\n",
		 profile, p[0], p[1], p[2], p[3], p[4]);
}

static void rtl930x_vlan_set_untagged(u32 vlan, u64 portmask)
{
	struct table_reg *r = rtl_table_get(RTL9300_TBL_2, 0);

	rtl930x_w32(portmask << 3, rtl_table_data(r, 0));
	rtl_table_write(r, vlan);
	rtl_table_release(r);
}

/* Sets the L2 forwarding to be based on either the inner VLAN tag or the outer */
static void rtl930x_vlan_fwd_on_inner(int port, bool is_set)
{
	/* Always set all tag modes to fwd based on either inner or outer tag */
	if (is_set)
		rtl930x_w32_mask(0xf, 0, RTL930X_VLAN_PORT_FWD + (port << 2));
	else
		rtl930x_w32_mask(0, 0xf, RTL930X_VLAN_PORT_FWD + (port << 2));
}

static void rtl930x_vlan_profile_setup(int profile)
{
	u32 p[5];

	pr_debug("In %s\n", __func__);
	p[0] = rtl930x_r32(RTL930X_VLAN_PROFILE_SET(profile));
	p[1] = rtl930x_r32(RTL930X_VLAN_PROFILE_SET(profile) + 4);

	/* Enable routing of Ipv4/6 Unicast and IPv4/6 Multicast traffic */
	p[0] |= BIT(17) | BIT(16) | BIT(13) | BIT(12);
	p[2] = 0x1fffffff; /* L2 unknown MC flooding portmask all ports, including the CPU-port */
	p[3] = 0x1fffffff; /* IPv4 unknown MC flooding portmask */
	p[4] = 0x1fffffff; /* IPv6 unknown MC flooding portmask */

	rtl930x_w32(p[0], RTL930X_VLAN_PROFILE_SET(profile));
	rtl930x_w32(p[1], RTL930X_VLAN_PROFILE_SET(profile) + 4);
	rtl930x_w32(p[2], RTL930X_VLAN_PROFILE_SET(profile) + 8);
	rtl930x_w32(p[3], RTL930X_VLAN_PROFILE_SET(profile) + 12);
	rtl930x_w32(p[4], RTL930X_VLAN_PROFILE_SET(profile) + 16);
}

static void rtl930x_l2_learning_setup(void)
{
	/* Portmask for flooding broadcast traffic */
	rtl930x_w32(0x1fffffff, RTL930X_L2_BC_FLD_PMSK);

	/* Portmask for flooding unicast traffic with unknown destination */
	rtl930x_w32(0x1fffffff, RTL930X_L2_UNKN_UC_FLD_PMSK);

	/* Limit learning to maximum: 32k entries, after that just flood (bits 0-1) */
	rtl930x_w32((0x7fff << 2) | 0, RTL930X_L2_LRN_CONSTRT_CTRL);
}

static void rtldsa_930x_enable_learning(int port, bool enable)
{
	/* Limit learning to maximum: 32k entries */
	rtl930x_w32_mask(GENMASK(17, 3), enable ? (0x7ffe << 3) : 0,
		    RTL930X_L2_LRN_PORT_CONSTRT_CTRL + port * 4);
}

static void rtldsa_930x_enable_flood(int port, bool enable)
{
	/* 0: forward
	 * 1: drop
	 * 2: trap to local CPU
	 * 3: copy to local CPU
	 * 4: trap to master CPU
	 * 5: copy to master CPU
	 */
	rtl930x_w32_mask(GENMASK(2, 0), enable ? 0 : 1,
		    RTL930X_L2_LRN_PORT_CONSTRT_CTRL + port * 4);
}

static void rtl930x_stp_get(struct rtl838x_switch_priv *priv, u16 msti, u32 port_state[])
{
	u32 cmd = 1 << 17 | /* Execute cmd */
		  0 << 16 | /* Read */
		  4 << 12 | /* Table type 0b10 */
		  (msti & 0xfff);
	priv->r->exec_tbl0_cmd(cmd);

	for (int i = 0; i < 2; i++)
		port_state[i] = rtl930x_r32(RTL930X_TBL_ACCESS_DATA_0(i));
	pr_debug("MSTI: %d STATE: %08x, %08x\n", msti, port_state[0], port_state[1]);
}

static void rtl930x_stp_set(struct rtl838x_switch_priv *priv, u16 msti, u32 port_state[])
{
	u32 cmd = 1 << 17 | /* Execute cmd */
		  1 << 16 | /* Write */
		  4 << 12 | /* Table type 4 */
		  (msti & 0xfff);

	for (int i = 0; i < 2; i++)
		rtl930x_w32(port_state[i], RTL930X_TBL_ACCESS_DATA_0(i));
	priv->r->exec_tbl0_cmd(cmd);
}

static inline int rtl930x_mac_force_mode_ctrl(int p)
{
	return RTL930X_MAC_FORCE_MODE_CTRL + (p << 2);
}

static inline int rtl930x_mac_port_ctrl(int p)
{
	return RTL930X_MAC_L2_PORT_CTRL(p);
}

static u64 rtl930x_l2_hash_seed(u64 mac, u32 vid)
{
	u64 v = vid;

	v <<= 48;
	v |= mac;

	return v;
}

/* Calculate both the block 0 and the block 1 hash by applyingthe same hash
 * algorithm as the one used currently by the ASIC to the seed, and return
 * both hashes in the lower and higher word of the return value since only 12 bit of
 * the hash are significant
 */
static u32 rtl930x_l2_hash_key(struct rtl838x_switch_priv *priv, u64 seed)
{
	u32 k0, k1, h1, h2, h;

	k0 = (u32)(((seed >> 55) & 0x1f) ^
		   ((seed >> 44) & 0x7ff) ^
		   ((seed >> 33) & 0x7ff) ^
		   ((seed >> 22) & 0x7ff) ^
		   ((seed >> 11) & 0x7ff) ^
		   (seed & 0x7ff));

	h1 = (seed >> 11) & 0x7ff;
	h1 = ((h1 & 0x1f) << 6) | ((h1 >> 5) & 0x3f);

	h2 = (seed >> 33) & 0x7ff;
	h2 = ((h2 & 0x3f) << 5) | ((h2 >> 6) & 0x3f);

	k1 = (u32)(((seed << 55) & 0x1f) ^
		   ((seed >> 44) & 0x7ff) ^
		   h2 ^
		   ((seed >> 22) & 0x7ff) ^
		   h1 ^
		   (seed & 0x7ff));

	/* Algorithm choice for block 0 */
	if (rtl930x_r32(RTL930X_L2_CTRL) & BIT(0))
		h = k1;
	else
		h = k0;

	/* Algorithm choice for block 1
	 * Since k0 and k1 are < 2048, adding 2048 will offset the hash into the second
	 * half of hash-space
	 * 2048 is in fact the hash-table size 16384 divided by 4 hashes per bucket
	 * divided by 2 to divide the hash space in 2
	 */
	if (rtl930x_r32(RTL930X_L2_CTRL) & BIT(1))
		h |= (k1 + 2048) << 16;
	else
		h |= (k0 + 2048) << 16;

	return h;
}
/* Fills an L2 entry structure from the SoC registers */
static void rtl930x_fill_l2_entry(u32 r[], struct rtl838x_l2_entry *e)
{
	pr_debug("In %s valid?\n", __func__);
	e->valid = !!(r[2] & BIT(31));
	if (!e->valid)
		return;

	pr_debug("In %s is valid\n", __func__);
	e->is_ip_mc = false;
	e->is_ipv6_mc = false;

	/* TODO: Is there not a function to copy directly MAC memory? */
	e->mac[0] = (r[0] >> 24);
	e->mac[1] = (r[0] >> 16);
	e->mac[2] = (r[0] >> 8);
	e->mac[3] = r[0];
	e->mac[4] = (r[1] >> 24);
	e->mac[5] = (r[1] >> 16);

	e->next_hop = !!(r[2] & BIT(12));
	e->rvid = r[1] & 0xfff;

	/* Is it a unicast entry? check multicast bit */
	if (!(e->mac[0] & 1)) {
		e->type = L2_UNICAST;
		e->is_static = !!(r[2] & BIT(14));
		e->port = (r[2] >> 20) & 0x3ff;
		/* Check for trunk port */
		if (r[2] & BIT(30)) {
			e->is_trunk = true;
			e->stack_dev = (e->port >> 9) & 1;
			e->trunk = e->port & 0x3f;
		} else {
			e->is_trunk = false;
			e->stack_dev = (e->port >> 6) & 0xf;
			e->port = e->port & 0x3f;
		}

		e->block_da = !!(r[2] & BIT(15));
		e->block_sa = !!(r[2] & BIT(16));
		e->suspended = !!(r[2] & BIT(13));
		e->age = (r[2] >> 17) & 3;
		e->valid = true;
		/* the UC_VID field in hardware is used for the VID or for the route id */
		if (e->next_hop) {
			e->nh_route_id = r[2] & 0x7ff;
			e->vid = 0;
		} else {
			e->vid = r[2] & 0xfff;
			e->nh_route_id = 0;
		}
	} else {
		e->valid = true;
		e->type = L2_MULTICAST;
		e->mc_portmask_index = (r[2] >> 16) & 0x3ff;
	}
}

/* Fills the 3 SoC table registers r[] with the information of in the rtl838x_l2_entry */
static void rtl930x_fill_l2_row(u32 r[], struct rtl838x_l2_entry *e)
{
	u32 port;

	if (!e->valid) {
		r[0] = r[1] = r[2] = 0;
		return;
	}

	r[2] = BIT(31);	/* Set valid bit */

	r[0] = ((u32)e->mac[0]) << 24 |
	       ((u32)e->mac[1]) << 16 |
	       ((u32)e->mac[2]) << 8 |
	       ((u32)e->mac[3]);
	r[1] = ((u32)e->mac[4]) << 24 |
	       ((u32)e->mac[5]) << 16;

	r[2] |= e->next_hop ? BIT(12) : 0;

	if (e->type == L2_UNICAST) {
		r[2] |= e->is_static ? BIT(14) : 0;
		r[1] |= e->rvid & 0xfff;
		r[2] |= (e->port & 0x3ff) << 20;
		if (e->is_trunk) {
			r[2] |= BIT(30);
			port = e->stack_dev << 9 | (e->port & 0x3f);
		} else {
			port = (e->stack_dev & 0xf) << 6;
			port |= e->port & 0x3f;
		}
		r[2] |= port << 20;
		r[2] |= e->block_da ? BIT(15) : 0;
		r[2] |= e->block_sa ? BIT(17) : 0;
		r[2] |= e->suspended ? BIT(13) : 0;
		r[2] |= (e->age & 0x3) << 17;
		/* the UC_VID field in hardware is used for the VID or for the route id */
		if (e->next_hop)
			r[2] |= e->nh_route_id & 0x7ff;
		else
			r[2] |= e->vid & 0xfff;
	} else { /* L2_MULTICAST */
		r[2] |= (e->mc_portmask_index & 0x3ff) << 16;
		r[2] |= e->mc_mac_index & 0x7ff;
	}
}

/* Read an L2 UC or MC entry out of a hash bucket of the L2 forwarding table
 * hash is the id of the bucket and pos is the position of the entry in that bucket
 * The data read from the SoC is filled into rtl838x_l2_entry
 */
static u64 rtl930x_read_l2_entry_using_hash(u32 hash, u32 pos, struct rtl838x_l2_entry *e)
{
	u32 r[3];
	struct table_reg *q = rtl_table_get(RTL9300_TBL_L2, 0);
	u32 idx;
	u64 mac;
	u64 seed;

	pr_debug("%s: hash %08x, pos: %d\n", __func__, hash, pos);

	/* On the RTL93xx, 2 different hash algorithms are used making it a
	 * total of 8 buckets that need to be searched, 4 for each hash-half
	 * Use second hash space when bucket is between 4 and 8
	 */
	if (pos >= 4) {
		pos -= 4;
		hash >>= 16;
	} else {
		hash &= 0xffff;
	}

	idx = (0 << 14) | (hash << 2) | pos; /* Search SRAM, with hash and at pos in bucket */
	pr_debug("%s: NOW hash %08x, pos: %d\n", __func__, hash, pos);

	rtl_table_read(q, idx);
	for (int i = 0; i < 3; i++)
		r[i] = rtl930x_r32(rtl_table_data(q, i));

	rtl_table_release(q);

	rtl930x_fill_l2_entry(r, e);

	pr_debug("%s: valid: %d, nh: %d\n", __func__, e->valid, e->next_hop);
	if (!e->valid)
		return 0;

	mac = ((u64)e->mac[0]) << 40 |
	      ((u64)e->mac[1]) << 32 |
	      ((u64)e->mac[2]) << 24 |
	      ((u64)e->mac[3]) << 16 |
	      ((u64)e->mac[4]) << 8 |
	      ((u64)e->mac[5]);

	seed = rtl930x_l2_hash_seed(mac, e->rvid);
	pr_debug("%s: mac %016llx, seed %016llx\n", __func__, mac, seed);

	/* return vid with concatenated mac as unique id */
	return seed;
}

static void rtl930x_write_l2_entry_using_hash(u32 hash, u32 pos, struct rtl838x_l2_entry *e)
{
	u32 r[3];
	struct table_reg *q = rtl_table_get(RTL9300_TBL_L2, 0);
	u32 idx = (0 << 14) | (hash << 2) | pos; /* Access SRAM, with hash and at pos in bucket */

	pr_debug("%s: hash %d, pos %d\n", __func__, hash, pos);
	pr_debug("%s: index %d -> mac %02x:%02x:%02x:%02x:%02x:%02x\n", __func__, idx,
		 e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);

	rtl930x_fill_l2_row(r, e);

	for (int i = 0; i < 3; i++)
		rtl930x_w32(r[i], rtl_table_data(q, i));

	rtl_table_write(q, idx);
	rtl_table_release(q);
}

static u64 rtl930x_read_cam(int idx, struct rtl838x_l2_entry *e)
{
	u32 r[3];
	struct table_reg *q = rtl_table_get(RTL9300_TBL_L2, 1);

	rtl_table_read(q, idx);
	for (int i = 0; i < 3; i++)
		r[i] = rtl930x_r32(rtl_table_data(q, i));

	rtl_table_release(q);

	rtl930x_fill_l2_entry(r, e);
	if (!e->valid)
		return 0;

	/* return mac with concatenated vid as unique id */
	return ((u64)r[0] << 28) | ((r[1] & 0xffff0000) >> 4) | e->vid;
}

static void rtl930x_write_cam(int idx, struct rtl838x_l2_entry *e)
{
	u32 r[3];
	struct table_reg *q = rtl_table_get(RTL9300_TBL_L2, 1); /* Access L2 Table 1 */

	rtl930x_fill_l2_row(r, e);

	for (int i = 0; i < 3; i++)
		rtl930x_w32(r[i], rtl_table_data(q, i));

	rtl_table_write(q, idx);
	rtl_table_release(q);
}

static u64 rtl930x_read_mcast_pmask(int idx)
{
	u32 portmask;
	/* Read MC_PORTMASK (2) via register RTL9300_TBL_L2 */
	struct table_reg *q = rtl_table_get(RTL9300_TBL_L2, 2);

	rtl_table_read(q, idx);
	portmask = rtl930x_r32(rtl_table_data(q, 0));
	portmask >>= 3;
	rtl_table_release(q);

	pr_debug("%s: Index idx %d has portmask %08x\n", __func__, idx, portmask);

	return portmask;
}

static void rtl930x_write_mcast_pmask(int idx, u64 portmask)
{
	u32 pm = portmask;

	/* Access MC_PORTMASK (2) via register RTL9300_TBL_L2 */
	struct table_reg *q = rtl_table_get(RTL9300_TBL_L2, 2);

	pr_debug("%s: Index idx %d has portmask %08x\n", __func__, idx, pm);
	pm <<= 3;
	rtl930x_w32(pm, rtl_table_data(q, 0));
	rtl_table_write(q, idx);
	rtl_table_release(q);
}

static void rtldsa_930x_set_receive_management_action(int port, rma_ctrl_t type,
						      action_type_t action)
{
	u32 shift;
	u32 value;
	u32 reg;

	/* hack for value mapping */
	if (type == GRATARP && action == COPY2CPU)
		action = TRAP2MASTERCPU;

	/* PTP doesn't allow to flood to all ports */
	if (action == FLOODALL &&
	    (type == PTP || type == PTP_UDP || type == PTP_ETH2)) {
		pr_warn("%s: Port flooding not supported for PTP\n", __func__);
		return;
	}

	switch (action) {
	case FORWARD:
		value = 0;
		break;
	case DROP:
		value = 1;
		break;
	case TRAP2CPU:
		value = 2;
		break;
	case TRAP2MASTERCPU:
		value = 3;
		break;
	case FLOODALL:
		value = 4;
		break;
	default:
		return;
	}

	switch (type) {
	case BPDU:
		reg = RTL930X_RMA_BPDU_CTRL + (port / 10) * 4;
		shift = (port % 10) * 3;
		rtl930x_w32_mask(GENMASK(shift + 2, shift), value << shift, reg);
		break;
	case PTP:
		reg = RTL930X_RMA_PTP_CTRL + port * 4;

		/* udp */
		rtl930x_w32_mask(GENMASK(3, 2), value << 2, reg);

		/* eth2 */
		rtl930x_w32_mask(GENMASK(1, 0), value, reg);
		break;
	case PTP_UDP:
		reg = RTL930X_RMA_PTP_CTRL + port * 4;
		rtl930x_w32_mask(GENMASK(3, 2), value << 2, reg);
		break;
	case PTP_ETH2:
		reg = RTL930X_RMA_PTP_CTRL + port * 4;
		rtl930x_w32_mask(GENMASK(1, 0), value, reg);
		break;
	case LLDP:
		reg = RTL930X_RMA_LLDP_CTRL + (port / 10) * 4;
		shift = (port % 10) * 3;
		rtl930x_w32_mask(GENMASK(shift + 2, shift), value << shift, reg);
		break;
	case EAPOL:
		reg = RTL930X_RMA_EAPOL_CTRL + (port / 10) * 4;
		shift = (port % 10) * 3;
		rtl930x_w32_mask(GENMASK(shift + 2, shift), value << shift, reg);
		break;
	case GRATARP:
		reg = RTL930X_SPCL_TRAP_PORT_CTRL + (port / 16) * 4;
		shift = (port % 16) * 2;
		rtl930x_w32_mask(GENMASK(shift + 1, shift), value << shift, reg);
		break;
	}
}

/* Enable traffic between a source port and a destination port matrix */
static void rtl930x_traffic_set(int source, u64 dest_matrix)
{
	struct table_reg *r = rtl_table_get(RTL9300_TBL_0, 6);

	rtl930x_w32((dest_matrix << 3), rtl_table_data(r, 0));
	rtl_table_write(r, source);
	rtl_table_release(r);
}

static void rtl930x_traffic_enable(int source, int dest)
{
	struct table_reg *r = rtl_table_get(RTL9300_TBL_0, 6);

	rtl_table_read(r, source);
	rtl930x_w32_mask(0, BIT(dest + 3), rtl_table_data(r, 0));
	rtl_table_write(r, source);
	rtl_table_release(r);
}

static void rtl930x_traffic_disable(int source, int dest)
{
	struct table_reg *r = rtl_table_get(RTL9300_TBL_0, 6);

	rtl_table_read(r, source);
	rtl930x_w32_mask(BIT(dest + 3), 0, rtl_table_data(r, 0));
	rtl_table_write(r, source);
	rtl_table_release(r);
}

void rtl9300_dump_debug(void)
{
	u16 r = RTL930X_STAT_PRVTE_DROP_COUNTER0;

	for (int i = 0; i < 10; i++) {
		pr_debug("# %d %08x %08x %08x %08x %08x %08x %08x %08x\n", i * 8,
			 rtl930x_r32(r), rtl930x_r32(r + 4), rtl930x_r32(r + 8), rtl930x_r32(r + 12),
			 rtl930x_r32(r + 16), rtl930x_r32(r + 20), rtl930x_r32(r + 24), rtl930x_r32(r + 28));
		r += 32;
	}
	pr_debug("# %08x %08x %08x %08x %08x\n",
		 rtl930x_r32(r), rtl930x_r32(r + 4), rtl930x_r32(r + 8), rtl930x_r32(r + 12), rtl930x_r32(r + 16));
	rtl930x_print_matrix();
	pr_debug("RTL930X_L2_PORT_SABLK_CTRL: %08x, RTL930X_L2_PORT_DABLK_CTRL %08x\n",
		 rtl930x_r32(RTL930X_L2_PORT_SABLK_CTRL), rtl930x_r32(RTL930X_L2_PORT_DABLK_CTRL)

	);
}

irqreturn_t rtldsa_930x_switch_irq(int irq, void *dev_id)
{
	struct dsa_switch *ds = dev_id;
	struct rtl838x_switch_priv *priv = ds->priv;
	unsigned long ports = rtl930x_r32(RTL930X_ISR_PORT_LINK_STS_CHG);
	unsigned int i;
	u32 link;

	/* Clear status */
	rtl930x_w32(ports, RTL930X_ISR_PORT_LINK_STS_CHG);

	/* Read the register twice because of issues with latency at least
	 * with the external RTL8226 PHY on the XGS1210
	 */
	link = rtl930x_r32(RTL930X_MAC_LINK_STS);
	link = rtl930x_r32(RTL930X_MAC_LINK_STS);

	for_each_set_bit(i, &ports, priv->cpu_port)
		dsa_port_phylink_mac_change(ds, i, link & BIT(i));

	return IRQ_HANDLED;
}
/* Calculate both the block 0 and the block 1 hash, and return in
 * lower and higher word of the return value since only 12 bit of
 * the hash are significant
 */
u32 rtl930x_hash(struct rtl838x_switch_priv *priv, u64 seed)
{
	u32 k0, k1, h1, h2, h;

	k0 = (u32) (((seed >> 55) & 0x1f) ^
		    ((seed >> 44) & 0x7ff) ^
		    ((seed >> 33) & 0x7ff) ^
		    ((seed >> 22) & 0x7ff) ^
		    ((seed >> 11) & 0x7ff) ^
		    (seed & 0x7ff));

	h1 = (seed >> 11) & 0x7ff;
	h1 = ((h1 & 0x1f) << 6) | ((h1 >> 5) & 0x3f);

	h2 = (seed >> 33) & 0x7ff;
	h2 = ((h2 & 0x3f) << 5) | ((h2 >> 6) & 0x3f);

	k1 = (u32) (((seed << 55) & 0x1f) ^
		    ((seed >> 44) & 0x7ff) ^
		    h2 ^
		    ((seed >> 22) & 0x7ff) ^
		    h1 ^
		    (seed & 0x7ff));

	/* Algorithm choice for block 0 */
	if (rtl930x_r32(RTL930X_L2_CTRL) & BIT(0))
		h = k1;
	else
		h = k0;

	/* Algorithm choice for block 1
	 * Since k0 and k1 are < 2048, adding 2048 will offset the hash into the second
	 * half of hash-space
	 * 2048 is in fact the hash-table size 16384 divided by 4 hashes per bucket
	 * divided by 2 to divide the hash space in 2
	 */
	if (rtl930x_r32(RTL930X_L2_CTRL) & BIT(1))
		h |= (k1 + 2048) << 16;
	else
		h |= (k0 + 2048) << 16;

	return h;
}

/* Enables or disables the EEE/EEEP capability of a port */
static void rtldsa_930x_set_mac_eee(struct rtl838x_switch_priv *priv, int port, bool enable)
{
	u32 v;

	/* This works only for Ethernet ports, and on the RTL930X, ports from 26 are SFP */
	if (port >= 26)
		return;

	pr_debug("In %s: setting port %d to %d\n", __func__, port, enable);
	v = enable ? 0x3f : 0x0;

	/* Set EEE/EEEP state for 100, 500, 1000MBit and 2.5, 5 and 10GBit */
	rtl930x_w32_mask(0, v << 10, rtl930x_mac_force_mode_ctrl(port));

	/* Set TX/RX EEE state */
	v = enable ? 0x3 : 0x0;
	rtl930x_w32(v, RTL930X_EEE_CTRL(port));

	priv->ports[port].eee_enabled = enable;
}

static void rtl930x_init_eee(struct rtl838x_switch_priv *priv, bool enable)
{
	pr_debug("Setting up EEE, state: %d\n", enable);

	/* Setup EEE on all ports */
	for (int i = 0; i < priv->cpu_port; i++) {
		if (priv->ports[i].phy)
			priv->r->set_mac_eee(priv, i, enable);
	}

	priv->eee_enabled = enable;
}

static u32 rtl930x_packet_cntr_read(int counter)
{
	u32 v;

	/* Read LOG table (3) via register RTL9300_TBL_0 */
	struct table_reg *r = rtl_table_get(RTL9300_TBL_0, 3);

	pr_debug("In %s, id %d\n", __func__, counter);
	rtl_table_read(r, counter / 2);

	pr_debug("Registers: %08x %08x\n",
		 rtl930x_r32(rtl_table_data(r, 0)), rtl930x_r32(rtl_table_data(r, 1)));
	/* The table has a size of 2 registers */
	if (counter % 2)
		v = rtl930x_r32(rtl_table_data(r, 0));
	else
		v = rtl930x_r32(rtl_table_data(r, 1));

	rtl_table_release(r);

	return v;
}

static void rtl930x_packet_cntr_clear(int counter)
{
	/* Access LOG table (3) via register RTL9300_TBL_0 */
	struct table_reg *r = rtl_table_get(RTL9300_TBL_0, 3);

	pr_debug("In %s, id %d\n", __func__, counter);
	/* The table has a size of 2 registers */
	if (counter % 2)
		rtl930x_w32(0, rtl_table_data(r, 0));
	else
		rtl930x_w32(0, rtl_table_data(r, 1));

	rtl_table_write(r, counter / 2);

	rtl_table_release(r);
}

static void rtl930x_vlan_port_keep_tag_set(int port, bool keep_outer, bool keep_inner)
{
	rtl930x_w32(FIELD_PREP(RTL930X_VLAN_PORT_TAG_STS_CTRL_EGR_OTAG_STS_MASK,
			  keep_outer ? RTL930X_VLAN_PORT_TAG_STS_TAGGED : RTL930X_VLAN_PORT_TAG_STS_UNTAG) |
	       FIELD_PREP(RTL930X_VLAN_PORT_TAG_STS_CTRL_EGR_ITAG_STS_MASK,
			  keep_inner ? RTL930X_VLAN_PORT_TAG_STS_TAGGED : RTL930X_VLAN_PORT_TAG_STS_UNTAG),
	       RTL930X_VLAN_PORT_TAG_STS_CTRL(port));
}

static void rtl930x_vlan_port_pvidmode_set(int port, enum pbvlan_type type, enum pbvlan_mode mode)
{
	if (type == PBVLAN_TYPE_INNER)
		rtl930x_w32_mask(0x3, mode, RTL930X_VLAN_PORT_PB_VLAN + (port << 2));
	else
		rtl930x_w32_mask(0x3 << 14, mode << 14, RTL930X_VLAN_PORT_PB_VLAN + (port << 2));
}

static void rtl930x_vlan_port_pvid_set(int port, enum pbvlan_type type, int pvid)
{
	if (type == PBVLAN_TYPE_INNER)
		rtl930x_w32_mask(0xfff << 2, pvid << 2, RTL930X_VLAN_PORT_PB_VLAN + (port << 2));
	else
		rtl930x_w32_mask(0xfff << 16, pvid << 16, RTL930X_VLAN_PORT_PB_VLAN + (port << 2));
}

static int rtldsa_930x_vlan_port_fast_age(struct rtl838x_switch_priv *priv, int port, u16 vid)
{
	u32 val;

	rtl930x_w32(port << 11, RTL930X_L2_TBL_FLUSH_CTRL + 4);

	val = 0;
	val |= vid << 12;
	val |= BIT(26); /* compare port id */
	val |= BIT(28); /* compare VID */
	val |= BIT(30); /* status - trigger flush */
	rtl930x_w32(val, RTL930X_L2_TBL_FLUSH_CTRL);

	do { } while (rtl930x_r32(priv->r->l2_tbl_flush_ctrl) & BIT(30));

	return 0;
}

static int rtl930x_set_ageing_time(unsigned long msec)
{
	int t = rtl930x_r32(RTL930X_L2_AGE_CTRL);

	t &= 0x1FFFFF;
	t = (t * 7) / 10;
	pr_debug("L2 AGING time: %d sec\n", t);

	t = (msec / 100 + 6) / 7;
	t = t > 0x1FFFFF ? 0x1FFFFF : t;
	rtl930x_w32_mask(0x1FFFFF, t, RTL930X_L2_AGE_CTRL);
	pr_debug("Dynamic aging for ports: %x\n", rtl930x_r32(RTL930X_L2_PORT_AGE_CTRL));

	return 0;
}

static void rtl930x_set_igr_filter(int port,  enum igr_filter state)
{
	rtl930x_w32_mask(0x3 << ((port & 0xf) << 1), state << ((port & 0xf) << 1),
		    RTL930X_VLAN_PORT_IGR_FLTR + (((port >> 4) << 2)));
}

static void rtl930x_set_egr_filter(int port,  enum egr_filter state)
{
	rtl930x_w32_mask(0x1 << (port % 0x1D), state << (port % 0x1D),
		    RTL930X_VLAN_PORT_EGR_FLTR + (((port / 29) << 2)));
}

static void rtl930x_set_distribution_algorithm(int group, int algoidx, u32 algomsk)
{
	u32 l3shift = 0;
	u32 newmask = 0;

	/* TODO: for now we set algoidx to 0 */
	algoidx = 0;
	if (algomsk & TRUNK_DISTRIBUTION_ALGO_SIP_BIT) {
		l3shift = 4;
		newmask |= TRUNK_DISTRIBUTION_ALGO_L3_SIP_BIT;
	}
	if (algomsk & TRUNK_DISTRIBUTION_ALGO_DIP_BIT) {
		l3shift = 4;
		newmask |= TRUNK_DISTRIBUTION_ALGO_L3_DIP_BIT;
	}
	if (algomsk & TRUNK_DISTRIBUTION_ALGO_SRC_L4PORT_BIT) {
		l3shift = 4;
		newmask |= TRUNK_DISTRIBUTION_ALGO_L3_SRC_L4PORT_BIT;
	}
	if (algomsk & TRUNK_DISTRIBUTION_ALGO_SRC_L4PORT_BIT) {
		l3shift = 4;
		newmask |= TRUNK_DISTRIBUTION_ALGO_L3_SRC_L4PORT_BIT;
	}

	if (l3shift == 4) {
		if (algomsk & TRUNK_DISTRIBUTION_ALGO_SMAC_BIT)
			newmask |= TRUNK_DISTRIBUTION_ALGO_L3_SMAC_BIT;

		if (algomsk & TRUNK_DISTRIBUTION_ALGO_DMAC_BIT)
			newmask |= TRUNK_DISTRIBUTION_ALGO_L3_DMAC_BIT;
	} else  {
		if (algomsk & TRUNK_DISTRIBUTION_ALGO_SMAC_BIT)
			newmask |= TRUNK_DISTRIBUTION_ALGO_L2_SMAC_BIT;
		if (algomsk & TRUNK_DISTRIBUTION_ALGO_DMAC_BIT)
			newmask |= TRUNK_DISTRIBUTION_ALGO_L2_DMAC_BIT;
	}

	rtl930x_w32(newmask << l3shift, RTL930X_TRK_HASH_CTRL + (algoidx << 2));
}
static void rtldsa_930x_led_get_forced(const struct device_node *node,
				       const u8 leds_in_set[4],
				       u8 forced_leds_per_port[RTL930X_CPU_PORT])
{
	DECLARE_BITMAP(mask, RTL930X_CPU_PORT);
	unsigned int port;
	char set_str[36];
	u32 pm;

	for (u8 set = 0; set < 4; set++) {
		snprintf(set_str, sizeof(set_str), "realtek,led-set%d-force-port-mask", set);
		if (of_property_read_u32(node, set_str, &pm))
			continue;

		bitmap_from_arr32(mask, &pm, RTL930X_CPU_PORT);

		for_each_set_bit(port, mask, RTL930X_CPU_PORT)
			forced_leds_per_port[port] = leds_in_set[set];
	}
}

static void rtl930x_led_init(struct rtl838x_switch_priv *priv)
{
	u8 forced_leds_per_port[RTL930X_CPU_PORT] = {};
	struct device_node *node;
	struct device *dev = priv->dev;
	u8 leds_in_set[4] = {};
	u32 pm = 0;

	node = of_find_compatible_node(NULL, NULL, "realtek,rtl9300-leds");
	if (!node) {
		dev_dbg(dev, "No compatible LED node found\n");
		return;
	}

	for (int set = 0; set < 4; set++) {
		char set_name[16] = {0};
		u32 set_config[4];
		int leds_in_this_set = 0;

		/* Reset LED set configuration */
		rtl930x_w32(0, RTL930X_LED_SETX_0_CTRL(set));
		rtl930x_w32(0, RTL930X_LED_SETX_1_CTRL(set));

		/* Each LED set has (up to) 4 LEDs, and each LED is configured
		 * with 16 bits. So each 32 bit register holds configuration for
		 * 2 LEDs. Therefore, each set requires 2 registers for
		 * configuring all 4 LEDs.
		 */
		snprintf(set_name, sizeof(set_name), "led_set%d", set);
		leds_in_this_set = of_property_count_u32_elems(node, set_name);

		if (leds_in_this_set <= 0 || leds_in_this_set > ARRAY_SIZE(set_config)) {
			if (leds_in_this_set != -EINVAL) {
				dev_err(dev, "%s invalid, skipping this set, leds_in_this_set=%d, should be (0, %d]\n",
					set_name, leds_in_this_set, ARRAY_SIZE(set_config));
			}

			continue;
		}

		dev_info(dev, "%s has %d LEDs configured\n", set_name, leds_in_this_set);
		leds_in_set[set] = leds_in_this_set;

		if (of_property_read_u32_array(node, set_name, set_config, leds_in_this_set))
			break;

		/* Write configuration for selected LEDs */
		for (int i = 0, led = leds_in_this_set - 1; led >= 0; led--, i++) {
			rtl930x_w32_mask(0xffff << RTL930X_LED_SET_LEDX_SHIFT(led),
				    (0xffff & set_config[i]) << RTL930X_LED_SET_LEDX_SHIFT(led),
				    RTL930X_LED_SETX_LEDY(set, led));
		}
	}

	rtldsa_930x_led_get_forced(node, leds_in_set, forced_leds_per_port);

	for (int i = 0; i < priv->cpu_port; i++) {
		int pos = (i << 1) % 32;
		u32 set;

		rtl930x_w32_mask(0x3 << pos, 0, RTL930X_LED_PORT_FIB_SET_SEL_CTRL(i));
		rtl930x_w32_mask(0x3 << pos, 0, RTL930X_LED_PORT_COPR_SET_SEL_CTRL(i));

		if (!priv->ports[i].phy && !priv->pcs[i] && !(forced_leds_per_port[i]))
			continue;

		if (forced_leds_per_port[i] > 0)
			priv->ports[i].leds_on_this_port = forced_leds_per_port[i];

		/* 0x0 = 1 led, 0x1 = 2 leds, 0x2 = 3 leds, 0x3 = 4 leds per port */
		rtl930x_w32_mask(0x3 << pos, (priv->ports[i].leds_on_this_port - 1) << pos, RTL930X_LED_PORT_NUM_CTRL(i));

		pm |= BIT(i);

		set = priv->ports[i].led_set;
		rtl930x_w32_mask(0, set << pos, RTL930X_LED_PORT_COPR_SET_SEL_CTRL(i));
		rtl930x_w32_mask(0, set << pos, RTL930X_LED_PORT_FIB_SET_SEL_CTRL(i));
	}

	/* Set LED mode to serial (0x1) */
	rtl930x_w32_mask(0x3, 0x1, RTL930X_LED_GLB_CTRL);

	/* Set LED active state */
	if (of_property_read_bool(node, "active-low"))
		rtl930x_w32_mask(RTL930X_LED_GLB_ACTIVE_LOW, 0, RTL930X_LED_GLB_CTRL);
	else
		rtl930x_w32_mask(0, RTL930X_LED_GLB_ACTIVE_LOW, RTL930X_LED_GLB_CTRL);

	/* Set port type masks */
	rtl930x_w32(pm, RTL930X_LED_PORT_COPR_MASK_CTRL);
	rtl930x_w32(pm, RTL930X_LED_PORT_FIB_MASK_CTRL);
	rtl930x_w32(pm, RTL930X_LED_PORT_COMBO_MASK_CTRL);

	for (int i = 0; i < 24; i++)
		dev_dbg(dev, "%08x: %08x\n", 0xbb00cc00 + i * 4, rtl930x_r32(0xcc00 + i * 4));
}

static void rtldsa_930x_qos_set_group_selector(int port, int group)
{
	rtl930x_w32_mask(RTL93XX_PORT_TBL_IDX_CTRL_IDX_MASK(port),
		    group << RTL93XX_PORT_TBL_IDX_CTRL_IDX_OFFSET(port),
		    RTL930X_PORT_TBL_IDX_CTRL(port));
}

static void rtldsa_930x_qos_setup_default_dscp2queue_map(void)
{
	u32 queue;

	/* The default mapping between dscp and queue is based on
	 * the first 3 bits indicate the precedence (prio = dscp >> 3).
	 */
	for (int i = 0; i < DSCP_MAP_MAX; i++) {
		queue = (i >> 3) << RTL93XX_REMAP_DSCP_INTPRI_DSCP_OFFSET(i);
		rtl930x_w32_mask(RTL93XX_REMAP_DSCP_INTPRI_DSCP_MASK(i),
			    queue, RTL930X_REMAP_DSCP(i));
	}
}

static void rtldsa_930x_qos_prio2queue_matrix(int *min_queues)
{
	u32 v = 0;

	for (int i = 0; i < MAX_PRIOS; i++)
		v |= i << (min_queues[i] * 3);

	rtl930x_w32(v, RTL930X_QM_INTPRI2QID_CTRL);
}

static void rtldsa_930x_qos_set_scheduling_queue_weights(struct rtl838x_switch_priv *priv)
{
	struct dsa_port *dp;
	u32 addr;

	dsa_switch_for_each_user_port(dp, priv->ds) {
		for (int q = 0; q < 8; q++) {
			if (dp->index < 24)
				addr = RTL930X_SCHED_PORT_Q_CTRL_SET0(dp->index, q);
			else
				addr = RTL930X_SCHED_PORT_Q_CTRL_SET1(dp->index, q);

			rtl930x_w32(rtldsa_default_queue_weights[q], addr);
		}
	}
}

static void rtldsa_930x_qos_init(struct rtl838x_switch_priv *priv)
{
	struct dsa_port *dp;
	u32 v;

	/* Assign all the ports to the Group-0 */
	dsa_switch_for_each_user_port(dp, priv->ds)
		rtldsa_930x_qos_set_group_selector(dp->index, 0);

	rtldsa_930x_qos_prio2queue_matrix(rtldsa_max_available_queue);

	/* configure priority weights */
	v = 0;
	v |= FIELD_PREP(RTL93XX_PRI_SEL_TBL_CTRL_PORT_MASK, 3);
	v |= FIELD_PREP(RTL93XX_PRI_SEL_TBL_CTRL_DSCP_MASK, 5);
	v |= FIELD_PREP(RTL93XX_PRI_SEL_TBL_CTRL_ITAG_MASK, 6);
	v |= FIELD_PREP(RTL93XX_PRI_SEL_TBL_CTRL_OTAG_MASK, 7);

	rtl930x_w32(v, RTL930X_PRI_SEL_TBL_CTRL(0));

	rtldsa_930x_qos_setup_default_dscp2queue_map();
	rtldsa_930x_qos_set_scheduling_queue_weights(priv);
}

/* RTL930x register structure with regmap-based functions */
const struct rtl838x_reg rtl930x_regmap_reg = {
	.mask_port_reg_be = rtl838x_mask_port_reg,
	.set_port_reg_be = rtl838x_set_port_reg,
	.get_port_reg_be = rtl838x_get_port_reg,
	.mask_port_reg_le = rtl838x_mask_port_reg,
	.set_port_reg_le = rtl838x_set_port_reg,
	.get_port_reg_le = rtl838x_get_port_reg,
	.stat_port_rst = RTL930X_STAT_PORT_RST,
	.stat_rst = RTL930X_STAT_RST,
	.stat_port_std_mib = RTL930X_STAT_PORT_MIB_CNTR,
	.stat_port_prv_mib = RTL930X_STAT_PORT_PRVTE_CNTR,
	.stat_counters_lock = rtldsa_counters_lock_register,
	.stat_counters_unlock = rtldsa_counters_unlock_register,
	.stat_update_counters_atomically = rtldsa_update_counters_atomically,
	.stat_counter_poll_interval = RTLDSA_COUNTERS_POLL_INTERVAL,
	.traffic_enable = rtl930x_traffic_enable,
	.traffic_disable = rtl930x_traffic_disable,
	.traffic_set = rtl930x_traffic_set,
	.l2_ctrl_0 = RTL930X_L2_CTRL,
	.l2_ctrl_1 = RTL930X_L2_AGE_CTRL,
	.l2_port_aging_out = RTL930X_L2_PORT_AGE_CTRL,
	.set_ageing_time = rtl930x_set_ageing_time,
	.smi_poll_ctrl = RTL930X_SMI_POLL_CTRL,
	.l2_tbl_flush_ctrl = RTL930X_L2_TBL_FLUSH_CTRL,
	.exec_tbl0_cmd = rtl930x_exec_tbl0_cmd,
	.exec_tbl1_cmd = rtl930x_exec_tbl1_cmd,
	.tbl_access_data_0 = rtl930x_tbl_access_data_0,
	.isr_glb_src = RTL930X_ISR_GLB,
	.isr_port_link_sts_chg = RTL930X_ISR_PORT_LINK_STS_CHG,
	.imr_port_link_sts_chg = RTL930X_IMR_PORT_LINK_STS_CHG,
	.imr_glb = RTL930X_IMR_GLB,
	.vlan_tables_read = rtl930x_vlan_tables_read,
	.vlan_set_tagged = rtl930x_vlan_set_tagged,
	.vlan_set_untagged = rtl930x_vlan_set_untagged,
	.vlan_profile_dump = rtl930x_vlan_profile_dump,
	.vlan_profile_setup = rtl930x_vlan_profile_setup,
	.vlan_fwd_on_inner = rtl930x_vlan_fwd_on_inner,
	.set_vlan_igr_filter = rtl930x_set_igr_filter,
	.set_vlan_egr_filter = rtl930x_set_egr_filter,
	.stp_get = rtl930x_stp_get,
	.stp_set = rtl930x_stp_set,
	.mac_force_mode_ctrl = rtl930x_mac_force_mode_ctrl,
	.mac_port_ctrl = rtl930x_mac_port_ctrl,
	.l2_port_new_salrn = rtl930x_l2_port_new_salrn,
	.l2_port_new_sa_fwd = rtl930x_l2_port_new_sa_fwd,
	.get_mirror_config = rtldsa_930x_get_mirror_config,
	.port_rate_police_add = rtldsa_930x_port_rate_police_add,
	.port_rate_police_del = rtldsa_930x_port_rate_police_del,
	.read_l2_entry_using_hash = rtl930x_read_l2_entry_using_hash,
	.write_l2_entry_using_hash = rtl930x_write_l2_entry_using_hash,
	.read_cam = rtl930x_read_cam,
	.write_cam = rtl930x_write_cam,
	.vlan_port_keep_tag_set = rtl930x_vlan_port_keep_tag_set,
	.vlan_port_pvidmode_set = rtl930x_vlan_port_pvidmode_set,
	.vlan_port_pvid_set = rtl930x_vlan_port_pvid_set,
	.vlan_port_fast_age = rtldsa_930x_vlan_port_fast_age,
	.trk_mbr_ctr = rtl930x_trk_mbr_ctr,
	.rma_bpdu_fld_pmask = RTL930X_RMA_BPDU_FLD_PMSK,
	.init_eee = rtl930x_init_eee,
	.set_mac_eee = rtldsa_930x_set_mac_eee,
	.l2_hash_seed = rtl930x_l2_hash_seed,
	.l2_hash_key = rtl930x_l2_hash_key,
	.read_mcast_pmask = rtl930x_read_mcast_pmask,
	.write_mcast_pmask = rtl930x_write_mcast_pmask,
	.l2_learning_setup = rtl930x_l2_learning_setup,
	.packet_cntr_read = rtl930x_packet_cntr_read,
	.packet_cntr_clear = rtl930x_packet_cntr_clear,
	.set_distribution_algorithm = rtl930x_set_distribution_algorithm,
	.led_init = rtl930x_led_init,
	.enable_learning = rtldsa_930x_enable_learning,
	.enable_flood = rtldsa_930x_enable_flood,
	.set_receive_management_action = rtldsa_930x_set_receive_management_action,
	.qos_init = rtldsa_930x_qos_init,
};

/* Export the regmap-based register structure */
EXPORT_SYMBOL(rtl930x_regmap_reg);
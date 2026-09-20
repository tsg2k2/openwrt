/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Stub replacement for <asm/mach-rtl-otto/mach-rtl-otto.h>.
 *
 * Used when the RTL83xx DSA driver is built as an out-of-tree module on a
 * non-Realtek host SoC (e.g. IPQ8072 on Verizon CR1000A) where the RTL9301
 * switch is a remote SPI slave rather than the chip the kernel runs on.
 *
 * All register accesses are redirected to the SPI regmap backend provided
 * by rtl930x_regmap.c instead of the on-chip MMIO window at 0xBB000000.
 *
 * Keep every #define and struct identical in name and value to the real
 * header so the driver source files compile without modification.
 */
#ifndef _MACH_RTL838X_H_
#define _MACH_RTL838X_H_

#include <linux/types.h>

/* ---- Register base (unused here, kept for source compatibility) ---- */
#define RTL838X_SW_BASE			((volatile void *)0xBB000000)

/* ---- Chip-identification registers ---- */
#define RTL838X_MODEL_NAME_INFO		(0x00D4)
#define RTL838X_CHIP_INFO		(0x00D8)
#define RTL839X_MODEL_NAME_INFO		(0x0FF0)
#define RTL839X_CHIP_INFO		(0x0FF4)
#define RTL93XX_MODEL_NAME_INFO		(0x0004)
#define RTL93XX_CHIP_INFO		(0x0008)
#define RTL96XX_MODEL_NAME_INFO		(0x10000)
#define RTL96XX_CHIP_INFO		(0x10004)
#define RTL96XX_CHIP_SUB_INFO		(0x10008)

/* ---- Misc register addresses (needed for compilation) ---- */
#define RTL838X_INT_RW_CTRL		(0x0058)
#define RTL838X_EXT_VERSION		(0x00D0)
#define RTL838X_PLL_CML_CTRL		(0x0ff8)
#define RTL931X_LED_GLB_CTRL		(0x0600)
#define RTL931X_MAC_L2_GLOBAL_CTRL2	(0x1358)

/* ---- Family IDs ---- */
#define RTL8380_FAMILY_ID		(0x8380)
#define RTL8390_FAMILY_ID		(0x8390)
#define RTL9300_FAMILY_ID		(0x9300)
#define RTL9310_FAMILY_ID		(0x9310)
#define RTL9607_FAMILY_ID		(0x9607)

/* ---- CPU port numbers ---- */
#define RTL838X_CPU_PORT		28
#define RTL839X_CPU_PORT		52
/* RTL930X: port 27 is the external SerDes CPU uplink (SDS7 → IPQ8072 GMAC5).
 * Port 28 is the internal MIPS CPU bus (disabled on CR1000A); always shows
 * MAC_LINK_STS=UP internally but has no SerDes wiring. hardware testing confirms
 * this: static L2 entries pin router MAC to port 27, and port 27 is the only
 * one with 10G media set and admin enable. */
#define RTL930X_CPU_PORT		27
#define RTL931X_CPU_PORT		56
#define RTL9607_CPU_PORT		9

/* ---- SoC info struct (populated by rtl930x_regmap.c for SPI case) ---- */
struct rtl83xx_soc_info {
	char         name[16];
	char         system_type[64];
	unsigned int id;
	unsigned int family;
	unsigned int revision;
	unsigned int cpu;
	bool         testchip;
	unsigned int subtype;
	int          cpu_port;
	int          memory_size;
};

extern struct rtl83xx_soc_info soc_info;

/* ---- Register accessors via SPI regmap --------------------------------
 *
 * The real header defines these as readl/writel on the MMIO base.
 * Here we call the thin regmap helpers from rtl930x_regmap.h.
 * sw_r32 silently returns 0 on bus errors (same behaviour as a stuck
 * MMIO read); callers that need error propagation should use
 * rtl930x_r32() directly.
 * -------------------------------------------------------------------- */
#include "../../rtl930x_regmap.h"

static __always_inline u32 sw_r32(u32 reg)
{
	u32 v = 0;

	rtl930x_r32(reg, &v);
	return v;
}

static __always_inline void sw_w32(u32 val, u32 reg)
{
	rtl930x_w32(val, reg);
}

static __always_inline void sw_w32_mask(u32 clear, u32 set, u32 reg)
{
	rtl930x_w32_mask(clear, set, reg);
}

#endif /* _MACH_RTL838X_H_ */

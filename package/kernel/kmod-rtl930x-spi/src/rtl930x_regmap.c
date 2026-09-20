// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL930x regmap backend
 *
 * Holds the single live regmap instance and exposes it to the DSA driver.
 * This file has NO dependency on the Realtek MIPS target headers
 * (mach-rtl-otto.h, rtl83xx.h) — it is designed to be built on any host
 * SoC (e.g. IPQ8072) that accesses the RTL930x as a remote SPI slave.
 */

#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/device.h>

#include "rtl930x_regmap.h"
#include "asm/mach-rtl-otto/mach-rtl-otto.h"

static struct regmap *rtl930x_regmap;
static bool rtl930x_use_spi;

/*
 * soc_info — normally populated by the Realtek MIPS prom code at boot.
 * For the external SPI case we pre-fill it here; the DSA driver reads
 * soc_info.family to select the RTL930x ops table.
 */
struct rtl83xx_soc_info soc_info = {
	.name      = "RTL930x",
	.family    = RTL9300_FAMILY_ID,
	.cpu_port  = RTL930X_CPU_PORT,
};
EXPORT_SYMBOL(soc_info);

int rtl930x_regmap_init_spi(struct device *dev, struct regmap *spi_regmap)
{
	if (!spi_regmap) {
		dev_err(dev, "rtl930x: NULL spi_regmap\n");
		return -EINVAL;
	}
	rtl930x_regmap  = spi_regmap;
	rtl930x_use_spi = true;
	dev_dbg(dev, "rtl930x: SPI regmap ready\n");
	return 0;
}
EXPORT_SYMBOL_GPL(rtl930x_regmap_init_spi);

void rtl930x_regmap_exit(void)
{
	rtl930x_regmap  = NULL;
	rtl930x_use_spi = false;
}
EXPORT_SYMBOL_GPL(rtl930x_regmap_exit);

struct regmap *rtl930x_get_regmap(void)
{
	return rtl930x_regmap;
}
EXPORT_SYMBOL_GPL(rtl930x_get_regmap);

bool rtl930x_is_spi_mode(void)
{
	return rtl930x_use_spi;
}
EXPORT_SYMBOL_GPL(rtl930x_is_spi_mode);

MODULE_DESCRIPTION("RTL930x regmap backend");
MODULE_LICENSE("GPL v2");

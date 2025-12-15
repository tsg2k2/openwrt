/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _RTL930X_REGMAP_H
#define _RTL930X_REGMAP_H

#include <linux/regmap.h>
#include <linux/device.h>

/* RTL930x regmap initialization and cleanup functions */
int rtl930x_regmap_init(struct device *dev, void __iomem *base);
int rtl930x_regmap_init_spi(struct device *dev, struct regmap *spi_regmap);
void rtl930x_regmap_exit(void);

/* Regmap access functions */
struct regmap *rtl930x_get_regmap(void);
bool rtl930x_is_spi_mode(void);

/* SPI interface functions */
struct regmap *rtl930x_spi_get_regmap(void);
bool rtl930x_spi_is_available(void);

/* External register structure for regmap-based RTL930x */
extern const struct rtl838x_reg rtl930x_regmap_reg;

/* Regmap-based register access functions */
u32 rtl930x_hash(struct rtl838x_switch_priv *priv, u64 seed);
irqreturn_t rtldsa_930x_switch_irq(int irq, void *dev_id);
void rtl930x_vlan_profile_dump(int index);
void rtl930x_print_matrix(void);
void rtl9300_dump_debug(void);

#endif /* _RTL930X_REGMAP_H */
/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _RTL930X_REGMAP_H
#define _RTL930X_REGMAP_H

#include <linux/regmap.h>
#include <linux/device.h>

int  rtl930x_regmap_init_spi(struct device *dev, struct regmap *spi_regmap);
void rtl930x_regmap_exit(void);

struct regmap *rtl930x_get_regmap(void);
bool           rtl930x_is_spi_mode(void);

/*
 * Inline helpers — upper layers call these instead of sw_r32/sw_w32.
 * They return 0 on success or a negative errno so callers can propagate
 * errors rather than silently reading 0 from a dead bus.
 */
static inline int rtl930x_r32(u32 reg, u32 *val)
{
	struct regmap *map = rtl930x_get_regmap();

	if (!map)
		return -ENODEV;
	return regmap_read(map, reg, val);
}

static inline int rtl930x_w32(u32 val, u32 reg)
{
	struct regmap *map = rtl930x_get_regmap();

	if (!map)
		return -ENODEV;
	return regmap_write(map, reg, val);
}

static inline int rtl930x_w32_mask(u32 clear, u32 set, u32 reg)
{
	struct regmap *map = rtl930x_get_regmap();

	if (!map)
		return -ENODEV;
	return regmap_update_bits(map, reg, clear | set, set);
}

#endif /* _RTL930X_REGMAP_H */

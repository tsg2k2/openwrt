/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _RTL930X_SPI_H
#define _RTL930X_SPI_H

#include <linux/spi/spi.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/gpio/consumer.h>

/* SPI protocol:
 *   [CMD:1][ADDR_H:1][ADDR_L:1][DUMMY:1][DATA:4]  = 8 bytes total
 *
 * The RTL930x SPI slave uses a 2-byte (16-bit) register address followed by
 * one mandatory dummy/turnaround byte before data.  All RTL930x registers
 * fit within 16-bit address space (max ~0xCF00).  Data is big-endian on the
 * wire; callers receive/send host-endian 32-bit values. */
#define RTL930X_SPI_CMD_READ		0x03
#define RTL930X_SPI_CMD_WRITE		0x02
#define RTL930X_SPI_MAX_SPEED_HZ	12000000
#define RTL930X_SPI_CMD_BYTES		1
#define RTL930X_SPI_ADDR_BYTES		2
#define RTL930X_SPI_DUMMY_BYTES		1
#define RTL930X_SPI_DATA_BYTES		4
#define RTL930X_SPI_FRAME_BYTES		(RTL930X_SPI_CMD_BYTES + RTL930X_SPI_ADDR_BYTES + \
					 RTL930X_SPI_DUMMY_BYTES + RTL930X_SPI_DATA_BYTES)

/* RTL930x chip model register — used for ID check at probe.
 * Bits [31:16] encode the chip model (0x9300..0x930F); bits [15:0] are
 * silicon revision.  Accept any RTL930x variant (9301, 9302, 9303 …). */
#define RTL930X_MODEL_NAME_INFO		0x0004
#define RTL930X_CHIP_FAMILY_MASK	0xFF000000
#define RTL930X_CHIP_FAMILY_RTL930X	0x93000000

/* 2-byte address → 16-bit space; highest known RTL930x register ~0xCF00 */
#define RTL930X_MAX_REGISTER		0xFFFF

struct platform_device;

struct rtl930x_spi_priv {
	struct spi_device	*spi;
	struct regmap		*regmap;
	struct mutex		 lock;
	struct gpio_desc	*reset_gpio;
	struct gpio_desc	*cs_gpio;	/* only if DT specifies cs-gpios */
	u32			 max_speed_hz;
	bool			 use_gpio_cs;
	bool			 initialized;

	/* Switch child node + its platform device: the syscon regmap is
	 * registered on sw_np, and the node's own children (the MDIO
	 * controller) are populated below sw_pdev. */
	struct device_node	*sw_np;
	struct platform_device	*sw_pdev;

	/* Re-assert of the silicon CPU-port designation (0xc70c bit0): the HW
	 * clears it on every CPU-port (dp5) link-down.  A netdev notifier fires
	 * the work on interface carrier-up; the work re-asserts only if cleared. */
	struct delayed_work	 cpu_tag_work;
	struct notifier_block	 netdev_nb;

	/* Current IP-mode value (0x02 SGMII / 0x16 2500BASEX / 0x1a 10GBase-R)
	 * configured into each SerDes.  Tracked so the DSA mac_config hook can
	 * skip a costly re-init when the requested mode is already active. */

	/* SMI-poll managed-port mask: switch ports whose PHY is on the RTL9303's
	 * own SMI/MDIO bus and is phylib-managed (our mii_bus drives them), so
	 * they must be removed from the HW SMI poller.  Derived from the DT at
	 * probe (phy-handle resolving to switch@0/mdio). */
	u32			 smi_managed_mask;
};

/* Exported so the DSA/regmap layer can retrieve the regmap handle */
struct regmap *rtl930x_spi_get_regmap(void);
bool rtl930x_spi_is_available(void);

/* Reconfigure a SerDes to a new IP mode at runtime (SGMII <-> 2500BASEX
 * follow for the RTL8221B user ports).  No-op if the mode is unchanged.
 * mode: RTL930X_SDS_MODE_SGMII (0x02) / _2500BASEX (0x16) / _10GBASER (0x1a). */

/* RTL9303 chip-fixed switch-port -> SerDes-lane map (silicon architecture,
 * identical on every RTL9303 board).  Ports without a SerDes return -1. */
int rtl9303_port_to_sds(u32 port);

#endif /* _RTL930X_SPI_H */

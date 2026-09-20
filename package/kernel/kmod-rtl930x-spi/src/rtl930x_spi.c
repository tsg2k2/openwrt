// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL930x SPI slave access driver
 *
 * Provides register access to an RTL9301/RTL9302/RTL9303 switch chip
 * connected as an SPI slave (e.g. IPQ8072 → SPI5 → RTL9301 on CR1000A).
 * Exposes a regmap that upper layers (DSA driver, switchdev, debugfs) use
 * instead of the MMIO sw_r32/sw_w32 macros used when Linux runs on-chip.
 */

#include <linux/bitfield.h>
#include <linux/cdev.h>
#include <linux/compat.h>
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/spi/spidev.h>
#include <linux/regmap.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/mfd/syscon.h>
#include <linux/platform_device.h>
#include <linux/of_mdio.h>
#include <linux/phy.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/ktime.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <net/dsa.h>

#include "rtl930x_spi.h"
#include "rtl930x_regmap.h"

/*
 * The SerDes bring-up is owned by the RTL-Otto PCS (pcs-rtl-otto.c, copied
 * at build time from the realtek target): phylink configures each lane via
 * the per-port pcs-handle.  This driver only registers the embedded platform
 * drivers (rtpcs_spi.h) and spawns their child devices.
 */
#include "rtpcs_spi.h"

/* Module-level pointer; set once per successful probe, cleared on remove.
 * Protected by the priv->lock for per-transfer use; the pointer itself is
 * only written from probe/remove which are serialised by the SPI core. */
static struct rtl930x_spi_priv *g_priv;

/* -------------------------------------------------------------------------
 * SPI register callbacks (called by regmap, already locked by regmap core)
 * ------------------------------------------------------------------------- */

static int rtl930x_spi_reg_read(void *context, unsigned int reg,
				 unsigned int *val)
{
	struct rtl930x_spi_priv *priv = context;
	/* Frame: [CMD][ADDR_H][ADDR_L][DUMMY][D3][D2][D1][D0] = 8 bytes.
	 * Data is big-endian in rx[4..7]; rx[0..3] are don't-care. */
	u8 tx[RTL930X_SPI_FRAME_BYTES] = {
		RTL930X_SPI_CMD_READ,
		(reg >> 8) & 0xFF,
		 reg       & 0xFF,
		0,		/* dummy turnaround byte */
		0, 0, 0, 0,	/* dummy TX while clocking in data */
	};
	u8 rx[RTL930X_SPI_FRAME_BYTES] = {};
	struct spi_transfer xfer = { .tx_buf = tx, .rx_buf = rx,
				     .len = sizeof(tx) };
	struct spi_message msg;
	int ret;

	if (priv->use_gpio_cs)
		gpiod_set_value_cansleep(priv->cs_gpio, 0);

	spi_message_init_with_transfers(&msg, &xfer, 1);
	ret = spi_sync(priv->spi, &msg);

	if (priv->use_gpio_cs)
		gpiod_set_value_cansleep(priv->cs_gpio, 1);

	if (ret) {
		dev_err(&priv->spi->dev, "SPI read reg 0x%x failed: %d\n",
			reg, ret);
		return ret;
	}

	*val = ((u32)rx[4] << 24) | ((u32)rx[5] << 16) |
	       ((u32)rx[6] << 8)  |  (u32)rx[7];
	return 0;
}

static int rtl930x_spi_reg_write(void *context, unsigned int reg,
				  unsigned int val)
{
	struct rtl930x_spi_priv *priv = context;
	/* Frame: [CMD][ADDR_H][ADDR_L][DUMMY][D3][D2][D1][D0] = 8 bytes. */
	u8 tx[RTL930X_SPI_FRAME_BYTES] = {
		RTL930X_SPI_CMD_WRITE,
		(reg >> 8) & 0xFF,
		 reg       & 0xFF,
		0,		/* dummy turnaround byte */
		(val >> 24) & 0xFF,
		(val >> 16) & 0xFF,
		(val >>  8) & 0xFF,
		 val        & 0xFF,
	};
	struct spi_transfer xfer = { .tx_buf = tx, .len = sizeof(tx) };
	struct spi_message msg;
	int ret;

	if (priv->use_gpio_cs)
		gpiod_set_value_cansleep(priv->cs_gpio, 0);

	spi_message_init_with_transfers(&msg, &xfer, 1);
	ret = spi_sync(priv->spi, &msg);

	if (priv->use_gpio_cs)
		gpiod_set_value_cansleep(priv->cs_gpio, 1);

	if (ret)
		dev_err(&priv->spi->dev, "SPI write reg 0x%x failed: %d\n",
			reg, ret);
	return ret;
}

/* regmap_bus: we supply raw callbacks so regmap does NOT apply any internal
 * byte-swapping on top of what we already do in the callbacks above. */
static const struct regmap_bus rtl930x_spi_regmap_bus = {
	.reg_write = rtl930x_spi_reg_write,
	.reg_read  = rtl930x_spi_reg_read,
};

static const struct regmap_config rtl930x_spi_regmap_cfg = {
	.reg_bits		= 32,
	.val_bits		= 32,
	.reg_stride		= 4,
	.max_register		= RTL930X_MAX_REGISTER,
	.cache_type		= REGCACHE_NONE,
	.use_single_read	= true,
	.use_single_write	= true,
};

/* Silicon CPU-port designation: bit0 = 1 (port 27) / 0 (port 28).  The
 * RTL9303 only inserts the inline 0x8899 CPU tag (needed for forwarding to/
 * from the IPQ8072) when this is set, and the HW clears it on every CPU-port
 * (dp5) link-down.  See rtl930x_cpu_tag_work() / rtl930x_spi_netdev_event(). */
#define RTL930X_MAC_L2_CPU_PORT_CTRL	0xc70c
#define RTL930X_CPU_TAG_INIT_DELAY_MS	8000	/* initial CPU-port designate after probe */

/* HW SMI poller enable mask (SMI_POLL_CTRL, per-port bitmask).  DSA enables it
 * for every linked port (ports 8/20/24/25/27 -> 0x0b100100).  Two problems with
 * it polling our phylib-managed user ports (8/20/24):
 *   1. Its autonomous use of the shared SMI master (cb70-cb7c) over the slow SPI
 *      bus corrupts our software mii_bus transactions -- handled per-transaction
 *      by rtl930x_smi_poll_pause() (the documented SMI polling workaround).
 *   2. It also drives each polled port's switch-side link state, so it FIGHTS
 *      phylink's software-forced link on the managed ports: the port links up,
 *      the poller's own PHY read knocks it down ~1s later, and it oscillates /
 *      sticks down (the lan1/lan2 "flap, then never recover" symptom).
 * Fix: keep the poller ON only for the ports we do NOT phylib-manage -- the CPU
 * port (27, where it maintains the IPQ uplink MAC link; a sustained full ca90=0
 * decays it) and MoCA (25) -- and clear the managed-port bits after DSA setup
 * (rtl930x_cpu_tag_work, re-fired by the carrier-up notifier).  HW-confirmed:
 * with ports 8/20/24 removed, lan2/2.5G links stably and forwards (in_octets
 * climbs, 10/10 ping) where it previously flapped. */
#define RTL930X_SMI_POLL_CTRL		0xca90
/* Ports we manage via the software mii_bus -- remove from the HW poll mask. */
#define RTL930X_SMI_POLL_MANAGED_MASK	(BIT(8) | BIT(20) | BIT(24))

/* RTL930x MDIO-master / SMI config registers written after switch init */
#define RTL930X_SMI_GLB_CTRL		0xca00
#define RTL930X_SMI_MAC_TYPE_CTRL	0xca04
#define RTL930X_SMI_10G_POLL_REG0	0xcbb4
#define RTL930X_SMI_10G_POLL_REG9	0xcbb8
#define RTL930X_SMI_10G_POLL_REG10	0xcbbc

/* RTL9303 SMI manual PHY access registers (Longan SDK swcore_rtl9300.h) */
#define RTL930X_SMI_ACCESS_PHY_CTRL_0	0xcb70	/* PHY bitmask [27:0] */
#define RTL930X_SMI_ACCESS_PHY_CTRL_1	0xcb74	/* CMD[0], TYPE[1], RWOP[2], FAIL[25] */
#define RTL930X_SMI_ACCESS_PHY_CTRL_2	0xcb78	/* INDATA[31:16], DATA[15:0] */
#define RTL930X_SMI_ACCESS_PHY_CTRL_3	0xcb7c	/* MMD_DEVAD[20:16], MMD_REG[15:0] */
#define RTL930X_SMI_PHY_CTRL1_CMD	BIT(0)
#define RTL930X_SMI_PHY_CTRL1_TYPE_C45	BIT(1)
#define RTL930X_SMI_PHY_CTRL1_WRITE	BIT(2)
#define RTL930X_SMI_PHY_CTRL1_FAIL	BIT(25)

/* -------------------------------------------------------------------------
 * RTL930x internal MDIO bus (switch SMI master)
 *
 * The RTL9303 has an internal SMI/MDIO master that reaches the per-port PHYs
 * (RTL8221B on ports 20/24, AQR/RTL8224 on port 8).  Accesses go through the
 * same manual PHY-access registers used by rtl930x_spi_phy_write_c45() above.
 *
 * Indexing is by *switch port number*, not raw MDIO address: CTRL_0 carries a
 * port bitmask (for writes), and CTRL_2[31:16] carries the port number (for
 * reads).  We therefore make the mii_bus address equal the port number — the
 * DT PHY nodes use reg = <port> (20, 24, ...).  Register/sequence semantics
 * mirror the upstream mdio-realtek-otto.c rtmd_930x_* helpers.
 * ------------------------------------------------------------------------- */

/* C22 page/reg encoding in CTRL_1: reg[24:20], fixed 0x1f marker[19:15],
 * page[14:3] (12 bits, holds the Realtek paged-register page). */
#define RTL930X_SMI_C22_DATA(page, reg) \
	((((reg) & 0x1f) << 20) | (0x1f << 15) | (((page) & 0xfff) << 3))

/* Poll the SMI master until the command completes; returns 0/-ETIMEDOUT/-EIO. */
static int rtl930x_smi_wait(struct rtl930x_spi_priv *priv)
{
	unsigned int ctrl1 = 0;
	int tries = 200;

	while (tries--) {
		regmap_read(priv->regmap, RTL930X_SMI_ACCESS_PHY_CTRL_1, &ctrl1);
		if (!(ctrl1 & RTL930X_SMI_PHY_CTRL1_CMD))
			break;
		udelay(100);
	}
	if (ctrl1 & RTL930X_SMI_PHY_CTRL1_CMD)
		return -ETIMEDOUT;
	if (ctrl1 & RTL930X_SMI_PHY_CTRL1_FAIL)
		return -EIO;
	return 0;
}

/* Core C22 read for a given (addr, page, reg). Retries on FAIL like c45. */
static int rtl930x_smi_read_c22(struct rtl930x_spi_priv *priv, int addr,
				u16 page, int reg)
{
	unsigned int v;
	int ret, retry;

	/* DSA's SMI setup resets SMI_GLB_CTRL to all-Clause-45, which breaks
	 * software C22 access to the RTL8221B PHYs on buses 0/1 (ports 20/24):
	 * read_status then returns EIO and the link never recovers.  Re-assert
	 * C22 for buses 0,1 (clear INTF_SEL bits) before every access. */
	regmap_update_bits(priv->regmap, RTL930X_SMI_GLB_CTRL,
			   BIT(16) | BIT(17), 0);

	for (retry = 0; retry < 100; retry++) {
		regmap_write(priv->regmap, RTL930X_SMI_ACCESS_PHY_CTRL_3, 0);
		regmap_write(priv->regmap, RTL930X_SMI_ACCESS_PHY_CTRL_2,
			     (u32)(addr & 0x3f) << 16);	/* port number, hi word */
		regmap_write(priv->regmap, RTL930X_SMI_ACCESS_PHY_CTRL_0, 0);
		regmap_write(priv->regmap, RTL930X_SMI_ACCESS_PHY_CTRL_1,
			     RTL930X_SMI_C22_DATA(page, reg) | RTL930X_SMI_PHY_CTRL1_CMD);
		ret = rtl930x_smi_wait(priv);
		if (ret != -EIO)
			break;
		msleep(10);
	}
	if (ret == -EIO)
		return 0xffff;
	if (ret)
		return ret;

	regmap_read(priv->regmap, RTL930X_SMI_ACCESS_PHY_CTRL_2, &v);
	return v & 0xffff;
}

/* Clause-22 with Realtek paging: reg 0x1f selects the page in software (the
 * page is baked into every other access via C22_DATA), mirroring the upstream
 * mdio-realtek-otto rtmd_*_c22 helpers.  Used by the realtek PHY driver in
 * C22 mode (is_c45=false) to reach its vendor (paged) registers. */
/*
 * Make the SPI regmap available to the standard switch-block consumers:
 * register it as the syscon regmap of the switch child node, so the
 * (backported) realtek,rtl9301-mdio controller and other in-kernel users
 * obtain it via syscon_node_to_regmap(np->parent) exactly as they would on
 * a memory-mapped RTL930x.  Must run before the child platform devices are
 * populated.
 */
static int rtl930x_spi_register_syscon(struct rtl930x_spi_priv *priv)
{
	struct device *dev = &priv->spi->dev;
	struct device_node *sw_np;
	int ret;

	sw_np = of_get_child_by_name(dev->of_node, "switch");
	if (!sw_np) {
		dev_dbg(dev, "no switch child node; nothing to expose\n");
		return 0;
	}

	ret = of_syscon_register_regmap(sw_np, priv->regmap);
	if (ret)
		dev_err(dev, "failed to register switch syscon regmap: %d\n", ret);

	priv->sw_np = sw_np;
	return ret;
}

/* The per-port PHYs (RTL8221B) don't answer MDIO reads immediately at boot —
 * the SMI master returns FAIL until the PHY is up.  The MDIO controller
 * driver probes the DT-listed PHYs synchronously, so wait for the slowest
 * known PHY (RTL8221B on port 20) to respond before populating children. */
static void rtl930x_spi_wait_phys_ready(struct rtl930x_spi_priv *priv)
{
	struct device *dev = &priv->spi->dev;
	int tries, id = -1;

	for (tries = 0; tries < 150; tries++) {	/* up to ~3 s */
		/* C22 PHYID1 — manual SMI access addresses by switch port. */
		id = rtl930x_smi_read_c22(priv, 20, 0, MII_PHYSID1);
		if (id >= 0 && id != 0xffff)
			break;
		msleep(20);
	}
	if (id >= 0 && id != 0xffff)
		dev_dbg(dev, "switch PHYs ready after %d ms (id1=0x%04x)\n",
			tries * 20, id);
	else
		dev_warn(dev, "switch PHY @port 20 not responding (last=%d); continuing\n",
			 id);
}

/*
 * Build the SMI-poll managed-port mask from the DT: switch ports whose
 * phy-handle resolves to a PHY on the RTL9303's OWN mdio bus (switch@0/mdio).
 * Those PHYs are driven by our SPI-backed mii_bus, so they must be removed
 * from the hardware SMI poller (which would otherwise fight phylink's forced
 * link).  Ports whose phy-handle points elsewhere (e.g. MoCA on the SoC's
 * 90000.mdio) are NOT on our bus and stay polled.  Returns the port bitmask.
 */
static u32 rtl930x_spi_build_managed_mask(struct rtl930x_spi_priv *priv)
{
	struct device_node *sw, *ports, *mdio, *pn;
	u32 mask = 0;

	sw = of_get_child_by_name(priv->spi->dev.of_node, "switch");
	if (!sw)
		return 0;
	mdio  = of_get_child_by_name(sw, "mdio-controller");
	ports = of_get_child_by_name(sw, "ethernet-ports");
	of_node_put(sw);
	if (!ports) {
		of_node_put(mdio);
		return 0;
	}

	for_each_available_child_of_node(ports, pn) {
		struct device_node *phy;
		u32 port;

		if (of_property_read_u32(pn, "reg", &port))
			continue;
		phy = of_parse_phandle(pn, "phy-handle", 0);
		if (!phy)
			continue;	/* fixed-link / CPU uplink — not managed */
		if (mdio && phy->parent && phy->parent->parent == mdio)
			mask |= BIT(port);	/* PHY on our SMI bus */
		of_node_put(phy);
	}
	of_node_put(ports);
	of_node_put(mdio);
	return mask;
}

/* -------------------------------------------------------------------------
 * Chip initialisation helpers
 * ------------------------------------------------------------------------- */

static int rtl930x_spi_reset_chip(struct rtl930x_spi_priv *priv)
{
	if (!priv->reset_gpio)
		return 0;

	gpiod_set_value_cansleep(priv->reset_gpio, 0); /* assert */
	msleep(10);
	gpiod_set_value_cansleep(priv->reset_gpio, 1); /* deassert */
	msleep(100);
	return 0;
}

static int rtl930x_spi_check_chip_id(struct rtl930x_spi_priv *priv)
{
	/* 8-byte frame: [CMD][ADDR_H][ADDR_L][DUMMY][D3][D2][D1][D0].
	 * Print the full rx buffer so any remaining protocol mismatch shows. */
	u8 tx[RTL930X_SPI_FRAME_BYTES] = {
		RTL930X_SPI_CMD_READ,
		(RTL930X_MODEL_NAME_INFO >> 8) & 0xFF,
		 RTL930X_MODEL_NAME_INFO       & 0xFF,
		0,
		0, 0, 0, 0,
	};
	u8 rx[RTL930X_SPI_FRAME_BYTES] = {};
	struct spi_transfer xfer = { .tx_buf = tx, .rx_buf = rx,
				     .len = sizeof(tx) };
	struct spi_message msg;
	unsigned int id;
	int ret;

	spi_message_init_with_transfers(&msg, &xfer, 1);
	ret = spi_sync(priv->spi, &msg);
	if (ret) {
		dev_err(&priv->spi->dev, "SPI chip ID read failed: %d\n", ret);
		return ret;
	}

	dev_dbg(&priv->spi->dev,
		 "SPI rx[cmd+addr+dummy | data]: %02x %02x %02x %02x | %02x %02x %02x %02x\n",
		 rx[0], rx[1], rx[2], rx[3], rx[4], rx[5], rx[6], rx[7]);

	id = ((u32)rx[4] << 24) | ((u32)rx[5] << 16) |
	     ((u32)rx[6] << 8)  |  (u32)rx[7];

	if ((id & RTL930X_CHIP_FAMILY_MASK) == RTL930X_CHIP_FAMILY_RTL930X)
		dev_dbg(&priv->spi->dev,
			 "RTL930x detected: chip 0x%04x rev 0x%04x\n",
			 id >> 16, id & 0xFFFF);
	else
		dev_warn(&priv->spi->dev,
			 "unexpected chip ID 0x%08x (expected RTL930x)\n", id);

	return 0;
}

#ifdef RTL930X_SPI_SPIDEV
/* -------------------------------------------------------------------------
 * Embedded spidev passthrough character device
 *
 * Creates /dev/spidev{bus}.{cs} (e.g. /dev/spidev1.0) while the rtl930x-spi
 * driver owns the SPI device, so userspace tools can issue
 * raw SPI transfers in parallel with normal driver operation.  The SPI core's
 * transfer queue serializes this driver's regmap transactions with any
 * userspace transfers automatically.
 * ------------------------------------------------------------------------- */

#define RTL930X_SPIDEV_XFER_MAX  4096

static dev_t         rtl930x_spidev_devno;
static struct class *rtl930x_spidev_class;
static struct cdev   rtl930x_spidev_cdev;
static bool          rtl930x_spidev_active;

static int rtl930x_spidev_open(struct inode *inode, struct file *filp)
{
	if (!g_priv)
		return -ENODEV;
	filp->private_data = g_priv;
	return nonseekable_open(inode, filp);
}

static int rtl930x_spidev_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static long rtl930x_spidev_ioctl_message(struct rtl930x_spi_priv *priv,
					  struct spi_ioc_transfer __user *u_ioc,
					  unsigned int n_xfers)
{
	struct spi_ioc_transfer *ioc;
	struct spi_message       msg;
	struct spi_transfer     *xfers;
	u8                      *buf;
	size_t                   buf_size = 0;
	long                     ret;
	unsigned int             i;

	ioc = memdup_user(u_ioc, n_xfers * sizeof(*ioc));
	if (IS_ERR(ioc))
		return PTR_ERR(ioc);

	for (i = 0; i < n_xfers; i++) {
		if (ioc[i].len > RTL930X_SPIDEV_XFER_MAX) {
			kfree(ioc);
			return -EINVAL;
		}
		buf_size += ioc[i].len;
	}

	xfers = kcalloc(n_xfers, sizeof(*xfers), GFP_KERNEL);
	buf   = kzalloc(buf_size * 2, GFP_KERNEL); /* tx then rx, per xfer */
	if (!xfers || !buf) {
		ret = -ENOMEM;
		goto out;
	}

	spi_message_init(&msg);
	{
		u8 *p = buf;

		for (i = 0; i < n_xfers; i++) {
			u8 *tx = p, *rx = p + ioc[i].len;

			p += ioc[i].len * 2;

			if (ioc[i].tx_buf &&
			    copy_from_user(tx, u64_to_user_ptr(ioc[i].tx_buf),
					   ioc[i].len)) {
				ret = -EFAULT;
				goto out;
			}
			xfers[i].tx_buf        = ioc[i].tx_buf ? tx : NULL;
			xfers[i].rx_buf        = ioc[i].rx_buf ? rx : NULL;
			xfers[i].len           = ioc[i].len;
			xfers[i].speed_hz      = ioc[i].speed_hz ?: priv->spi->max_speed_hz;
			xfers[i].bits_per_word = ioc[i].bits_per_word ?: priv->spi->bits_per_word;
			xfers[i].delay.value   = ioc[i].delay_usecs;
			xfers[i].delay.unit    = SPI_DELAY_UNIT_USECS;
			xfers[i].cs_change     = ioc[i].cs_change;
			spi_message_add_tail(&xfers[i], &msg);
		}
	}

	ret = spi_sync(priv->spi, &msg);
	if (ret)
		goto out;

	{
		u8 *p = buf;

		for (i = 0; i < n_xfers; i++) {
			u8 *rx = p + ioc[i].len;

			if (ioc[i].rx_buf &&
			    copy_to_user(u64_to_user_ptr(ioc[i].rx_buf), rx, ioc[i].len)) {
				ret = -EFAULT;
				goto out;
			}
			p += ioc[i].len * 2;
		}
	}
	ret = (long)buf_size;

out:
	kfree(buf);
	kfree(xfers);
	kfree(ioc);
	return ret;
}

static long rtl930x_spidev_ioctl(struct file *filp, unsigned int cmd,
				  unsigned long arg)
{
	struct rtl930x_spi_priv *priv = filp->private_data;

	if (_IOC_TYPE(cmd) != SPI_IOC_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case SPI_IOC_RD_MODE:
		return put_user((u8)(priv->spi->mode & 0xff),
				(__u8 __user *)arg) ? -EFAULT : 0;
	case SPI_IOC_RD_MODE32:
		return put_user(priv->spi->mode,
				(__u32 __user *)arg) ? -EFAULT : 0;
	case SPI_IOC_WR_MODE:
	case SPI_IOC_WR_MODE32:
		return 0; /* mode is fixed; silently accept writes */
	case SPI_IOC_RD_BITS_PER_WORD:
		return put_user(priv->spi->bits_per_word,
				(__u8 __user *)arg) ? -EFAULT : 0;
	case SPI_IOC_WR_BITS_PER_WORD:
		return 0;
	case SPI_IOC_RD_MAX_SPEED_HZ:
		return put_user(priv->spi->max_speed_hz,
				(__u32 __user *)arg) ? -EFAULT : 0;
	case SPI_IOC_WR_MAX_SPEED_HZ:
		return 0;
	case SPI_IOC_RD_LSB_FIRST:
		return put_user((u8)(!!(priv->spi->mode & SPI_LSB_FIRST)),
				(__u8 __user *)arg) ? -EFAULT : 0;
	case SPI_IOC_WR_LSB_FIRST:
		return 0;
	default:
		if (_IOC_NR(cmd) != _IOC_NR(SPI_IOC_MESSAGE(0)))
			return -ENOTTY;
		{
			unsigned int n = _IOC_SIZE(cmd) /
					 sizeof(struct spi_ioc_transfer);
			return n ? rtl930x_spidev_ioctl_message(
					priv,
					(struct spi_ioc_transfer __user *)arg,
					n) : 0;
		}
	}
}

static const struct file_operations rtl930x_spidev_fops = {
	.owner          = THIS_MODULE,
	.open           = rtl930x_spidev_open,
	.release        = rtl930x_spidev_release,
	.unlocked_ioctl = rtl930x_spidev_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
	.llseek         = noop_llseek,
};

static int rtl930x_spidev_create(struct spi_device *spi)
{
	struct device *d;
	int            ret;
	int bus = spi->controller->bus_num;
	int cs  = spi_get_chipselect(spi, 0);

	ret = alloc_chrdev_region(&rtl930x_spidev_devno, 0, 1, "rtl930x-spidev");
	if (ret)
		return ret;

	/* Use a unique class name to avoid conflicting with spidev.ko if loaded */
	rtl930x_spidev_class = class_create("rtl930x-spi");
	if (IS_ERR(rtl930x_spidev_class)) {
		ret = PTR_ERR(rtl930x_spidev_class);
		rtl930x_spidev_class = NULL;
		goto err_region;
	}

	cdev_init(&rtl930x_spidev_cdev, &rtl930x_spidev_fops);
	rtl930x_spidev_cdev.owner = THIS_MODULE;
	ret = cdev_add(&rtl930x_spidev_cdev, rtl930x_spidev_devno, 1);
	if (ret)
		goto err_class;

	d = device_create(rtl930x_spidev_class, &spi->dev,
			  rtl930x_spidev_devno, NULL,
			  "spidev%d.%d", bus, cs);
	if (IS_ERR(d)) {
		ret = PTR_ERR(d);
		goto err_cdev;
	}

	rtl930x_spidev_active = true;
	dev_dbg(&spi->dev,
		 "RTL930x: /dev/spidev%d.%d available for userspace SPI access\n",
		 bus, cs);
	return 0;

err_cdev:
	cdev_del(&rtl930x_spidev_cdev);
err_class:
	class_destroy(rtl930x_spidev_class);
	rtl930x_spidev_class = NULL;
err_region:
	unregister_chrdev_region(rtl930x_spidev_devno, 1);
	return ret;
}

static void rtl930x_spidev_destroy(struct spi_device *spi)
{
	if (!rtl930x_spidev_active)
		return;
	device_destroy(rtl930x_spidev_class, rtl930x_spidev_devno);
	cdev_del(&rtl930x_spidev_cdev);
	class_destroy(rtl930x_spidev_class);
	rtl930x_spidev_class = NULL;
	unregister_chrdev_region(rtl930x_spidev_devno, 1);
	rtl930x_spidev_active = false;
}
#else /* !RTL930X_SPI_SPIDEV */
/* Raw-SPI passthrough is a debug-only aid (run external SPI tools against the
 * switch while this driver owns it).  Compiled out by default; enable with
 * -DRTL930X_SPI_SPIDEV (see src/Makefile). */
static inline int rtl930x_spidev_create(struct spi_device *spi) { return 0; }
static inline void rtl930x_spidev_destroy(struct spi_device *spi) {}
#endif /* RTL930X_SPI_SPIDEV */

/* -------------------------------------------------------------------------
 * Probe / remove
 * ------------------------------------------------------------------------- */

/*
 * Re-assert the silicon CPU-port designation (MAC_L2_CPU_PORT_CTRL @ 0xc70c
 * bit0 = 1 -> port 27).  The RTL9303 HW clears it every time the CPU-port (dp5
 * conduit / SDS7) link goes DOWN, and dp5 bounces during the DSA/nss-dp conduit
 * bring-up; with it cleared the chip inserts no inline 0x8899 CPU tag and
 * forwarding is dead in both directions on every port.  Fired by the netdev
 * notifier on every interface carrier-up; re-asserts only if read back cleared
 * (idempotent -- a no-op otherwise).
 *
 * The same work also removes our phylib-managed ports (8/20/24) from the HW SMI
 * poll mask (SMI_POLL_CTRL @ 0xca90).  DSA re-enables polling for each port as
 * it links during bring-up, and the poller then fights phylink's forced link on
 * those ports (the lan1/lan2 flap-then-stick-down symptom); clearing the managed
 * bits here -- while leaving the CPU(27)/MoCA(25) bits the poller needs -- lets
 * phylink own their link state.  Both actions are idempotent no-ops once settled.
 */
/* The re-assert must not fire while the DSA tree runs the 802.1Q tagger:
 * it would re-enable inline 0x8899 tag insertion underneath a tagger that
 * expects plain 802.1Q frames. The conduit's active tagger (swapped under
 * rtnl by the DSA core on protocol changes) is the authoritative source;
 * before any conduit exists, default to the OTTO boot behaviour.
 */
static bool rtl930x_inline_tag_wanted(void)
{
	struct net_device *dev;
	bool wanted = true;

	rtnl_lock();
	for_each_netdev(&init_net, dev) {
		if (dev->dsa_ptr) {
			wanted = (dev->dsa_ptr->tag_ops->proto ==
				  DSA_TAG_PROTO_RTL_OTTO);
			break;
		}
	}
	rtnl_unlock();

	return wanted;
}

static void rtl930x_cpu_tag_work(struct work_struct *work)
{
	struct rtl930x_spi_priv *priv = container_of(to_delayed_work(work),
					struct rtl930x_spi_priv, cpu_tag_work);
	u32 val = 0;

	/* Drop the managed user ports from the HW poll mask if DSA re-added them. */
	regmap_read(priv->regmap, RTL930X_SMI_POLL_CTRL, &val);
	if (val & priv->smi_managed_mask) {
		regmap_update_bits(priv->regmap, RTL930X_SMI_POLL_CTRL,
				   priv->smi_managed_mask, 0);
		dev_dbg(&priv->spi->dev,
			 "smi-poll: cleared managed ports from 0xca90 (was 0x%x) at uptime %lus — phylink owns ports 8/20/24 link\n",
			 val, (unsigned long)ktime_get_seconds());
	}

	val = 0;
	regmap_read(priv->regmap, RTL930X_MAC_L2_CPU_PORT_CTRL, &val);

	if (!rtl930x_inline_tag_wanted()) {
		if (val & BIT(0)) {
			regmap_update_bits(priv->regmap,
					   RTL930X_MAC_L2_CPU_PORT_CTRL,
					   BIT(0), 0);
			dev_dbg(&priv->spi->dev,
				"cpu-tag: cleared 0xc70c bit0 (802.1Q tagger active)\n");
		}
		return;
	}

	if (val & BIT(0))
		return;		/* still designated -- nothing to do */

	regmap_update_bits(priv->regmap, RTL930X_MAC_L2_CPU_PORT_CTRL,
			   BIT(0), BIT(0));
	dev_dbg(&priv->spi->dev,
		 "cpu-tag: re-asserted 0xc70c bit0 (was 0x%x) at uptime %lus — CPU-port designate restored\n",
		 val, (unsigned long)ktime_get_seconds());
}

/*
 * On any interface carrier-up, kick the work to re-check/re-assert the
 * CPU-port designation.  The event we care about is the dp5 conduit coming
 * back up after a boot-time bounce (which clears 0xc70c); firing on others is
 * harmless.  Deferring to the work keeps regmap/SPI access in sleepable ctx.
 */
static int rtl930x_spi_netdev_event(struct notifier_block *nb,
				    unsigned long event, void *ptr)
{
	struct rtl930x_spi_priv *priv = container_of(nb,
					struct rtl930x_spi_priv, netdev_nb);
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);

	if ((event == NETDEV_CHANGE || event == NETDEV_UP) &&
	    dev && netif_carrier_ok(dev))
		schedule_delayed_work(&priv->cpu_tag_work, 0);

	return NOTIFY_DONE;
}

static int rtl930x_spi_probe(struct spi_device *spi)
{
	struct rtl930x_spi_priv *priv;
	u32 speed;
	int ret;

	priv = devm_kzalloc(&spi->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->spi = spi;
	mutex_init(&priv->lock);

	/* Speed: honour DT, cap at driver maximum */
	if (of_property_read_u32(spi->dev.of_node, "spi-max-frequency", &speed))
		speed = RTL930X_SPI_MAX_SPEED_HZ;
	priv->max_speed_hz = min(speed, (u32)RTL930X_SPI_MAX_SPEED_HZ);

	/* Optional GPIO reset */
	priv->reset_gpio = devm_gpiod_get_optional(&spi->dev, "reset",
						   GPIOD_OUT_HIGH);
	if (IS_ERR(priv->reset_gpio))
		return PTR_ERR(priv->reset_gpio);

	/* Optional manual GPIO chip-select.
	 * When present, tell the SPI controller to leave its own CS alone so
	 * we don't double-toggle it. */
	priv->cs_gpio = devm_gpiod_get_optional(&spi->dev, "cs",
						GPIOD_OUT_HIGH);
	if (IS_ERR(priv->cs_gpio))
		return PTR_ERR(priv->cs_gpio);

	priv->use_gpio_cs = (priv->cs_gpio != NULL);

	/* Configure the SPI device */
	spi->mode          = SPI_MODE_0;
	spi->max_speed_hz  = priv->max_speed_hz;
	spi->bits_per_word = 8;
	if (priv->use_gpio_cs)
		spi->mode |= SPI_NO_CS; /* controller must not touch its CS pin */

	ret = spi_setup(spi);
	if (ret) {
		dev_err(&spi->dev, "spi_setup failed: %d\n", ret);
		return ret;
	}

	/* Hardware reset before first register access */
	ret = rtl930x_spi_reset_chip(priv);
	if (ret)
		return ret;

	/* Build the regmap backed by our SPI callbacks */
	priv->regmap = devm_regmap_init(&spi->dev, &rtl930x_spi_regmap_bus,
					priv, &rtl930x_spi_regmap_cfg);
	if (IS_ERR(priv->regmap)) {
		dev_err(&spi->dev, "regmap_init failed: %ld\n",
			PTR_ERR(priv->regmap));
		return PTR_ERR(priv->regmap);
	}

	/* Verify we can talk to the chip before handing off to the DSA layer */
	ret = rtl930x_spi_check_chip_id(priv);
	if (ret)
		return ret;

	/* Post-init SMI/MDIO master config:
	 * configure RTL9303 MDIO master polling for AQR113C and MXL3711. */
	/* SMI_GLB_CTRL: bit (16+bus) selects Clause-45 (1) vs Clause-22 (0) per
	 * SMI bus.  The RTL8221B PHYs (ports 20=bus0, 24=bus1) must run in C22
	 * mode so the Linux realtek driver can reach their vendor (paged)
	 * registers via C22 paging — over plain C45 those return 0xDEAD and the
	 * driver's config_init crashes.  Ports 8/25 (AQR/MoCA, bus2) stay C45.
	 * 0x0fbf5500 (all C45) → 0x0fbc5500 (buses 0,1 = C22). */
	regmap_write(priv->regmap, RTL930X_SMI_GLB_CTRL,    0x0fbc5500);
	regmap_write(priv->regmap, RTL930X_SMI_10G_POLL_REG0, 0x01010000);
	regmap_write(priv->regmap, RTL930X_SMI_10G_POLL_REG9, 0x01e7c400);
	regmap_write(priv->regmap, RTL930X_SMI_10G_POLL_REG10, 0x01e7e820);
	regmap_write(priv->regmap, RTL930X_SMI_MAC_TYPE_CTRL, 0x000017df);
	regmap_write(priv->regmap, 0xca80, 0x000001af);

	/* SMI master port->PHY-address topology + per-port C22/C45 poll select.
	 * The manual PHY access (used by our internal MDIO bus) resolves the
	 * target PHY from the *port number* via these registers; without them
	 * the SMI master cannot reach the per-port PHYs (RTL8221B on 20/24) and
	 * reads FAIL.  The rtl83xx DSA setup programs the same values later from
	 * the DT, but that runs long after this probe — so we must set them here
	 * for the MDIO bus registration (and early phylink connect) to work.
	 * Values for the CR1000A switch. */
	regmap_write(priv->regmap, 0xca08, 0x55560000);	/* SMI_PORT0_15_POLL_SEL  */
	regmap_write(priv->regmap, 0xca0c, 0x00f1a8aa);	/* SMI_PORT16_27_POLL_SEL */
	regmap_write(priv->regmap, 0xcb80, 0x0a418820);	/* PORT_ADDR_CTRL  0-5    */
	regmap_write(priv->regmap, 0xcb84, 0x16a480e6);	/* PORT_ADDR_CTRL  6-11   */
	regmap_write(priv->regmap, 0xcb88, 0x2307b9ac);	/* PORT_ADDR_CTRL 12-17   */
	regmap_write(priv->regmap, 0xcb8c, 0x2f6a9e72);	/* PORT_ADDR_CTRL 18-23 (p20) */
	regmap_write(priv->regmap, 0xcb90, 0x000de9e3);	/* PORT_ADDR_CTRL 24-29 (p24) */
	regmap_write(priv->regmap, 0xcb98, 0x00f00000);

	/* The SerDes bring-up reads/writes the SerDes pages through the
	 * otto-serdes-mdio bus, which resolves our regmap through the switch
	 * node's syscon.  Register the syscon and spawn that child device
	 * before touching any SerDes; the bus driver was registered by the
	 * module init, so the bind is synchronous. */
	ret = rtl930x_spi_register_syscon(priv);
	if (ret)
		dev_warn(&spi->dev, "switch syscon regmap unavailable: %d\n", ret);

	if (priv->sw_np) {
		static const char * const early_children[] = {
			"realtek,otto-serdes-mdio",	/* SerDes register bus */
			"realtek,otto-pcs",		/* PCS (needs the bus) */
		};

		for (int i = 0; i < ARRAY_SIZE(early_children); i++) {
			struct device_node *np =
				of_get_compatible_child(priv->sw_np,
							early_children[i]);

			if (!np)
				continue;
			if (!of_platform_device_create(np, NULL, &spi->dev))
				dev_warn(&spi->dev, "%s child device creation failed\n",
					 early_children[i]);
			of_node_put(np);
		}
	}

	/* SMI-poll managed-port mask, derived from the DT (phy-handle on the
	 * switch's own mdio).  Falls back to the static mask if the DT walk
	 * yields nothing. */
	priv->smi_managed_mask = rtl930x_spi_build_managed_mask(priv);
	if (!priv->smi_managed_mask)
		priv->smi_managed_mask = RTL930X_SMI_POLL_MANAGED_MASK;

	/* Flush the RTL9303 L2 table before DSA registers.  After a soft reset the
	 * chip retains SRAM state, so pre-existing static entries (router_MAC → port 27)
	 * persist.  The MAC address in pre-existing entries may differ from OpenWrt's
	 * MAC; flush ensures a clean slate so our driver's dynamic L2 learning
	 * starts from scratch.
	 *
	 * Register RTL930X_L2_TBL_FLUSH_CTRL = 0x9404.
	 *   BIT(30): EXEC — write 1 to start, hardware clears when done.
	 *   BIT(29): flush static entries too (hypothesis; BIT(30) alone only
	 *            flushes dynamic entries — verify empirically if needed).
	 * Writing 0 to the +4 register clears all port/VID comparators, so the
	 * flush applies to every entry in the table. */
	{
		u32 flush_val;
		int i;

		regmap_write(priv->regmap, 0x9408, 0);
		regmap_write(priv->regmap, 0x9404, BIT(30) | BIT(29));
		for (i = 0; i < 100; i++) {
			regmap_read(priv->regmap, 0x9404, &flush_val);
			if (!(flush_val & BIT(30)))
				break;
			usleep_range(500, 1000);
		}
		if (flush_val & BIT(30))
			dev_warn(&spi->dev, "RTL930x: L2 flush timed out\n");
		else
			dev_dbg(&spi->dev, "RTL930x: L2 table flushed\n");
	}

	/* Force RTL8221B MAC-side SerDes to fixed HSGMII (2500BASE-X) with rate adaptation.
	 * Default mode 0 (RTL822X_VND1_SERDES_OPTION_MODE_2500BASEX_SGMII) drops to SGMII
	 * when the line-side peer is 1G, causing a MAC/PHY mismatch since RTL9303 SerDes 5/6
	 * are fixed at 2500BASE-X.  Mode 2 (MODE_2500BASEX) keeps HSGMII on the MAC side
	 * and uses rate adaptation for both 1G and 2.5G line-side speeds.
	 * MMD device 1, reg 0x697a (RTL822X_VND1_SERDES_OPTION): PHY addr 0x14=port20, 0x18=port24 */
	/* Hand regmap to the upper DSA/regmap layer.
	 * Do this AFTER all initialisation so that g_priv is never visible
	 * in a partially-initialised state. */
	ret = rtl930x_regmap_init_spi(&spi->dev, priv->regmap);
	if (ret) {
		dev_err(&spi->dev, "rtl930x_regmap_init_spi failed: %d\n", ret);
		return ret;
	}

	priv->initialized = true;
	g_priv = priv;
	spi_set_drvdata(spi, priv);

	dev_info(&spi->dev, "RTL930x SPI driver ready (%u Hz%s)\n",
		 priv->max_speed_hz,
		 priv->use_gpio_cs ? ", GPIO CS" : "");

	/* Expose /dev/spidev{bus}.{cs} for userspace raw access alongside normal
	 * driver operation.  Failure here is non-fatal — DSA still works. */
	if (rtl930x_spidev_create(spi))
		dev_warn(&spi->dev, "RTL930x: spidev passthrough device unavailable\n");

	/* Spawn the remaining child platform devices: the
	 * realtek,rtl9301-switch node (rtl83xx DSA driver) and, below it, the
	 * realtek,rtl9301-mdio controller which provides the SMI buses as
	 * standard MDIO buses (the syscon regmap was registered before the
	 * SerDes bring-up).  Wait for the slow RTL8221B PHYs first so the MDIO
	 * controller's synchronous PHY probe succeeds. */
	rtl930x_spi_wait_phys_ready(priv);

	of_platform_populate(spi->dev.of_node, NULL, NULL, &spi->dev);

	if (priv->sw_np) {
		struct platform_device *sw_pdev =
			of_find_device_by_node(priv->sw_np);

		if (sw_pdev) {
			of_platform_populate(priv->sw_np, NULL, NULL,
					     &sw_pdev->dev);
			priv->sw_pdev = sw_pdev;
		}
	}

	/* CPU-port-designate (0xc70c) re-assert: a netdev notifier re-fires the
	 * work on every interface carrier-up (the dp5 conduit bounces clear the
	 * bit in HW); the initial schedule covers the first designate. */
	INIT_DELAYED_WORK(&priv->cpu_tag_work, rtl930x_cpu_tag_work);
	priv->netdev_nb.notifier_call = rtl930x_spi_netdev_event;
	if (register_netdevice_notifier(&priv->netdev_nb))
		dev_warn(&spi->dev,
			 "cpu-tag: netdev notifier registration failed; CPU-port designate won't self-heal on dp5 bounce\n");
	schedule_delayed_work(&priv->cpu_tag_work,
			      msecs_to_jiffies(RTL930X_CPU_TAG_INIT_DELAY_MS));

	return 0;
}

static void rtl930x_spi_remove(struct spi_device *spi)
{
	struct rtl930x_spi_priv *priv = spi_get_drvdata(spi);

	if (priv) {
		unregister_netdevice_notifier(&priv->netdev_nb);
		cancel_delayed_work_sync(&priv->cpu_tag_work);
		rtl930x_spidev_destroy(spi);
		if (priv->sw_pdev) {
			of_platform_depopulate(&priv->sw_pdev->dev);
			put_device(&priv->sw_pdev->dev);
		}
		of_platform_depopulate(&spi->dev);
		of_node_put(priv->sw_np);
		rtl930x_regmap_exit();
		g_priv = NULL;
	}
}

/* -------------------------------------------------------------------------
 * Exported accessors for the DSA/regmap layer
 * ------------------------------------------------------------------------- */

struct regmap *rtl930x_spi_get_regmap(void)
{
	return g_priv ? g_priv->regmap : NULL;
}
EXPORT_SYMBOL_GPL(rtl930x_spi_get_regmap);

bool rtl930x_spi_is_available(void)
{
	return g_priv && g_priv->initialized;
}
EXPORT_SYMBOL_GPL(rtl930x_spi_is_available);

/* -------------------------------------------------------------------------
 * Module boilerplate
 * ------------------------------------------------------------------------- */

static const struct of_device_id rtl930x_spi_of_match[] = {
	{ .compatible = "realtek,rtl9300-spi" },
	{ .compatible = "realtek,rtl9301-spi" },
	{}
};
MODULE_DEVICE_TABLE(of, rtl930x_spi_of_match);

static const struct spi_device_id rtl930x_spi_id[] = {
	{ "rtl9300-spi", 0 },
	{ "rtl9301-spi", 0 },
	{}
};
MODULE_DEVICE_TABLE(spi, rtl930x_spi_id);

static struct spi_driver rtl930x_spi_driver = {
	.driver = {
		.name           = "rtl930x-spi",
		.of_match_table = rtl930x_spi_of_match,
	},
	.probe    = rtl930x_spi_probe,
	.remove   = rtl930x_spi_remove,
	.id_table = rtl930x_spi_id,
};
/*
 * The module bundles two platform drivers taken from the realtek target
 * (the otto-serdes-mdio bus and the RTL-Otto PCS).  Register them before
 * the SPI driver: the SPI probe spawns their child devices and relies on
 * the binds happening synchronously.
 */
static int __init rtl930x_spi_module_init(void)
{
	int ret;

	ret = platform_driver_register(rtl930x_serdes_mdio_driver);
	if (ret)
		return ret;

	ret = platform_driver_register(rtl930x_pcs_platform_driver);
	if (ret)
		goto err_serdes;

	ret = spi_register_driver(&rtl930x_spi_driver);
	if (ret)
		goto err_pcs;

	return 0;

err_pcs:
	platform_driver_unregister(rtl930x_pcs_platform_driver);
err_serdes:
	platform_driver_unregister(rtl930x_serdes_mdio_driver);
	return ret;
}
module_init(rtl930x_spi_module_init);

static void __exit rtl930x_spi_module_exit(void)
{
	spi_unregister_driver(&rtl930x_spi_driver);
	platform_driver_unregister(rtl930x_pcs_platform_driver);
	platform_driver_unregister(rtl930x_serdes_mdio_driver);
}
module_exit(rtl930x_spi_module_exit);

MODULE_DESCRIPTION("RTL930x SPI slave register access");
MODULE_AUTHOR("OpenWrt");
MODULE_LICENSE("GPL v2");

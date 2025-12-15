// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL930x SPI slave access driver
 * 
 * This driver provides SPI slave interface support for RTL930x switch,
 * allowing register access via SPI bus instead of memory-mapped I/O.
 */

#include <linux/spi/spi.h>
#include <linux/regmap.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>

#include "rtl83xx.h"
#include "rtl930x_regmap.h"

#define RTL930X_SPI_READ_CMD		0x03
#define RTL930X_SPI_WRITE_CMD		0x02
#define RTL930X_SPI_MAX_SPEED_HZ	10000000  /* 10 MHz */
#define RTL930X_SPI_ADDR_BYTES		4
#define RTL930X_SPI_DATA_BYTES		4
#define RTL930X_SPI_CMD_BYTES		1
#define RTL930X_SPI_TOTAL_BYTES		(RTL930X_SPI_CMD_BYTES + RTL930X_SPI_ADDR_BYTES + RTL930X_SPI_DATA_BYTES)

/* SPI protocol frame format:
 * READ:  [CMD:1][ADDR:4][DATA:4] - CMD=0x03, ADDR=register, DATA=read_data
 * WRITE: [CMD:1][ADDR:4][DATA:4] - CMD=0x02, ADDR=register, DATA=write_data
 */

struct rtl930x_spi_priv {
	struct spi_device *spi;
	struct regmap *regmap;
	struct mutex spi_lock;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *cs_gpio;
	u32 max_speed_hz;
	bool use_cs_gpio;
};

static struct rtl930x_spi_priv *rtl930x_spi_priv;

/* SPI register read function */
static int rtl930x_spi_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	struct rtl930x_spi_priv *priv = context;
	struct spi_device *spi = priv->spi;
	u8 tx_buf[RTL930X_SPI_CMD_BYTES + RTL930X_SPI_ADDR_BYTES] = {0};
	u8 rx_buf[RTL930X_SPI_DATA_BYTES] = {0};
	struct spi_transfer xfers[] = {
		{
			.tx_buf = tx_buf,
			.len = sizeof(tx_buf),
		},
		{
			.rx_buf = rx_buf,
			.len = sizeof(rx_buf),
		},
	};
	struct spi_message msg;
	int ret;

	if (!priv || !spi || !val)
		return -EINVAL;

	/* Prepare command and address */
	tx_buf[0] = RTL930X_SPI_READ_CMD;
	tx_buf[1] = (reg >> 24) & 0xFF;
	tx_buf[2] = (reg >> 16) & 0xFF;
	tx_buf[3] = (reg >> 8) & 0xFF;
	tx_buf[4] = reg & 0xFF;

	mutex_lock(&priv->spi_lock);

	/* Assert CS if using GPIO CS */
	if (priv->use_cs_gpio && priv->cs_gpio)
		gpiod_set_value_cansleep(priv->cs_gpio, 0);

	spi_message_init(&msg);
	spi_message_add_tail(&xfers[0], &msg);
	spi_message_add_tail(&xfers[1], &msg);

	ret = spi_sync(spi, &msg);

	/* Deassert CS if using GPIO CS */
	if (priv->use_cs_gpio && priv->cs_gpio)
		gpiod_set_value_cansleep(priv->cs_gpio, 1);

	mutex_unlock(&priv->spi_lock);

	if (ret) {
		dev_err(&spi->dev, "SPI read failed for reg 0x%x: %d\n", reg, ret);
		return ret;
	}

	/* Convert received data to host byte order */
	*val = (rx_buf[0] << 24) | (rx_buf[1] << 16) | (rx_buf[2] << 8) | rx_buf[3];

	dev_dbg(&spi->dev, "SPI read reg 0x%x = 0x%x\n", reg, *val);

	return 0;
}

/* SPI register write function */
static int rtl930x_spi_reg_write(void *context, unsigned int reg, unsigned int val)
{
	struct rtl930x_spi_priv *priv = context;
	struct spi_device *spi = priv->spi;
	u8 tx_buf[RTL930X_SPI_TOTAL_BYTES] = {0};
	struct spi_transfer xfer = {
		.tx_buf = tx_buf,
		.len = sizeof(tx_buf),
	};
	struct spi_message msg;
	int ret;

	if (!priv || !spi)
		return -EINVAL;

	/* Prepare command, address, and data */
	tx_buf[0] = RTL930X_SPI_WRITE_CMD;
	tx_buf[1] = (reg >> 24) & 0xFF;
	tx_buf[2] = (reg >> 16) & 0xFF;
	tx_buf[3] = (reg >> 8) & 0xFF;
	tx_buf[4] = reg & 0xFF;
	tx_buf[5] = (val >> 24) & 0xFF;
	tx_buf[6] = (val >> 16) & 0xFF;
	tx_buf[7] = (val >> 8) & 0xFF;
	tx_buf[8] = val & 0xFF;

	mutex_lock(&priv->spi_lock);

	/* Assert CS if using GPIO CS */
	if (priv->use_cs_gpio && priv->cs_gpio)
		gpiod_set_value_cansleep(priv->cs_gpio, 0);

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	ret = spi_sync(spi, &msg);

	/* Deassert CS if using GPIO CS */
	if (priv->use_cs_gpio && priv->cs_gpio)
		gpiod_set_value_cansleep(priv->cs_gpio, 1);

	mutex_unlock(&priv->spi_lock);

	if (ret) {
		dev_err(&spi->dev, "SPI write failed for reg 0x%x: %d\n", reg, ret);
		return ret;
	}

	dev_dbg(&spi->dev, "SPI write reg 0x%x = 0x%x\n", reg, val);

	return 0;
}

/* Regmap bus configuration for SPI */
static const struct regmap_bus rtl930x_spi_regmap_bus = {
	.reg_write = rtl930x_spi_reg_write,
	.reg_read = rtl930x_spi_reg_read,
	.reg_format_endian_default = REGMAP_ENDIAN_BIG,
	.val_format_endian_default = REGMAP_ENDIAN_BIG,
};

/* Regmap configuration for SPI access */
static const struct regmap_config rtl930x_spi_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0xFFFF,
	.cache_type = REGCACHE_NONE,
	.use_single_read = true,
	.use_single_write = true,
	.can_multi_write = false,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_BIG,
};

/* Reset the RTL930x chip via GPIO */
static int rtl930x_spi_reset_chip(struct rtl930x_spi_priv *priv)
{
	if (!priv->reset_gpio)
		return 0;

	dev_info(&priv->spi->dev, "Resetting RTL930x chip\n");

	/* Assert reset (active low) */
	gpiod_set_value_cansleep(priv->reset_gpio, 0);
	msleep(100);

	/* Deassert reset */
	gpiod_set_value_cansleep(priv->reset_gpio, 1);
	msleep(200);

	return 0;
}

/* Initialize SPI interface */
static int rtl930x_spi_init(struct rtl930x_spi_priv *priv)
{
	struct spi_device *spi = priv->spi;
	int ret;

	/* Configure SPI mode and speed */
	spi->mode = SPI_MODE_0;
	spi->max_speed_hz = priv->max_speed_hz;
	spi->bits_per_word = 8;

	ret = spi_setup(spi);
	if (ret) {
		dev_err(&spi->dev, "Failed to setup SPI: %d\n", ret);
		return ret;
	}

	/* Reset chip if reset GPIO is available */
	ret = rtl930x_spi_reset_chip(priv);
	if (ret) {
		dev_err(&spi->dev, "Failed to reset chip: %d\n", ret);
		return ret;
	}

	/* Initialize CS GPIO if specified */
	if (priv->use_cs_gpio && priv->cs_gpio) {
		gpiod_direction_output(priv->cs_gpio, 1); /* CS high (inactive) */
	}

	dev_info(&spi->dev, "RTL930x SPI interface initialized (speed: %d Hz)\n", 
		 spi->max_speed_hz);

	return 0;
}

/* Test SPI communication by reading chip ID */
static int rtl930x_spi_test_communication(struct rtl930x_spi_priv *priv)
{
	unsigned int chip_id;
	int ret;

	/* Try to read chip identification register */
	ret = rtl930x_spi_reg_read(priv, RTL838X_MODEL_NAME_INFO, &chip_id);
	if (ret) {
		dev_err(&priv->spi->dev, "Failed to read chip ID via SPI: %d\n", ret);
		return ret;
	}

	dev_info(&priv->spi->dev, "RTL930x chip ID: 0x%08x\n", chip_id);

	/* Verify this is an RTL930x chip */
	if ((chip_id & 0xFFFF0000) != 0x93000000) {
		dev_warn(&priv->spi->dev, "Unexpected chip ID: 0x%08x\n", chip_id);
	}

	return 0;
}

/* Parse device tree properties */
static int rtl930x_spi_parse_dt(struct rtl930x_spi_priv *priv)
{
	struct device *dev = &priv->spi->dev;
	struct device_node *np = dev->of_node;
	u32 speed;

	/* Get SPI speed from device tree */
	if (!of_property_read_u32(np, "spi-max-frequency", &speed)) {
		priv->max_speed_hz = min(speed, RTL930X_SPI_MAX_SPEED_HZ);
	} else {
		priv->max_speed_hz = RTL930X_SPI_MAX_SPEED_HZ;
	}

	/* Get reset GPIO */
	priv->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->reset_gpio)) {
		dev_err(dev, "Failed to get reset GPIO: %ld\n", PTR_ERR(priv->reset_gpio));
		return PTR_ERR(priv->reset_gpio);
	}

	/* Get CS GPIO (optional, for manual CS control) */
	priv->cs_gpio = devm_gpiod_get_optional(dev, "cs", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->cs_gpio)) {
		dev_warn(dev, "Failed to get CS GPIO, using SPI controller CS: %ld\n", 
			 PTR_ERR(priv->cs_gpio));
		priv->cs_gpio = NULL;
		priv->use_cs_gpio = false;
	} else if (priv->cs_gpio) {
		priv->use_cs_gpio = true;
		dev_info(dev, "Using GPIO CS control\n");
	} else {
		priv->use_cs_gpio = false;
	}

	dev_info(dev, "SPI max frequency: %d Hz\n", priv->max_speed_hz);

	return 0;
}

/* SPI driver probe function */
static int rtl930x_spi_probe(struct spi_device *spi)
{
	struct rtl930x_spi_priv *priv;
	int ret;

	dev_info(&spi->dev, "RTL930x SPI driver probe\n");

	/* Allocate private data */
	priv = devm_kzalloc(&spi->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->spi = spi;
	mutex_init(&priv->spi_lock);

	/* Parse device tree properties */
	ret = rtl930x_spi_parse_dt(priv);
	if (ret)
		return ret;

	/* Initialize SPI interface */
	ret = rtl930x_spi_init(priv);
	if (ret)
		return ret;

	/* Create regmap for register access */
	priv->regmap = devm_regmap_init(&spi->dev, &rtl930x_spi_regmap_bus,
					priv, &rtl930x_spi_regmap_config);
	if (IS_ERR(priv->regmap)) {
		dev_err(&spi->dev, "Failed to create regmap: %ld\n", PTR_ERR(priv->regmap));
		return PTR_ERR(priv->regmap);
	}

	/* Test SPI communication */
	ret = rtl930x_spi_test_communication(priv);
	if (ret) {
		dev_err(&spi->dev, "SPI communication test failed: %d\n", ret);
		return ret;
	}

	/* Set global SPI private data for use by regmap functions */
	rtl930x_spi_priv = priv;
	spi_set_drvdata(spi, priv);

	/* Initialize RTL930x regmap with SPI backend */
	ret = rtl930x_regmap_init_spi(&spi->dev, priv->regmap);
	if (ret) {
		dev_err(&spi->dev, "Failed to initialize RTL930x regmap: %d\n", ret);
		return ret;
	}

	dev_info(&spi->dev, "RTL930x SPI driver probe completed successfully\n");

	return 0;
}

/* SPI driver remove function */
static int rtl930x_spi_remove(struct spi_device *spi)
{
	struct rtl930x_spi_priv *priv = spi_get_drvdata(spi);

	dev_info(&spi->dev, "RTL930x SPI driver remove\n");

	if (priv) {
		rtl930x_regmap_exit();
		rtl930x_spi_priv = NULL;
	}

	return 0;
}

/* Device tree match table */
static const struct of_device_id rtl930x_spi_of_match[] = {
	{ .compatible = "realtek,rtl9300-spi", },
	{ .compatible = "realtek,rtl9302-spi", },
	{ .compatible = "realtek,rtl9303-spi", },
	{ }
};
MODULE_DEVICE_TABLE(of, rtl930x_spi_of_match);

/* SPI device ID table */
static const struct spi_device_id rtl930x_spi_id[] = {
	{ "rtl9300-spi", 0 },
	{ "rtl9302-spi", 0 },
	{ "rtl9303-spi", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, rtl930x_spi_id);

/* SPI driver structure */
static struct spi_driver rtl930x_spi_driver = {
	.driver = {
		.name = "rtl930x-spi",
		.of_match_table = rtl930x_spi_of_match,
	},
	.probe = rtl930x_spi_probe,
	.remove = rtl930x_spi_remove,
	.id_table = rtl930x_spi_id,
};

/* Get SPI regmap instance */
struct regmap *rtl930x_spi_get_regmap(void)
{
	if (rtl930x_spi_priv)
		return rtl930x_spi_priv->regmap;
	return NULL;
}
EXPORT_SYMBOL(rtl930x_spi_get_regmap);

/* Check if SPI interface is available */
bool rtl930x_spi_is_available(void)
{
	return (rtl930x_spi_priv != NULL);
}
EXPORT_SYMBOL(rtl930x_spi_is_available);

module_spi_driver(rtl930x_spi_driver);

MODULE_DESCRIPTION("Realtek RTL930x SPI slave access driver");
MODULE_AUTHOR("OpenWrt RTL930x Team");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("spi:rtl930x");
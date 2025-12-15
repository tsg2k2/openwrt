/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _RTL930X_SPI_H
#define _RTL930X_SPI_H

#include <linux/spi/spi.h>
#include <linux/regmap.h>

/* SPI Protocol Commands */
#define RTL930X_SPI_CMD_READ		0x03
#define RTL930X_SPI_CMD_WRITE		0x02
#define RTL930X_SPI_CMD_FAST_READ	0x0B
#define RTL930X_SPI_CMD_BURST_READ	0x13
#define RTL930X_SPI_CMD_BURST_WRITE	0x12

/* SPI Timing Parameters */
#define RTL930X_SPI_MAX_SPEED_HZ	10000000  /* 10 MHz */
#define RTL930X_SPI_MIN_SPEED_HZ	1000000   /* 1 MHz */
#define RTL930X_SPI_SETUP_TIME_US	10
#define RTL930X_SPI_HOLD_TIME_US	5
#define RTL930X_SPI_CS_SETUP_US		2
#define RTL930X_SPI_CS_HOLD_US		2

/* SPI Frame Format */
#define RTL930X_SPI_ADDR_BYTES		4
#define RTL930X_SPI_DATA_BYTES		4
#define RTL930X_SPI_CMD_BYTES		1
#define RTL930X_SPI_DUMMY_BYTES		1  /* For fast read */

/* SPI Register Access Modes */
enum rtl930x_spi_mode {
	RTL930X_SPI_MODE_NORMAL = 0,
	RTL930X_SPI_MODE_FAST,
	RTL930X_SPI_MODE_BURST,
};

/* SPI Configuration Structure */
struct rtl930x_spi_config {
	u32 max_speed_hz;
	u32 min_speed_hz;
	enum rtl930x_spi_mode mode;
	bool use_gpio_cs;
	bool use_interrupts;
	u8 cs_setup_time;
	u8 cs_hold_time;
	u8 dummy_cycles;
};

/* SPI Statistics */
struct rtl930x_spi_stats {
	u64 read_count;
	u64 write_count;
	u64 error_count;
	u64 timeout_count;
	u64 bytes_transferred;
	u32 max_transfer_time_us;
	u32 avg_transfer_time_us;
};

/* SPI Private Data Structure */
struct rtl930x_spi_priv {
	struct spi_device *spi;
	struct regmap *regmap;
	struct mutex spi_lock;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *cs_gpio;
	struct rtl930x_spi_config config;
	struct rtl930x_spi_stats stats;
	bool initialized;
};

/* Function Prototypes */

/* Core SPI functions */
int rtl930x_spi_init(struct rtl930x_spi_priv *priv);
void rtl930x_spi_cleanup(struct rtl930x_spi_priv *priv);
int rtl930x_spi_reset_chip(struct rtl930x_spi_priv *priv);

/* Register access functions */
int rtl930x_spi_reg_read(void *context, unsigned int reg, unsigned int *val);
int rtl930x_spi_reg_write(void *context, unsigned int reg, unsigned int val);
int rtl930x_spi_reg_read_bulk(void *context, unsigned int reg, 
			      void *val, size_t val_count);
int rtl930x_spi_reg_write_bulk(void *context, unsigned int reg,
			       const void *val, size_t val_count);

/* Configuration functions */
int rtl930x_spi_set_speed(struct rtl930x_spi_priv *priv, u32 speed_hz);
int rtl930x_spi_set_mode(struct rtl930x_spi_priv *priv, enum rtl930x_spi_mode mode);
void rtl930x_spi_get_stats(struct rtl930x_spi_priv *priv, struct rtl930x_spi_stats *stats);
void rtl930x_spi_reset_stats(struct rtl930x_spi_priv *priv);

/* Utility functions */
bool rtl930x_spi_is_available(void);
struct regmap *rtl930x_spi_get_regmap(void);
int rtl930x_spi_test_communication(struct rtl930x_spi_priv *priv);

/* Debug functions */
void rtl930x_spi_dump_config(struct rtl930x_spi_priv *priv);
void rtl930x_spi_dump_stats(struct rtl930x_spi_priv *priv);

/* Error codes */
#define RTL930X_SPI_ERR_TIMEOUT		-ETIMEDOUT
#define RTL930X_SPI_ERR_INVALID_CMD	-EINVAL
#define RTL930X_SPI_ERR_BUS_ERROR	-EIO
#define RTL930X_SPI_ERR_CS_ERROR	-ENODEV

/* SPI Register Definitions for RTL930x */
#define RTL930X_SPI_CTRL_REG		0x0000
#define RTL930X_SPI_STATUS_REG		0x0004
#define RTL930X_SPI_CONFIG_REG		0x0008
#define RTL930X_SPI_INT_MASK_REG	0x000C
#define RTL930X_SPI_INT_STATUS_REG	0x0010

/* SPI Control Register Bits */
#define RTL930X_SPI_CTRL_ENABLE		BIT(0)
#define RTL930X_SPI_CTRL_RESET		BIT(1)
#define RTL930X_SPI_CTRL_MODE_MASK	GENMASK(3, 2)
#define RTL930X_SPI_CTRL_SPEED_MASK	GENMASK(7, 4)

/* SPI Status Register Bits */
#define RTL930X_SPI_STATUS_READY	BIT(0)
#define RTL930X_SPI_STATUS_BUSY		BIT(1)
#define RTL930X_SPI_STATUS_ERROR	BIT(2)
#define RTL930X_SPI_STATUS_TIMEOUT	BIT(3)

/* Macros for SPI operations */
#define RTL930X_SPI_READ_TIMEOUT_MS	100
#define RTL930X_SPI_WRITE_TIMEOUT_MS	100
#define RTL930X_SPI_RESET_TIMEOUT_MS	1000

#define rtl930x_spi_is_ready(priv) \
	(spi_get_drvdata((priv)->spi) && (priv)->initialized)

#define rtl930x_spi_lock(priv) \
	mutex_lock(&(priv)->spi_lock)

#define rtl930x_spi_unlock(priv) \
	mutex_unlock(&(priv)->spi_lock)

#endif /* _RTL930X_SPI_H */
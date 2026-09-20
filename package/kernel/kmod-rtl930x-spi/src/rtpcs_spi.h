/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Platform drivers embedded in rtl930x-spi.ko alongside the SPI access
 * driver: the RTL-Otto PCS (pcs-rtl-otto.c) and the SerDes MDIO bus
 * (mdio-realtek-otto-serdes.c), both copied at build time from the realtek
 * target.  The rtl930x_spi module init registers them before the SPI probe
 * spawns their child devices, so the binds are synchronous and ordered.
 */
#ifndef _RTPCS_SPI_H
#define _RTPCS_SPI_H

struct platform_driver;

extern struct platform_driver *rtl930x_pcs_platform_driver;
extern struct platform_driver *rtl930x_serdes_mdio_driver;

#endif /* _RTPCS_SPI_H */

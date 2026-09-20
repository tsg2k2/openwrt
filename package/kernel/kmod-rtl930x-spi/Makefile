include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=kmod-rtl930x-spi
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0

# The rtl83xx DSA driver, the RTL-Otto PCS and the otto-serdes-mdio bus
# driver are all taken from the in-tree realtek target and patched.
RTL83XX_SRC:=$(TOPDIR)/target/linux/realtek/files-6.18/drivers/net/dsa/rtl83xx
RTINC_SRC:=$(TOPDIR)/target/linux/realtek/files-6.18/include/linux/soc/realtek
RTSOC_SRC:=$(TOPDIR)/target/linux/realtek/files-6.18/drivers/soc/realtek
RTPCS_SRC:=$(TOPDIR)/target/linux/realtek/files-6.18/drivers/net/pcs
RTSDS_SRC:=$(TOPDIR)/target/linux/realtek/files-6.18/drivers/net/mdio

# Re-prepare whenever the copied-in realtek target sources or our patches change
PKG_FILE_DEPENDS:=$(RTL83XX_SRC)/ $(RTINC_SRC)/ $(RTSOC_SRC)/otto_table.c $(RTPCS_SRC)/pcs-rtl-otto.c $(RTSDS_SRC)/mdio-realtek-otto-serdes.c $(RTSDS_SRC)/mdio-realtek-otto.c ./patches/

include $(INCLUDE_DIR)/package.mk

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)/dsa
	# Copy all rtl83xx driver source files
	$(CP) $(RTL83XX_SRC)/*.c $(PKG_BUILD_DIR)/dsa/
	$(CP) $(RTL83XX_SRC)/*.h $(PKG_BUILD_DIR)/dsa/
	# upstream's rtl83xx now includes <linux/soc/realtek/otto_table.h>, which
	# lives in the realtek target's own include tree rather than the kernel's
	mkdir -p $(PKG_BUILD_DIR)/include/linux/soc/realtek
	$(CP) $(RTINC_SRC)/*.h $(PKG_BUILD_DIR)/include/linux/soc/realtek/
	# ...and its implementation, which upstream added as a realtek SoC driver
	$(CP) $(RTSOC_SRC)/otto_table.c $(PKG_BUILD_DIR)/
	# Copy the upstream PCS + SerDes MDIO bus and adapt them for SPI-slave
	# embedding on the 6.12 kernel
	$(CP) $(RTSDS_SRC)/mdio-realtek-otto-serdes.c $(PKG_BUILD_DIR)/
	# the switch's own port MDIO bus (realtek,otto-mdio); upstream split it
	# out of the switch driver into its own module
	$(CP) $(RTSDS_SRC)/mdio-realtek-otto.c $(PKG_BUILD_DIR)/
	$(CP) $(RTPCS_SRC)/pcs-rtl-otto.c $(PKG_BUILD_DIR)/
	patch -p1 -d $(PKG_BUILD_DIR) -i $(CURDIR)/patches/00-mdio-otto-serdes-embed.patch
	patch -p1 -d $(PKG_BUILD_DIR) -i $(CURDIR)/patches/01-pcs-rtl-otto-spi-adapter.patch
	patch -p1 -d $(PKG_BUILD_DIR) -i $(CURDIR)/patches/02-pcs-rtl-otto-hisgmii.patch
	patch -p1 -d $(PKG_BUILD_DIR) -i $(CURDIR)/patches/03-pcs-rtl-otto-link-up-self-heal.patch
	patch -p1 -d $(PKG_BUILD_DIR) -i $(CURDIR)/patches/15-otto-table-modular.patch
	# Overlay our files (stub header, regmap, spi driver, module Makefile)
	$(CP) -r ./src/. $(PKG_BUILD_DIR)/
	# Apply our deltas to the copied rtl83xx DSA driver
	patch -p1 -d $(PKG_BUILD_DIR)/dsa -i $(CURDIR)/patches/04-external-spi-port-bridge-setup.patch
	patch -p1 -d $(PKG_BUILD_DIR)/dsa -i $(CURDIR)/patches/05-switch-irq-optional-cpu-vlans.patch
	patch -p1 -d $(PKG_BUILD_DIR)/dsa -i $(CURDIR)/patches/06-cpu-port-pvid.patch
	patch -p1 -d $(PKG_BUILD_DIR)/dsa -i $(CURDIR)/patches/07-module-reload-eexist-fix.patch
	patch -p1 -d $(PKG_BUILD_DIR)/dsa -i $(CURDIR)/patches/08-vlan-stp-untagged-bridging.patch
	patch -p1 -d $(PKG_BUILD_DIR)/dsa -i $(CURDIR)/patches/09-cpu-port-wire-tag.patch
	patch -p1 -d $(PKG_BUILD_DIR)/dsa -i $(CURDIR)/patches/10-counters-spi-table-lock.patch
	patch -p1 -d $(PKG_BUILD_DIR)/dsa -i $(CURDIR)/patches/13-vlan-unaware-bridge-vid.patch
	patch -p1 -d $(PKG_BUILD_DIR)/dsa -i $(CURDIR)/patches/14-eee-working.patch
	patch -p1 -d $(PKG_BUILD_DIR) -i $(CURDIR)/patches/16-external-spi-skip-soc-dependency-checks.patch
	patch -p1 -d $(PKG_BUILD_DIR) -i $(CURDIR)/patches/17-otto-mdio-foreign-bus-phy.patch
endef

define KernelPackage/rtl930x-regmap
  SUBMENU:=Network Devices
  TITLE:=RTL930x regmap backend
  FILES:=$(PKG_BUILD_DIR)/rtl930x-regmap.ko
  AUTOLOAD:=$(call AutoProbe,rtl930x-regmap,1)
  DEPENDS:=@TARGET_qualcommax_ipq807x +kmod-regmap-core
endef

define KernelPackage/rtl930x-regmap/description
  Thin regmap backend for the Realtek RTL930x switch accessed as a remote
  SPI slave from an external SoC (e.g. IPQ8072 on Verizon CR1000A).
  Exports the regmap handle and the soc_info global used by the DSA driver.
endef

define KernelPackage/rtl930x-spi
  SUBMENU:=Network Devices
  TITLE:=RTL930x SPI slave access driver
  FILES:=$(PKG_BUILD_DIR)/rtl930x-spi.ko
  AUTOLOAD:=$(call AutoProbe,rtl930x-spi,1)
  DEPENDS:=@TARGET_qualcommax_ipq807x +kmod-rtl930x-regmap +kmod-spi-dev
endef

define KernelPackage/rtl930x-spi/description
  SPI slave driver for the Realtek RTL9301/RTL9302/RTL9303 switch.
  Binds to cisco,spi-petra or realtek,rtl9300-spi DT nodes, sets up the
  SPI regmap, and populates the child realtek,rtl9301-switch platform
  device for the DSA driver to probe.
endef

define KernelPackage/rtl930x-otto-table
  SUBMENU:=Network Devices
  TITLE:=Realtek Otto table accessor (external SPI mode)
  FILES:=$(PKG_BUILD_DIR)/rtl930x-otto-table.ko
  AUTOLOAD:=$(call AutoProbe,rtl930x-otto-table,1)
  DEPENDS:=@TARGET_qualcommax_ipq807x +kmod-rtl930x-spi
endef

define KernelPackage/rtl930x-otto-table/description
  Upstream's drivers/soc/realtek/otto_table.c, which the rtl83xx DSA driver
  now calls into.  Built as its own module because it carries a module_init.
endef

define KernelPackage/rtl930x-otto-mdio
  SUBMENU:=Network Devices
  TITLE:=Realtek Otto switch-port MDIO bus (external SPI mode)
  FILES:=$(PKG_BUILD_DIR)/rtl930x-otto-mdio.ko
  AUTOLOAD:=$(call AutoProbe,rtl930x-otto-mdio,1)
  DEPENDS:=@TARGET_qualcommax_ipq807x +kmod-rtl930x-spi +kmod-libphy
endef

define KernelPackage/rtl930x-otto-mdio/description
  The MDIO controller inside the RTL93xx that carries the user-port PHYs.
  Upstream split it out of the switch driver; without it the switch cannot
  connect to its port PHYs ("failed to connect to PHY: -ENODEV").
endef

define KernelPackage/rtl930x-dsa
  SUBMENU:=Network Devices
  TITLE:=RTL930x DSA switch driver (external SPI mode)
  FILES:=$(PKG_BUILD_DIR)/rtl930x-dsa.ko
  AUTOLOAD:=$(call AutoProbe,rtl930x-dsa,1)
  DEPENDS:=@TARGET_qualcommax_ipq807x +kmod-rtl930x-spi +kmod-rtl930x-otto-table +kmod-rtl930x-otto-mdio \
           +kmod-dsa +kmod-libphy +kmod-phy-realtek
endef

define KernelPackage/rtl930x-dsa/description
  The upstream Realtek RTL83xx DSA switch driver compiled for use with an
  RTL9301 connected as an external SPI slave.  Register accesses are
  redirected through the SPI regmap instead of on-chip MMIO.
  Supports RTL9301 with PSGMII uplink to IPQ8072 ESS and HSGMII MoCA port.
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,rtl930x-regmap))
$(eval $(call KernelPackage,rtl930x-spi))
$(eval $(call KernelPackage,rtl930x-otto-table))
$(eval $(call KernelPackage,rtl930x-otto-mdio))
$(eval $(call KernelPackage,rtl930x-dsa))

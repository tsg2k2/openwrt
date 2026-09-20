include $(TOPDIR)/rules.mk

PKG_NAME:=cr1000a-diag
PKG_RELEASE:=1
PKG_LICENSE:=Proprietary

include $(INCLUDE_DIR)/package.mk

define Package/cr1000a-diag
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=CR1000A hardware diagnostic tooling
  DEPENDS:=@TARGET_qualcommax +libubox +libubus
endef

define Package/cr1000a-diag/description
  Bench and diagnostic tools for the Verizon CR1000A:
  - /lib/rtl: the OEM RTL9303 userspace diag app (usrApp, driven over
    a FIFO - see init.sh) and a register dump helper. Not started at
    boot; usrApp conflicts with the rtl930x DSA driver and is for
    manual offline diagnosis only.
  - /lib/aqr: AQR113C firmware image v5.6.7 plus the loader used for
    firmware experiments (the PHYs normally boot from their own NVR).
  Drop this package from DEVICE_PACKAGES for a slim production image.
endef

define Build/Compile
endef

# The OEM usrApp blob links the unversioned library names; its init.sh
# materializes them at runtime from the versioned ones. Declare them so
# the packaging dependency scan accepts the prebuilt binary.
define Package/cr1000a-diag/extra_provides
	echo 'libubox.so'; echo 'libubus.so';
endef

define Package/cr1000a-diag/install
	$(INSTALL_DIR) $(1)/lib/rtl $(1)/lib/aqr
	$(INSTALL_BIN) ./files/rtl/usrApp $(1)/lib/rtl/
	$(INSTALL_BIN) ./files/rtl/init.sh $(1)/lib/rtl/
	$(INSTALL_BIN) ./files/rtl/dump-regs.sh $(1)/lib/rtl/
	$(INSTALL_BIN) ./files/aqr/aq-fw-download $(1)/lib/aqr/
	$(INSTALL_BIN) ./files/aqr/init.sh $(1)/lib/aqr/
	$(INSTALL_DATA) ./files/aqr/aqr-v5.6.7.cld $(1)/lib/aqr/
endef

$(eval $(call BuildPackage,cr1000a-diag))

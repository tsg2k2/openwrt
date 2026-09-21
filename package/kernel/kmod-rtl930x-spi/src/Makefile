# -I$(src) lets the compiler find our stub asm/mach-rtl-otto/mach-rtl-otto.h
# in place of the real Realtek MIPS header, and also finds rtl930x_regmap.h.
# -DCONFIG_NET_DSA_RTL83XX_EXTERNAL_SPI activates the #ifdef guard in common.c
# that skips the realtek,otto-mdio / realtek,otto-switch DT lookups.
# -I$(src)/include carries the realtek target's <linux/soc/realtek/*.h>,
# which upstream's rtl83xx driver now includes
ccflags-y += -I$(src) -I$(src)/include -DCONFIG_NET_DSA_RTL83XX_EXTERNAL_SPI

# Debug-only: a raw-SPI passthrough cdev (/dev/spidevX.Y) for poking the switch
# from userspace while this driver owns it.  Off by default; uncomment to enable.
#ccflags-y += -DRTL930X_SPI_SPIDEV

obj-m := rtl930x-regmap.o rtl930x-spi.o rtl930x-otto-table.o rtl930x-otto-mdio.o rtl930x-dsa.o

rtl930x-regmap-objs := rtl930x_regmap.o
rtl930x-spi-objs    := rtl930x_spi.o pcs-rtl-otto.o mdio-realtek-otto-serdes.o
rtl930x-otto-table-objs := otto_table.o
rtl930x-otto-mdio-objs  := mdio-realtek-otto.o

rtl930x-dsa-objs    := dsa/common.o   \
                        dsa/dsa.o      \
                        dsa/rtl838x.o  \
                        dsa/rtl839x.o  \
                        dsa/rtl930x.o  \
                        dsa/rtl931x.o  \
                        dsa/debugfs.o  \
                        dsa/qos.o      \
                        dsa/tc.o       \
                        dsa/l2.o       \
                        dsa/l3.o       \
                        dsa/lag.o      \
                        dsa/pie.o      \
                        dsa/stats.o    \
                        dsa/vlan.o

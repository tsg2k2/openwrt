# RTL930x Driver Regmap Conversion

## Overview

This document describes the conversion of the RTL930x Ethernet switch driver from direct register access to regmap-based register access. The regmap subsystem provides a standardized way to access hardware registers with built-in caching, debugging, and abstraction capabilities.

## Original Implementation Analysis

The original RTL930x driver (`rtl930x.c`) uses direct memory-mapped register access through these macros:

```c
#define sw_r32(reg)			readl(RTL838X_SW_BASE + reg)
#define sw_w32(val, reg)		writel(val, RTL838X_SW_BASE + reg)
#define sw_w32_mask(clear, set, reg)	sw_w32((sw_r32(reg) & ~(clear)) | (set), reg)
```

### Key Components Converted

1. **L2 Forwarding Table Management**
   - Hash-based L2 entry read/write operations
   - CAM (Content Addressable Memory) operations
   - Multicast portmask management

2. **VLAN Configuration**
   - VLAN table operations
   - Port-based VLAN settings
   - VLAN profile management

3. **Port Management**
   - Port mirroring configuration
   - Rate limiting/policing
   - Traffic control

4. **PIE (Packet Inspection Engine)**
   - Rule management
   - Template-based packet classification

5. **Statistics and Counters**
   - Packet counter read/clear operations
   - Port statistics management

6. **LED Control**
   - LED set configuration
   - Port LED assignment

7. **QoS (Quality of Service)**
   - DSCP to queue mapping
   - Priority queue configuration
   - Scheduling algorithms

## Regmap Conversion Details

### Register Access Abstraction

The conversion replaces direct register access with regmap API calls:

```c
// Original
u32 val = sw_r32(RTL930X_L2_CTRL);
sw_w32(val | BIT(0), RTL930X_L2_CTRL);
sw_w32_mask(0xff, new_val, RTL930X_SOME_REG);

// Regmap version
u32 val = rtl930x_r32(RTL930X_L2_CTRL);
rtl930x_w32(val | BIT(0), RTL930X_L2_CTRL);
rtl930x_w32_mask(0xff, new_val, RTL930X_SOME_REG);
```

### Regmap Configuration

```c
static const struct regmap_config rtl930x_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0xFFFF,
	.cache_type = REGCACHE_NONE, /* No caching for hardware registers */
	.use_single_read = true,
	.use_single_write = true,
};
```

### Key Features

1. **Error Handling**: All regmap operations include proper error checking
2. **Debugging**: Regmap provides built-in register access tracing
3. **Abstraction**: Hardware access is abstracted through the regmap layer
4. **Consistency**: Uniform API for all register operations

## Benefits of Regmap Conversion

### 1. Better Debugging
- Built-in register access tracing via debugfs
- Automatic register dump capabilities
- Access pattern analysis

### 2. Error Handling
- Proper error propagation from hardware access
- Bus error detection and handling
- Timeout handling for register operations

### 3. Abstraction
- Hardware access abstracted through regmap layer
- Easier to port to different bus types (SPI, I2C, etc.)
- Consistent API across different register types

### 4. Maintainability
- Cleaner code structure
- Standardized register access patterns
- Better separation of concerns

### 5. Performance Monitoring
- Register access statistics
- Performance profiling capabilities
- Access pattern optimization

## File Structure

```
target/linux/realtek/files-6.12/drivers/net/dsa/rtl83xx/
├── rtl930x.c                    # Original driver
├── rtl930x_regmap.c            # Regmap-based driver
├── rtl930x_regmap.h            # Regmap driver header
└── rtl930x_regmap_integration.patch  # Build system integration
```

## Integration

### Build System Changes

The regmap version can be enabled through Kconfig:

```kconfig
config NET_DSA_RTL83XX_RTL930X_REGMAP
	bool "Use regmap for RTL930x register access"
	depends on NET_DSA_RTL83XX
	select REGMAP_MMIO
	default n
```

### Makefile Changes

```makefile
obj-$(CONFIG_NET_DSA_RTL83XX) += rtl930x_regmap.o
```

## Usage

### Initialization

```c
int rtl930x_regmap_init(struct device *dev, void __iomem *base)
```

### Register Access

```c
// Read register
u32 val = rtl930x_r32(RTL930X_L2_CTRL);

// Write register
rtl930x_w32(0x12345678, RTL930X_L2_CTRL);

// Read-modify-write
rtl930x_w32_mask(0xff, new_val, RTL930X_SOME_REG);
```

### Cleanup

```c
void rtl930x_regmap_exit(void)
```

## Testing and Validation

### Functional Testing
1. Verify all L2 forwarding operations work correctly
2. Test VLAN configuration and traffic forwarding
3. Validate port mirroring functionality
4. Check PIE rule programming and packet classification
5. Test statistics collection and LED control

### Performance Testing
1. Compare register access performance vs. original driver
2. Measure memory usage impact
3. Validate interrupt handling latency

### Debugging Features
1. Enable regmap debugging via debugfs
2. Trace register access patterns
3. Verify error handling paths

## Migration Path

### Phase 1: Parallel Implementation
- Keep both original and regmap versions
- Allow selection via Kconfig option
- Extensive testing of regmap version

### Phase 2: Feature Parity
- Ensure all features work in regmap version
- Performance optimization if needed
- Documentation updates

### Phase 3: Migration
- Default to regmap version
- Deprecate original implementation
- Remove original code after validation period

## Debugging

### Enable Regmap Debugging

```bash
# Enable regmap debugging
echo 1 > /sys/kernel/debug/regmap/rtl930x/cache_only
echo 1 > /sys/kernel/debug/regmap/rtl930x/cache_bypass

# View register access log
cat /sys/kernel/debug/regmap/rtl930x/access
```

### Register Dumps

```bash
# Dump all registers
cat /sys/kernel/debug/regmap/rtl930x/registers

# Dump specific register ranges
cat /sys/kernel/debug/regmap/rtl930x/range
```

## Known Limitations

1. **Performance**: Slight overhead due to regmap abstraction layer
2. **Memory**: Additional memory usage for regmap structures
3. **Complexity**: More complex initialization sequence

## SPI Slave Access Support

### Overview

In addition to memory-mapped I/O, the RTL930x regmap driver now supports SPI slave access. This allows the switch to be controlled via SPI bus, which is useful for external switch configurations where the switch is connected to a host processor via SPI.

### SPI Implementation

#### Key Components

1. **SPI Driver** (`rtl930x_spi.c`):
   - SPI device driver with regmap backend
   - Hardware reset and CS control via GPIO
   - Configurable SPI speed and timing parameters
   - Built-in communication testing

2. **SPI Protocol**:
   - Read command: `[0x03][ADDR:4][DATA:4]`
   - Write command: `[0x02][ADDR:4][DATA:4]`
   - Maximum speed: 10 MHz
   - Big-endian byte order

3. **Device Tree Integration**:
   - Compatible strings: `realtek,rtl9300-spi`, `realtek,rtl9302-spi`, `realtek,rtl9303-spi`
   - GPIO control for reset and CS
   - Interrupt support
   - Configurable SPI parameters

#### Configuration

```kconfig
config NET_DSA_RTL83XX_RTL930X_SPI
	tristate "RTL930x SPI slave interface support"
	depends on NET_DSA_RTL83XX && SPI
	select NET_DSA_RTL83XX_RTL930X_REGMAP
	select REGMAP_SPI
```

#### Device Tree Example

```dts
&spi0 {
	switch@0 {
		compatible = "realtek,rtl9300-spi";
		reg = <0>;
		spi-max-frequency = <10000000>;
		
		reset-gpios = <&gpio 12 GPIO_ACTIVE_LOW>;
		cs-gpios = <&gpio 8 GPIO_ACTIVE_LOW>;
		
		interrupt-parent = <&gpio>;
		interrupts = <15 IRQ_TYPE_LEVEL_LOW>;
		
		ports {
			/* Port configuration */
		};
	};
};
```

#### Testing

The SPI interface includes comprehensive testing via `rtl930x_spi_test.c`:

```bash
# Run all tests
echo "run" > /sys/kernel/debug/rtl930x_spi_test/test

# Run specific tests
echo "read" > /sys/kernel/debug/rtl930x_spi_test/test
echo "write" > /sys/kernel/debug/rtl930x_spi_test/test
echo "verify" > /sys/kernel/debug/rtl930x_spi_test/test
echo "perf" > /sys/kernel/debug/rtl930x_spi_test/test
echo "stress" > /sys/kernel/debug/rtl930x_spi_test/test

# View test results
cat /sys/kernel/debug/rtl930x_spi_test/test
```

### Benefits of SPI Access

1. **External Switch Support**: Control switches connected via SPI bus
2. **Isolation**: Electrical isolation between host and switch
3. **Flexibility**: Support for various host processor architectures
4. **Debugging**: Better isolation for debugging register access issues
5. **Power Management**: Independent power domains for host and switch

### Performance Considerations

- **Latency**: Higher latency compared to memory-mapped access
- **Throughput**: Limited by SPI bus speed (max 10 MHz)
- **CPU Usage**: Higher CPU overhead for register access
- **Optimization**: Bulk operations and caching can improve performance

## Future Enhancements

1. **Caching**: Implement selective register caching for read-only registers
2. **Bulk Operations**: Use regmap bulk operations for table access
3. **Power Management**: Integrate with regmap PM features
4. **Multi-device**: Support for multiple RTL930x devices
5. **SPI Optimization**: Implement burst read/write modes for better performance
6. **I2C Support**: Add I2C slave interface support
7. **PCIe Support**: Add PCIe interface support for high-performance applications

## Conclusion

The regmap conversion with SPI slave access support provides a comprehensive, flexible, and maintainable implementation of the RTL930x driver. It supports both memory-mapped I/O and SPI access methods, enabling use in various hardware configurations while maintaining full functional compatibility. The abstraction layer enables better error handling, debugging capabilities, and future extensibility for additional interface types.
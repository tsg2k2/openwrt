// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL930x SPI interface test module
 * 
 * This module provides testing functionality for the RTL930x SPI interface,
 * including register access tests, performance benchmarks, and stress tests.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/spi/spi.h>
#include <linux/regmap.h>
#include <linux/delay.h>
#include <linux/random.h>
#include <linux/ktime.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>

#include "rtl930x_spi.h"
#include "rtl930x_regmap.h"

#define RTL930X_SPI_TEST_ITERATIONS	1000
#define RTL930X_SPI_STRESS_ITERATIONS	10000
#define RTL930X_SPI_PERF_ITERATIONS	5000

struct rtl930x_spi_test_stats {
	u64 read_tests;
	u64 write_tests;
	u64 read_errors;
	u64 write_errors;
	u64 total_time_ns;
	u32 min_time_ns;
	u32 max_time_ns;
	u32 avg_time_ns;
};

static struct rtl930x_spi_test_stats test_stats;
static struct dentry *debugfs_root;

/* Test register addresses (safe to read/write) */
static const u32 test_registers[] = {
	0x0000,  /* Model info */
	0x0004,  /* Chip info */
	0x000C,  /* Reset control */
	0x0010,  /* Clock control */
	0x0014,  /* Power control */
};

/* Test patterns for write/read verification */
static const u32 test_patterns[] = {
	0x00000000,
	0xFFFFFFFF,
	0x55555555,
	0xAAAAAAAA,
	0x12345678,
	0x87654321,
	0xDEADBEEF,
	0xCAFEBABE,
};

/* Basic register read test */
static int rtl930x_spi_test_read(void)
{
	struct regmap *regmap = rtl930x_spi_get_regmap();
	unsigned int val;
	ktime_t start, end;
	int ret, i;

	if (!regmap) {
		pr_err("RTL930x SPI regmap not available\n");
		return -ENODEV;
	}

	pr_info("Starting RTL930x SPI read test...\n");

	for (i = 0; i < ARRAY_SIZE(test_registers); i++) {
		start = ktime_get();
		ret = regmap_read(regmap, test_registers[i], &val);
		end = ktime_get();

		test_stats.read_tests++;
		
		if (ret) {
			test_stats.read_errors++;
			pr_err("Read test failed for reg 0x%x: %d\n", test_registers[i], ret);
			continue;
		}

		u64 time_ns = ktime_to_ns(ktime_sub(end, start));
		test_stats.total_time_ns += time_ns;
		
		if (time_ns < test_stats.min_time_ns || test_stats.min_time_ns == 0)
			test_stats.min_time_ns = time_ns;
		if (time_ns > test_stats.max_time_ns)
			test_stats.max_time_ns = time_ns;

		pr_debug("Read reg 0x%x = 0x%x (time: %llu ns)\n", 
			 test_registers[i], val, time_ns);
	}

	pr_info("RTL930x SPI read test completed\n");
	return 0;
}

/* Basic register write test */
static int rtl930x_spi_test_write(void)
{
	struct regmap *regmap = rtl930x_spi_get_regmap();
	ktime_t start, end;
	int ret, i, j;

	if (!regmap) {
		pr_err("RTL930x SPI regmap not available\n");
		return -ENODEV;
	}

	pr_info("Starting RTL930x SPI write test...\n");

	/* Test with safe registers that can be written */
	for (i = 0; i < ARRAY_SIZE(test_patterns); i++) {
		for (j = 2; j < ARRAY_SIZE(test_registers); j++) { /* Skip read-only regs */
			start = ktime_get();
			ret = regmap_write(regmap, test_registers[j], test_patterns[i]);
			end = ktime_get();

			test_stats.write_tests++;
			
			if (ret) {
				test_stats.write_errors++;
				pr_err("Write test failed for reg 0x%x: %d\n", 
				       test_registers[j], ret);
				continue;
			}

			u64 time_ns = ktime_to_ns(ktime_sub(end, start));
			test_stats.total_time_ns += time_ns;
			
			if (time_ns < test_stats.min_time_ns || test_stats.min_time_ns == 0)
				test_stats.min_time_ns = time_ns;
			if (time_ns > test_stats.max_time_ns)
				test_stats.max_time_ns = time_ns;

			pr_debug("Write reg 0x%x = 0x%x (time: %llu ns)\n", 
				 test_registers[j], test_patterns[i], time_ns);
		}
	}

	pr_info("RTL930x SPI write test completed\n");
	return 0;
}

/* Write/read verification test */
static int rtl930x_spi_test_write_read_verify(void)
{
	struct regmap *regmap = rtl930x_spi_get_regmap();
	unsigned int written_val, read_val;
	int ret, i, j, errors = 0;

	if (!regmap) {
		pr_err("RTL930x SPI regmap not available\n");
		return -ENODEV;
	}

	pr_info("Starting RTL930x SPI write/read verification test...\n");

	for (i = 0; i < ARRAY_SIZE(test_patterns); i++) {
		for (j = 2; j < ARRAY_SIZE(test_registers); j++) { /* Skip read-only regs */
			written_val = test_patterns[i];
			
			/* Write value */
			ret = regmap_write(regmap, test_registers[j], written_val);
			if (ret) {
				pr_err("Write failed for reg 0x%x: %d\n", test_registers[j], ret);
				errors++;
				continue;
			}

			/* Small delay to ensure write completion */
			udelay(10);

			/* Read back value */
			ret = regmap_read(regmap, test_registers[j], &read_val);
			if (ret) {
				pr_err("Read failed for reg 0x%x: %d\n", test_registers[j], ret);
				errors++;
				continue;
			}

			/* Verify values match */
			if (written_val != read_val) {
				pr_err("Verification failed for reg 0x%x: wrote 0x%x, read 0x%x\n",
				       test_registers[j], written_val, read_val);
				errors++;
			} else {
				pr_debug("Verification passed for reg 0x%x: 0x%x\n",
					 test_registers[j], written_val);
			}
		}
	}

	pr_info("RTL930x SPI write/read verification test completed (%d errors)\n", errors);
	return errors ? -EIO : 0;
}

/* Performance benchmark test */
static int rtl930x_spi_test_performance(void)
{
	struct regmap *regmap = rtl930x_spi_get_regmap();
	ktime_t start, end;
	u64 total_time_ns = 0;
	unsigned int val;
	int ret, i;

	if (!regmap) {
		pr_err("RTL930x SPI regmap not available\n");
		return -ENODEV;
	}

	pr_info("Starting RTL930x SPI performance test (%d iterations)...\n", 
		RTL930X_SPI_PERF_ITERATIONS);

	/* Read performance test */
	start = ktime_get();
	for (i = 0; i < RTL930X_SPI_PERF_ITERATIONS; i++) {
		ret = regmap_read(regmap, test_registers[0], &val);
		if (ret) {
			pr_err("Performance test read failed: %d\n", ret);
			return ret;
		}
	}
	end = ktime_get();
	
	total_time_ns = ktime_to_ns(ktime_sub(end, start));
	pr_info("Read performance: %llu ns total, %llu ns avg per read\n",
		total_time_ns, total_time_ns / RTL930X_SPI_PERF_ITERATIONS);

	/* Write performance test */
	start = ktime_get();
	for (i = 0; i < RTL930X_SPI_PERF_ITERATIONS; i++) {
		ret = regmap_write(regmap, test_registers[2], test_patterns[i % ARRAY_SIZE(test_patterns)]);
		if (ret) {
			pr_err("Performance test write failed: %d\n", ret);
			return ret;
		}
	}
	end = ktime_get();
	
	total_time_ns = ktime_to_ns(ktime_sub(end, start));
	pr_info("Write performance: %llu ns total, %llu ns avg per write\n",
		total_time_ns, total_time_ns / RTL930X_SPI_PERF_ITERATIONS);

	pr_info("RTL930x SPI performance test completed\n");
	return 0;
}

/* Stress test with random operations */
static int rtl930x_spi_test_stress(void)
{
	struct regmap *regmap = rtl930x_spi_get_regmap();
	unsigned int val;
	int ret, i, errors = 0;
	u32 reg_idx, pattern_idx;

	if (!regmap) {
		pr_err("RTL930x SPI regmap not available\n");
		return -ENODEV;
	}

	pr_info("Starting RTL930x SPI stress test (%d iterations)...\n", 
		RTL930X_SPI_STRESS_ITERATIONS);

	for (i = 0; i < RTL930X_SPI_STRESS_ITERATIONS; i++) {
		/* Random register selection */
		get_random_bytes(&reg_idx, sizeof(reg_idx));
		reg_idx %= ARRAY_SIZE(test_registers);

		/* Random pattern selection */
		get_random_bytes(&pattern_idx, sizeof(pattern_idx));
		pattern_idx %= ARRAY_SIZE(test_patterns);

		/* Random operation (read or write) */
		if (i % 2 == 0) {
			/* Read operation */
			ret = regmap_read(regmap, test_registers[reg_idx], &val);
			if (ret) {
				errors++;
				if (errors < 10) /* Limit error spam */
					pr_err("Stress test read failed (iter %d): %d\n", i, ret);
			}
		} else if (reg_idx >= 2) { /* Write only to safe registers */
			/* Write operation */
			ret = regmap_write(regmap, test_registers[reg_idx], test_patterns[pattern_idx]);
			if (ret) {
				errors++;
				if (errors < 10) /* Limit error spam */
					pr_err("Stress test write failed (iter %d): %d\n", i, ret);
			}
		}

		/* Progress indicator */
		if (i % 1000 == 0)
			pr_debug("Stress test progress: %d/%d\n", i, RTL930X_SPI_STRESS_ITERATIONS);
	}

	pr_info("RTL930x SPI stress test completed (%d errors)\n", errors);
	return errors ? -EIO : 0;
}

/* Run all tests */
static int rtl930x_spi_run_all_tests(void)
{
	int ret = 0;

	pr_info("Starting RTL930x SPI comprehensive test suite...\n");

	/* Reset statistics */
	memset(&test_stats, 0, sizeof(test_stats));

	/* Basic read test */
	ret = rtl930x_spi_test_read();
	if (ret)
		pr_err("Read test failed: %d\n", ret);

	/* Basic write test */
	ret = rtl930x_spi_test_write();
	if (ret)
		pr_err("Write test failed: %d\n", ret);

	/* Write/read verification test */
	ret = rtl930x_spi_test_write_read_verify();
	if (ret)
		pr_err("Write/read verification test failed: %d\n", ret);

	/* Performance test */
	ret = rtl930x_spi_test_performance();
	if (ret)
		pr_err("Performance test failed: %d\n", ret);

	/* Stress test */
	ret = rtl930x_spi_test_stress();
	if (ret)
		pr_err("Stress test failed: %d\n", ret);

	/* Calculate average time */
	if (test_stats.read_tests + test_stats.write_tests > 0) {
		test_stats.avg_time_ns = test_stats.total_time_ns / 
					 (test_stats.read_tests + test_stats.write_tests);
	}

	pr_info("RTL930x SPI test suite completed\n");
	pr_info("Statistics: Reads: %llu, Writes: %llu, Read errors: %llu, Write errors: %llu\n",
		test_stats.read_tests, test_stats.write_tests, 
		test_stats.read_errors, test_stats.write_errors);
	pr_info("Timing: Min: %u ns, Max: %u ns, Avg: %u ns\n",
		test_stats.min_time_ns, test_stats.max_time_ns, test_stats.avg_time_ns);

	return ret;
}

/* Debugfs interface */
static int rtl930x_spi_test_show(struct seq_file *s, void *unused)
{
	seq_printf(s, "RTL930x SPI Test Statistics:\n");
	seq_printf(s, "Read tests: %llu\n", test_stats.read_tests);
	seq_printf(s, "Write tests: %llu\n", test_stats.write_tests);
	seq_printf(s, "Read errors: %llu\n", test_stats.read_errors);
	seq_printf(s, "Write errors: %llu\n", test_stats.write_errors);
	seq_printf(s, "Total time: %llu ns\n", test_stats.total_time_ns);
	seq_printf(s, "Min time: %u ns\n", test_stats.min_time_ns);
	seq_printf(s, "Max time: %u ns\n", test_stats.max_time_ns);
	seq_printf(s, "Avg time: %u ns\n", test_stats.avg_time_ns);
	seq_printf(s, "\nSPI Available: %s\n", rtl930x_spi_is_available() ? "Yes" : "No");
	seq_printf(s, "Regmap Available: %s\n", rtl930x_spi_get_regmap() ? "Yes" : "No");
	
	return 0;
}

static int rtl930x_spi_test_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtl930x_spi_test_show, NULL);
}

static ssize_t rtl930x_spi_test_write(struct file *file, const char __user *buf,
				      size_t count, loff_t *ppos)
{
	char cmd[32];
	
	if (count >= sizeof(cmd))
		return -EINVAL;
		
	if (copy_from_user(cmd, buf, count))
		return -EFAULT;
		
	cmd[count] = '\0';
	
	if (strncmp(cmd, "run", 3) == 0) {
		rtl930x_spi_run_all_tests();
	} else if (strncmp(cmd, "read", 4) == 0) {
		rtl930x_spi_test_read();
	} else if (strncmp(cmd, "write", 5) == 0) {
		rtl930x_spi_test_write();
	} else if (strncmp(cmd, "verify", 6) == 0) {
		rtl930x_spi_test_write_read_verify();
	} else if (strncmp(cmd, "perf", 4) == 0) {
		rtl930x_spi_test_performance();
	} else if (strncmp(cmd, "stress", 6) == 0) {
		rtl930x_spi_test_stress();
	} else {
		pr_info("Available commands: run, read, write, verify, perf, stress\n");
	}
	
	return count;
}

static const struct file_operations rtl930x_spi_test_fops = {
	.open = rtl930x_spi_test_open,
	.read = seq_read,
	.write = rtl930x_spi_test_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init rtl930x_spi_test_init(void)
{
	pr_info("RTL930x SPI test module loaded\n");

	/* Create debugfs interface */
	debugfs_root = debugfs_create_dir("rtl930x_spi_test", NULL);
	if (debugfs_root) {
		debugfs_create_file("test", 0644, debugfs_root, NULL, &rtl930x_spi_test_fops);
	}

	/* Run initial test if SPI is available */
	if (rtl930x_spi_is_available()) {
		pr_info("RTL930x SPI interface detected, running basic tests...\n");
		rtl930x_spi_test_read();
	} else {
		pr_info("RTL930x SPI interface not available\n");
	}

	return 0;
}

static void __exit rtl930x_spi_test_exit(void)
{
	debugfs_remove_recursive(debugfs_root);
	pr_info("RTL930x SPI test module unloaded\n");
}

module_init(rtl930x_spi_test_init);
module_exit(rtl930x_spi_test_exit);

MODULE_DESCRIPTION("Realtek RTL930x SPI interface test module");
MODULE_AUTHOR("OpenWrt RTL930x Team");
MODULE_LICENSE("GPL v2");
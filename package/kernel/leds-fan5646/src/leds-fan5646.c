// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Fairchild FAN5646 1-wire ("tinywire") blinking-LED driver.
 *
 * Drives the Verizon CR1000A front-panel white LED. The IC is programmed over
 * a single bit-banged GPIO; it has a small register file (SLEW1/PULSE1/SLEW2/
 * PULSE2/CONTROL) and an autonomous pulse engine that can hold the LED on or
 * loop a breathing pattern without further CPU involvement.
 *
 * Originally derived from Motorola's Android FAN5646 driver; the 1-wire framing
 * and register programming were reworked to match this board's FAN5646.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/leds.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/of_platform.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/gpio.h>

#define FAN5646_SLEW1_REG   0
#define FAN5646_PULSE1_REG  0x1
#define FAN5646_SLEW2_REG   0x2
#define FAN5646_PULSE2_REG  0x3
#define FAN5646_CONTROL_REG 0x4

/* CONTROL[7:6] = ISET current step */
#define FAN5646_ISET_5mA  0x00
#define FAN5646_ISET_10mA 0x40
#define FAN5646_ISET_15mA 0x80
#define FAN5646_ISET_20mA 0xC0
#define FAN5646_ISET_SHIFT 6
#define FAN5646_ISET_STEPS 4		/* 5/10/15/20 mA */

#define FAN5646_FOLLOW 0x1
#define FAN5646_PLAY   0x2
#define FAN5646_SLOW   0x4

/*
 * Solid on is NOT achieved by holding the CTRL line; it comes from programming
 * PULSE1 with a full on-time. nOn=15/nOff=0 makes the IC drive the LED
 * continuously, and SLEW2=0x55 matches what the OEM loads alongside it.
 *
 * A pattern loops forever only when PLAY is set together with the FOLLOW bit
 * (PLAY on its own fires a single shot, which is why our blink used to flash
 * once and stop). The OEM also primes the pulse engine with a throwaway
 * 0x55/0x55 PLAY frame + ~1ms settle before loading the real pattern.
 */
#define FAN5646_PULSE_FULL_ON 0xF0	/* nOn=15, nOff=0: continuous on  */
#define FAN5646_PLAY_REPEAT   (FAN5646_PLAY | FAN5646_FOLLOW)
#define FAN5646_PRIME_DELAY   1000	/* us settle after the prime frame */

/*
 * 1-wire frame timing. The bus idles HIGH; every frame is 3 address bits + 8
 * data bits sent LSB-first, LOW-first, and is terminated by a STOP (LOW 12us)
 * + LATCH (HIGH 300us).
 *
 *   bit '1' = LOW for the long phase, then HIGH for the short phase
 *   bit '0' = LOW for the short phase, then HIGH for the long phase
 *
 * tsleep is the short phase; the long phase is 3 * tsleep (3us / 9us).
 */
#define FAN5646_STOP_LOW_US   12	/* frame stop: LOW pulse before latch */
#define FAN5646_LATCH_HIGH_US 300	/* frame latch: HIGH, returns to idle */

#define FAN5646_MAX_TON        1600
#define FAN5646_MAX_TOFF       4800
#define FAN5646_MAX_TRISE      1550
#define FAN5646_MAX_TFALL      1550
#define FAN5646_MAX_ON         4700	/* tRise + tFall + tOn */
#define FAN5646_MAX_ON_SLOW    6300	/* tRise + tFall + 2 * tOn */
#define FAN5646_MAX_OFF        FAN5646_MAX_TOFF
#define FAN5646_MAX_OFF_SLOW   9600

#define FAN5646_VREG_DELAY   600 /* Fixed regulator ramp up time */

#define DEFAULT_UP   45
#define DEFAULT_DOWN 45

/**
 * struct fan5646_data
 * @led:          LED class device for the white LED (brightness + the standard
 *                timer/pattern blink triggers).
 * @lock:         guards the bit-banged 1-wire transaction (held with IRQs off).
 * @mlock:        serialises regulator enable/disable refcounting.
 * @vreg:         supply gating the LED rail.
 * @regname:      regulator name parsed from the DT supply node.
 * @full_current: ceiling ISET register bits (0/0x40/0x80/0xC0); brightness is
 *                scaled onto the current steps up to this value.
 * @tsleep:       1-wire short-phase bit duration in us.
 * @com_gpio:     GPIO used for IC control.
 * @power:        regulator enable refcount flag.
 * @boot_on:      DT default-state = "on" requested.
 */
struct fan5646_data {
	struct led_classdev led;
	spinlock_t lock;
	struct mutex mlock;
	struct regulator *vreg;
	const char *regname;
	unsigned full_current;
	unsigned tsleep;
	int com_gpio;
	int power;
	bool boot_on;
};

static int fan5646_brightness_set(struct led_classdev *led_cdev,
				  enum led_brightness bvalue);
static void fan5646_set_pulse(unsigned msOn, unsigned msOff,
			      unsigned ramp_up, unsigned ramp_down, __u8 *slew,
			      __u8 *pulse);

static inline void tinywire_send_bit(int gpio, __u8 bit, unsigned tsleep)
{
	if (bit) {		/* '1' : LOW long, then HIGH short */
		gpio_set_value(gpio, 0);
		udelay(tsleep * 3);
		gpio_set_value(gpio, 1);
		udelay(tsleep);
	} else {		/* '0' : LOW short, then HIGH long */
		gpio_set_value(gpio, 0);
		udelay(tsleep);
		gpio_set_value(gpio, 1);
		udelay(tsleep * 3);
	}
}

/*
 * Bring the bus to its idle level (HIGH) before the first frame of a sequence.
 */
static inline void tinywire_send_reset(int gpio)
{
	gpio_set_value(gpio, 1);
	udelay(FAN5646_STOP_LOW_US);
}

/*
 * End of a programming sequence: drive the line LOW after the final CONTROL
 * frame (active presets). Holding it LOW is what latches the pattern. Only the
 * "off" path leaves it HIGH.
 */
static inline void tinywire_send_exec(int gpio)
{
	gpio_set_value(gpio, 0);
}

static inline void fan5646_power_on(struct fan5646_data *cb)
{
	int rc = 0;

	mutex_lock(&cb->mlock);
	if (!cb->power) {
		rc = regulator_enable(cb->vreg);
		if (rc) {
			pr_err("%s reg. enable error %d state %d\n",
				__func__, rc, regulator_is_enabled(cb->vreg));
			mutex_unlock(&cb->mlock);
			return;
		}
		udelay(FAN5646_VREG_DELAY);
		cb->power = 1;
	}
	mutex_unlock(&cb->mlock);
}

static inline void fan5646_power_off(struct fan5646_data *cb)
{
	int rc = 0;

	tinywire_send_reset(cb->com_gpio);
	mutex_lock(&cb->mlock);
	if (cb->power) {
		rc = regulator_disable(cb->vreg);
		if (rc) {
			pr_err("%s reg. disable error %d state %d\n",
				__func__, rc, regulator_is_enabled(cb->vreg));
			mutex_unlock(&cb->mlock);
			return;
		}
		cb->power = 0;
	}
	mutex_unlock(&cb->mlock);
}

static void tinywire_set_reg(int gpio, __u8 reg, __u8 value, unsigned tsleep)
{
	int i;
	__u8 mask = 0x1;

	pr_debug("%s: reg=0x%x, value=0x%0x, tsleep=%dus\n",
		 __func__, reg, value, tsleep);
	/* Register address is 3 bits.  Send it LSB first */
	for (i = 0; i < 3; i++)
		tinywire_send_bit(gpio, reg & (mask << i), tsleep);
	/* Now send data LSB first */
	for (i = 0; i < 8; i++)
		tinywire_send_bit(gpio, value & (mask << i), tsleep);
	/*
	 * Frame terminator: a STOP pulse (LOW 12us) followed by the LATCH
	 * (HIGH 300us), which also returns the bus to its idle HIGH level
	 * ready for the next frame. There is no extra stop *bit*.
	 */
	gpio_set_value(gpio, 0);
	udelay(FAN5646_STOP_LOW_US);
	gpio_set_value(gpio, 1);
	udelay(FAN5646_LATCH_HIGH_US);
}

/*
 * Prime the pulse engine before loading a repeating pattern: a throwaway
 * 0x55/0x55 frame in PLAY followed by a short settle. Without this the IC
 * plays the subsequent pattern only once.
 */
static void fan5646_prime(struct fan5646_data *pdata)
{
	tinywire_set_reg(pdata->com_gpio, FAN5646_SLEW1_REG, 0x55, pdata->tsleep);
	tinywire_set_reg(pdata->com_gpio, FAN5646_PULSE1_REG, 0x55, pdata->tsleep);
	tinywire_set_reg(pdata->com_gpio, FAN5646_SLEW2_REG, 0, pdata->tsleep);
	tinywire_set_reg(pdata->com_gpio, FAN5646_PULSE2_REG, 0, pdata->tsleep);
	tinywire_set_reg(pdata->com_gpio, FAN5646_CONTROL_REG, FAN5646_PLAY,
			 pdata->tsleep);
	udelay(FAN5646_PRIME_DELAY);
}

/*
 * Map an LED brightness onto a discrete ISET current step. full_current is the
 * configured ceiling (bits 6-7); brightness 1..max scales linearly across the
 * 5/10/15/20 mA steps up to that ceiling, and brightness 0 means off.
 */
static __u8 fan5646_iset_for_brightness(struct fan5646_data *pdata,
					enum led_brightness value)
{
	unsigned max_step = pdata->full_current >> FAN5646_ISET_SHIFT;
	unsigned max_b = pdata->led.max_brightness;
	unsigned step;

	if (!max_b || value >= max_b)
		return max_step << FAN5646_ISET_SHIFT;

	step = value * (max_step + 1) / max_b;
	if (step > max_step)
		step = max_step;
	return step << FAN5646_ISET_SHIFT;
}

static int
fan5646_brightness_set(struct led_classdev *led_cdev, enum led_brightness value)
{
	struct fan5646_data *pdata = dev_get_drvdata(led_cdev->dev->parent);
	unsigned long flags;
	__u8 ctrl_value;

	pr_debug("%s: %d\n", __func__, value);

	if (!value) {
		fan5646_power_off(pdata);
		return 0;
	}

	fan5646_power_on(pdata);

	/*
	 * Solid on: idle-high pre-frame, then no ramps (SLEW1=SLEW2=0) and BOTH
	 * pulse segments programmed full-on (PULSE1=PULSE2=nOn=15,nOff=0), with
	 * CONTROL=ISET|FOLLOW and the line driven LOW. Filling both segments is
	 * what makes it truly steady: with only PULSE1 set, the engine's empty
	 * second half-cycle produced a brief (~50 ms) dropout once per cycle. The
	 * ISET step is scaled from the requested brightness.
	 */
	ctrl_value = fan5646_iset_for_brightness(pdata, value) | FAN5646_FOLLOW;

	spin_lock_irqsave(&pdata->lock, flags);
	tinywire_send_reset(pdata->com_gpio);
	tinywire_set_reg(pdata->com_gpio, FAN5646_SLEW1_REG, 0, pdata->tsleep);
	tinywire_set_reg(pdata->com_gpio, FAN5646_PULSE1_REG,
			 FAN5646_PULSE_FULL_ON, pdata->tsleep);
	tinywire_set_reg(pdata->com_gpio, FAN5646_SLEW2_REG, 0, pdata->tsleep);
	tinywire_set_reg(pdata->com_gpio, FAN5646_PULSE2_REG,
			 FAN5646_PULSE_FULL_ON, pdata->tsleep);
	tinywire_set_reg(pdata->com_gpio, FAN5646_CONTROL_REG, ctrl_value,
			 pdata->tsleep);
	tinywire_send_exec(pdata->com_gpio);
	spin_unlock_irqrestore(&pdata->lock, flags);

	return 0;
}

static int fan5646_blink_set(struct led_classdev *led_cdev,
			     unsigned long *delay_on, unsigned long *delay_off)
{
	struct fan5646_data *pdata = dev_get_drvdata(led_cdev->dev->parent);
	__u8 ctrl_value = pdata->full_current | FAN5646_PLAY_REPEAT;
	__u8 slew, pulse;
	unsigned long flags;

	pr_debug("%s: delay_on = %lu, delay_off = %lu\n",
		 __func__, *delay_on, *delay_off);
	if (*delay_on == 0 && *delay_off == 0) {
		*delay_on = 500;
		*delay_off = 500;
	}
	fan5646_set_pulse(*delay_on, *delay_off, DEFAULT_UP, DEFAULT_DOWN,
			  &slew, &pulse);
	fan5646_power_on(pdata);
	spin_lock_irqsave(&pdata->lock, flags);
	tinywire_send_reset(pdata->com_gpio);
	fan5646_prime(pdata);
	tinywire_set_reg(pdata->com_gpio, FAN5646_SLEW1_REG, slew,
			   pdata->tsleep);
	tinywire_set_reg(pdata->com_gpio, FAN5646_PULSE1_REG, pulse,
			   pdata->tsleep);
	tinywire_set_reg(pdata->com_gpio, FAN5646_SLEW2_REG, 0, pdata->tsleep);
	tinywire_set_reg(pdata->com_gpio, FAN5646_PULSE2_REG, 0, pdata->tsleep);
	tinywire_set_reg(pdata->com_gpio, FAN5646_CONTROL_REG, ctrl_value,
			   pdata->tsleep);
	tinywire_send_exec(pdata->com_gpio);
	spin_unlock_irqrestore(&pdata->lock, flags);
	return 0;
}

static void
fan5646_set_pulse(unsigned msOn, unsigned msOff,
		  unsigned ramp_up, unsigned ramp_down, __u8 *slew,
		  __u8 *pulse)
{
	__u8 nRise, nFall, nOn, nOff;
	unsigned tRise, tFall, tOn, tOff;
	unsigned slow = 0;

	pr_debug("%s: msOn = %d, msOff = %d, ramp up = %d%%, down = %d%%\n",
		 __func__, msOn, msOff, ramp_up, ramp_down);
	*slew = 0;
	*pulse = 0;

	if (msOn == 0 && msOff == 0)
		return;
	/* We won't do slow for now */
	if (msOn > FAN5646_MAX_ON)
		msOn = FAN5646_MAX_ON;
	if (msOff > FAN5646_MAX_OFF)
		msOff = FAN5646_MAX_OFF;
	tOff = msOff;
	/* Now the blinking part
	 * msOn consists of 3 parts: tRise, tFall, and tOn.
	 */
	if (ramp_up + ramp_down > 100) {
		pr_err("%s: bad ramp up %d%%, ramp down %d%%; resetting\n",
		       __func__, ramp_up, ramp_down);
		ramp_up = DEFAULT_UP;
		ramp_down = DEFAULT_DOWN;
	}
	tOn = (100 - ramp_up - ramp_down) * msOn / 100;
	tRise = ramp_up * msOn / 100;
	tFall = ramp_down * msOn / 100;
	if (tRise > FAN5646_MAX_TRISE) {
		tOn += tRise - FAN5646_MAX_TRISE;
		tRise = FAN5646_MAX_TRISE;
	}
	if (tFall > FAN5646_MAX_TRISE) {
		tOn += tFall - FAN5646_MAX_TRISE;
		tFall = FAN5646_MAX_TRISE;
	}
	/* Now we need to calculate nRise, nFall, nOn and nOff
	   tRise = 31 * nRise * 3.33 ms, same for tFall
	   nRise = tRise / 103.23 */
	nRise = tRise * 100 / 10323;
	if (nRise > 0xF)
		nRise = 0xF;
	if (nRise == 0 && ramp_up != 0)
		nRise = 1;
	nFall = tFall * 100 / 10323;
	if (nFall > 0xF)
		nFall = 0xF;
	if (nFall == 0 && ramp_down != 0)
		nFall = 1;

	*slew = nRise << 4 | nFall;

	/* Now tOn and tOff
	 * tOn = (SLOW + 1) * nOn * 106.6
	 * tOff = (SLOW + 1) * nOff * 320
	 * nOn = tOn / ((SLOW + 1) * 106.6)
	 * nOff = tOff / ((SLOW + 1) * 320)
	 */
	nOn = tOn * 10 / ((slow + 1) * 1066);
	nOff = tOff / ((slow + 1) * 320);
	if (nOn > 0xF)
		nOn = 0xF;
	if (nOff > 0xF)
		nOff = 0xF;
	if (nOn == 0 && (ramp_up + ramp_down < 100))
		nOn = 1;
	if (nOff == 0 && msOff != 0)
		nOff = 1;
	*pulse = nOn << 4 | nOff;

	pr_debug("%s: tRise = %d, tFall = %d, tOn = %d, tOff = %d, slow = %d\n",
		 __func__, tRise, tFall, tOn, tOff, slow);
	pr_debug("%s: nRise = 0x%x, nFall = 0x%x, nOn = 0x%x, nOff = 0x%x\n",
		 __func__, nRise, nFall, nOn, nOff);
}

/*
 * Hardware pattern offload for the standard LED "pattern" trigger.
 *
 * The FAN5646 pulse engine runs one breathing cycle autonomously (rise -> on
 * -> fall -> off) and loops it forever in PLAY mode. We expose that through
 * the trigger's hw_pattern attribute as exactly four "brightness delta_t"
 * tuples (delta_t in ms), one per segment of the cycle:
 *
 *   hw_pattern = "<on> <t_rise>  <on> <t_on>  0 <t_fall>  0 <t_off>"
 *
 * which maps onto fan5646_set_pulse() as:
 *
 *   msOn      = t_rise + t_on + t_fall
 *   ramp_up   = t_rise * 100 / msOn   (percent of the on phase spent ramping up)
 *   ramp_down = t_fall * 100 / msOn
 *   msOff     = t_off
 *
 * Only indefinite repeat is supported (the IC has no finite repeat counter).
 * The trigger conveys "repeat forever" as repeat <= 0: -1 when the repeat
 * attribute is set explicitly, and 0 by default (pattern_trig_activate only
 * initialises last_repeat to -1, leaving data->repeat at its kzalloc 0). A
 * finite positive repeat count cannot be honoured, so reject only those.
 */
static int fan5646_pattern_set(struct led_classdev *led_cdev,
			       struct led_pattern *pattern, u32 len, int repeat)
{
	struct fan5646_data *pdata = dev_get_drvdata(led_cdev->dev->parent);
	unsigned int t_rise, t_on, t_fall, t_off, ms_on, ramp_up, ramp_down;
	__u8 ctrl_value = pdata->full_current | FAN5646_PLAY_REPEAT;
	__u8 slew, pulse;
	unsigned long flags;

	if (repeat > 0)
		return -EINVAL;		/* IC can only loop indefinitely */

	if (len != 4)
		return -EINVAL;

	/* tuples 0/1 are the "on" segments, 2/3 are the "off" segments */
	if (!pattern[0].brightness || !pattern[1].brightness ||
	    pattern[2].brightness || pattern[3].brightness)
		return -EINVAL;

	t_rise = pattern[0].delta_t;
	t_on   = pattern[1].delta_t;
	t_fall = pattern[2].delta_t;
	t_off  = pattern[3].delta_t;

	ms_on = t_rise + t_on + t_fall;
	if (ms_on == 0)
		return -EINVAL;

	ramp_up   = t_rise * 100 / ms_on;
	ramp_down = t_fall * 100 / ms_on;

	fan5646_set_pulse(ms_on, t_off, ramp_up, ramp_down, &slew, &pulse);

	fan5646_power_on(pdata);
	spin_lock_irqsave(&pdata->lock, flags);
	tinywire_send_reset(pdata->com_gpio);
	fan5646_prime(pdata);
	tinywire_set_reg(pdata->com_gpio, FAN5646_SLEW1_REG, slew, pdata->tsleep);
	tinywire_set_reg(pdata->com_gpio, FAN5646_PULSE1_REG, pulse, pdata->tsleep);
	tinywire_set_reg(pdata->com_gpio, FAN5646_SLEW2_REG, 0, pdata->tsleep);
	tinywire_set_reg(pdata->com_gpio, FAN5646_PULSE2_REG, 0, pdata->tsleep);
	tinywire_set_reg(pdata->com_gpio, FAN5646_CONTROL_REG, ctrl_value,
			 pdata->tsleep);
	tinywire_send_exec(pdata->com_gpio);
	spin_unlock_irqrestore(&pdata->lock, flags);

	return 0;
}

static int fan5646_pattern_clear(struct led_classdev *led_cdev)
{
	/* Stop the autonomous cycle and return to manual brightness. */
	return fan5646_brightness_set(led_cdev, led_cdev->brightness);
}

static int fan5646_of_init(struct device *dev)
{
	struct device_node *np, *sp;
	const char *state;
	int rc = 0;
	struct fan5646_data *pdata = dev_get_drvdata(dev);

	np = dev->of_node;
	if (!np)
		return -ENODEV;

	rc = of_property_read_string(np, "linux-name", &pdata->led.name);
	if (rc) {
		dev_err(dev, "Error reading name rc %d\n", rc);
		return rc;
	}

	rc = of_property_read_u32(np, "full-current",
				  &pdata->full_current);
	if (rc) {
		dev_err(dev, "Error reading current rc %d\n", rc);
		return rc;
	}

	rc = of_property_read_u32(np, "tsleep", &pdata->tsleep);
	if (rc) {
		dev_err(dev, "Error reading tsleep rc %d\n", rc);
		return rc;
	}

	pdata->com_gpio = of_get_named_gpio(np, "gpios", 0);
	if (pdata->com_gpio < 0) {
		dev_err(dev, "Error getting comm gpio\n");
		return -EINVAL;
	}

	/* Optional power-on behaviour: default trigger + default state. */
	of_property_read_string(np, "linux,default-trigger",
				&pdata->led.default_trigger);
	if (!of_property_read_string(np, "default-state", &state))
		pdata->boot_on = !strcmp(state, "on");

	/* Done reading fan5646 node. Find node describing our regulator */
	/* For now we need only regulator-name */

	sp = of_parse_phandle(np, "fan5646-supply", 0);
	if (!sp) {
		dev_err(dev, "Error getting vreg node\n");
		return -ENODEV;
	}

	rc = of_property_read_string(sp, "regulator-name", &pdata->regname);
	if (rc)
		dev_err(dev, "Error reading regulator name rc %d\n", rc);

	of_node_put(sp);
	return rc;
}

static int fan5646_probe(struct platform_device *pdev)
{
	int rc;
	struct fan5646_data *pdata;

	pdata = devm_kzalloc(&pdev->dev, sizeof(struct fan5646_data),
			     GFP_KERNEL);
	if (pdata == NULL)
		return -ENOMEM;

	dev_set_drvdata(&pdev->dev, pdata);

	rc = fan5646_of_init(&pdev->dev);
	if (rc)
		return rc;

	spin_lock_init(&pdata->lock);
	mutex_init(&pdata->mlock);

	rc = gpio_request(pdata->com_gpio, "leds-fan5646_com");
	if (rc) {
		dev_err(&pdev->dev, "gpio_request(%d, fan5646_ctrl) error %d\n",
			pdata->com_gpio, rc);
		return rc;
	}

	rc = gpio_direction_output(pdata->com_gpio, 1);
	if (rc) {
		dev_err(&pdev->dev, "gpio_direction_output(%d, 1) error %d\n",
			pdata->com_gpio, rc);
		goto free_gpios;
	}

	gpio_set_value(pdata->com_gpio, 0);

	pdata->vreg = regulator_get(&pdev->dev, pdata->regname);
	rc = PTR_ERR_OR_ZERO(pdata->vreg);
	if (rc) {
		dev_err(&pdev->dev, "regulator get for %s error %d\n",
			pdata->regname, rc);
		goto free_gpios;
	}

	pdata->led.max_brightness = LED_FULL;
	pdata->led.brightness_set_blocking = fan5646_brightness_set;
	pdata->led.blink_set = fan5646_blink_set;
	pdata->led.pattern_set = fan5646_pattern_set;
	pdata->led.pattern_clear = fan5646_pattern_clear;

	rc = led_classdev_register(&pdev->dev, &pdata->led);
	if (rc) {
		dev_err(&pdev->dev, "unable to register led %s: error %d\n",
			pdata->led.name, rc);
		goto put_regulator;
	}

	/* Apply DT default-state = "on" once the LED is live. */
	if (pdata->boot_on)
		fan5646_brightness_set(&pdata->led, pdata->led.max_brightness);

	return 0;

 put_regulator:
	regulator_put(pdata->vreg);
 free_gpios:
	gpio_free(pdata->com_gpio);
	mutex_destroy(&pdata->mlock);

	return rc;
}

static void fan5646_remove(struct platform_device *pdev)
{
	struct fan5646_data *pdata = dev_get_drvdata(&pdev->dev);

	/*
	 * Order matters. led_classdev_unregister() drives brightness to 0,
	 * which goes through fan5646_power_off() and does the (single,
	 * balanced) regulator disable + a final tinywire transaction on
	 * com_gpio -- so it must run while the GPIO and regulator are still
	 * valid. Do NOT call regulator_disable() here as well: that
	 * double-disables the supply ("unbalanced disables" WARN + -EIO).
	 * Release the regulator with regulator_put() (regulator_get() in probe
	 * is not managed), then free the GPIO last.
	 */
	led_classdev_unregister(&pdata->led);
	regulator_put(pdata->vreg);
	gpio_set_value(pdata->com_gpio, 0);
	gpio_free(pdata->com_gpio);
	mutex_destroy(&pdata->mlock);
}

static struct of_device_id fan5646_match_table[] = {
	{.compatible = "fsi,leds-fan5646"},
	{},
};
MODULE_DEVICE_TABLE(of, fan5646_match_table);

static struct platform_driver fan5646_driver = {
	.probe = fan5646_probe,
	.remove = fan5646_remove,
	.driver = {
		   .name = "leds-fan5646",
		   .owner = THIS_MODULE,
		   .of_match_table = fan5646_match_table},
};

module_platform_driver(fan5646_driver);

MODULE_DESCRIPTION("Fairchild FAN5646 LED Driver");
MODULE_LICENSE("GPL v2");

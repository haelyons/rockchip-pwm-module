// SPDX-License-Identifier: GPL-2.0-only
/*
 * PWM driver for Rockchip SoCs
 *
 * Copyright (C) 2014 Beniamino Galvani <b.galvani@gmail.com>
 * Copyright (C) 2014 ROCKCHIP, Inc.
 */

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/time.h>

#include <linux/device.h> 
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/delay.h>

#include "pwm-rockchip.h"

// -------- PWM ROCKCHIP --------
#define PWM_MAX_CHANNEL_NUM		4

#define PWM_CTRL_TIMER_EN		(1 << 0)
#define PWM_CTRL_OUTPUT_EN		(1 << 3)

#define PWM_ENABLE				(1 << 0)
#define PWM_CONTINUOUS			(1 << 1)
#define PWM_DUTY_POSITIVE		(1 << 3)
#define PWM_DUTY_NEGATIVE		(0 << 3)
#define PWM_INACTIVE_NEGATIVE	(0 << 4)
#define PWM_INACTIVE_POSITIVE	(1 << 4)
#define PWM_POLARITY_MASK		(PWM_DUTY_POSITIVE | PWM_INACTIVE_POSITIVE)
#define PWM_OUTPUT_LEFT			(0 << 5)
#define PWM_OUTPUT_CENTER		(1 << 5)
#define PWM_LOCK_EN				(1 << 6)
#define PWM_LP_DISABLE			(0 << 8)

#define PWM_ONESHOT_COUNT_SHIFT	24
#define PWM_ONESHOT_COUNT_MASK	(0xff << PWM_ONESHOT_COUNT_SHIFT)
#define PWM_ONESHOT_COUNT_MAX	256

#define PWM_REG_INTSTS(n)		((3 - (n)) * 0x10 + 0x10)
#define PWM_REG_INT_EN(n)		((3 - (n)) * 0x10 + 0x14)

#define PWM_CH_INT(n)			BIT(n)

// -------- SK6812 Spec. Values --------
#define LED_BITS				24
#define LEDS					57 // total 1368 bits per 57 LED strip
#define T0H                     400 // Duty cycle high / low for 0
#define T0L                     800  
#define T1H                     800 // Duty cycle high / low for 1
#define T1L                     400
#define FPWM                    (T0H + T0L) // PWM frequency (period)
#define RST                     50000 // min. reset value

struct rockchip_pwm_chip {
	struct pwm_chip chip;
	struct clk *clk;
	struct clk *pclk;
	struct pinctrl *pinctrl;
	struct pinctrl_state *active_state;
	const struct rockchip_pwm_data *data;
	void __iomem *base;
	unsigned long clk_rate;
	bool vop_pwm_en; /* indicate voppwm mirror register state */
	bool center_aligned;
	bool oneshot;
	int channel_id;
	int irq;
	//int hex_start;
	//int hex_end;
};

/* LEDSTRIP MODES 
0: static (uniform color, provided by hex, default 0xFF FF FF)
1: gradient (gradient for 57 LEDs (as macro) between provided colors)
2: rainbow (oof, we'll see, not for MVP...)
*/

struct rockchip_pwm_regs {
	unsigned long duty;
	unsigned long period;
	unsigned long cntr;
	unsigned long ctrl;
};

struct rockchip_pwm_data {
	struct rockchip_pwm_regs regs;
	unsigned int prescaler;
	bool supports_polarity;
	bool supports_lock;
	bool vop_pwm;
	u32 enable_conf;
	u32 enable_conf_mask;
};

static inline struct rockchip_pwm_chip *to_rockchip_pwm_chip(struct pwm_chip *c)
{
	return container_of(c, struct rockchip_pwm_chip, chip);
}

static void rockchip_pwm_get_state(struct pwm_chip *chip,
				   struct pwm_device *pwm,
				   struct pwm_state *state)
{
	struct rockchip_pwm_chip *pc = to_rockchip_pwm_chip(chip);
	u32 enable_conf = pc->data->enable_conf;
	u64 tmp;
	u32 val;
	int ret;

	ret = clk_enable(pc->pclk);
	if (ret)
		return;

	tmp = readl_relaxed(pc->base + pc->data->regs.period);
	tmp *= pc->data->prescaler * NSEC_PER_SEC;
	state->period = DIV_ROUND_CLOSEST_ULL(tmp, pc->clk_rate);

	tmp = readl_relaxed(pc->base + pc->data->regs.duty);
	tmp *= pc->data->prescaler * NSEC_PER_SEC;
	state->duty_cycle =  DIV_ROUND_CLOSEST_ULL(tmp, pc->clk_rate);

	//printk(KERN_INFO "[LIGHT] Getting state in driver, current mode: %llu", state->mode);

	val = readl_relaxed(pc->base + pc->data->regs.ctrl);
	state->enabled = (val & enable_conf) == enable_conf;

	if (pc->data->supports_polarity && !(val & PWM_DUTY_POSITIVE))
		state->polarity = PWM_POLARITY_INVERSED;
	else
		state->polarity = PWM_POLARITY_NORMAL;

	clk_disable(pc->pclk);
}

static irqreturn_t rockchip_pwm_oneshot_irq(int irq, void *data)
{
	struct rockchip_pwm_chip *pc = data;
	struct pwm_state state;
	unsigned int id = pc->channel_id;
	int val;

	if (id > 3)
		return IRQ_NONE;
	val = readl_relaxed(pc->base + PWM_REG_INTSTS(id));

	if ((val & PWM_CH_INT(id)) == 0)
		return IRQ_NONE;

	writel_relaxed(PWM_CH_INT(id), pc->base + PWM_REG_INTSTS(id));

	/*
	 * Set pwm state to disabled when the oneshot mode finished.
	 */
	pwm_get_state(&pc->chip.pwms[0], &state);
	state.enabled = false;
	pwm_apply_state(&pc->chip.pwms[0], &state);

	rockchip_pwm_oneshot_callback(&pc->chip.pwms[0], &state);

	return IRQ_HANDLED;
}

static void rockchip_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
			       const struct pwm_state *state)
{
	struct rockchip_pwm_chip *pc = to_rockchip_pwm_chip(chip);
	unsigned long period, duty;
	unsigned long flags;
	u64 div;
	u32 ctrl;

	//printk(KERN_INFO "[LIGHT] Configure PWM chip, period is %llu and duty cycle is %llu\n", state->period, state->duty_cycle); /* Note state struct is read-only */

	/*
	 * Since period and duty cycle registers have a width of 32
	 * bits, every possible input period can be obtained using the
	 * default prescaler value for all practical clock rate values.
	 */
	div = (u64)pc->clk_rate * state->period;
	period = DIV_ROUND_CLOSEST_ULL(div,
				       pc->data->prescaler * NSEC_PER_SEC);

	div = (u64)pc->clk_rate * state->duty_cycle;
	duty = DIV_ROUND_CLOSEST_ULL(div, pc->data->prescaler * NSEC_PER_SEC);

	local_irq_save(flags);
	/*
	 * Lock the period and duty of previous configuration, then
	 * change the duty and period, that would not be effective.
	 */
	ctrl = readl_relaxed(pc->base + pc->data->regs.ctrl);
	if (pc->data->vop_pwm) {
		if (pc->vop_pwm_en)
			ctrl |= PWM_ENABLE;
		else
			ctrl &= ~PWM_ENABLE;
	}

#ifdef CONFIG_PWM_ROCKCHIP_ONESHOT
	if (state->oneshot_count > PWM_ONESHOT_COUNT_MAX) {
		pc->oneshot = false;
		dev_err(chip->dev, "Oneshot_count value overflow.\n");
	} else if (state->oneshot_count > 0) {
		u32 int_ctrl;

		pc->oneshot = true;
		ctrl &= ~PWM_ONESHOT_COUNT_MASK;
		ctrl |= (state->oneshot_count - 1) << PWM_ONESHOT_COUNT_SHIFT;

		int_ctrl = readl_relaxed(pc->base + PWM_REG_INT_EN(pc->channel_id));
		int_ctrl |= PWM_CH_INT(pc->channel_id);
		writel_relaxed(int_ctrl, pc->base + PWM_REG_INT_EN(pc->channel_id));
	} else {
		u32 int_ctrl;

		pc->oneshot = false;
		ctrl |= PWM_CONTINUOUS;

		int_ctrl = readl_relaxed(pc->base + PWM_REG_INT_EN(pc->channel_id));
		int_ctrl &= ~PWM_CH_INT(pc->channel_id);
		writel_relaxed(int_ctrl, pc->base + PWM_REG_INT_EN(pc->channel_id));
	}
#endif

	if (pc->data->supports_lock) {
		ctrl |= PWM_LOCK_EN;
		writel_relaxed(ctrl, pc->base + pc->data->regs.ctrl);
	}

	writel(period, pc->base + pc->data->regs.period);
	writel(duty, pc->base + pc->data->regs.duty);

	if (pc->data->supports_polarity) {
		ctrl &= ~PWM_POLARITY_MASK;
		if (state->polarity == PWM_POLARITY_INVERSED)
			ctrl |= PWM_DUTY_NEGATIVE | PWM_INACTIVE_POSITIVE;
		else
			ctrl |= PWM_DUTY_POSITIVE | PWM_INACTIVE_NEGATIVE;
	}

	/*
	 * Unlock and set polarity at the same time,
	 * the configuration of duty, period and polarity
	 * would be effective together at next period.
	 */
	if (pc->data->supports_lock)
		ctrl &= ~PWM_LOCK_EN;

	writel(ctrl, pc->base + pc->data->regs.ctrl);
	local_irq_restore(flags);
}

/* BLK DIAG
	+-------------------------------------+
	| External Request (Change PWM Config)|
	+-------------------------------------+
						|
						V
	+----------------------------------+
	| rockchip_pwm_apply               |
	+----------------------------------+
	1. Enable/Disable PWM clock
	2. Retrieve Current State 
	3. Handle Polarity Changes
	4. **Call rockchip_pwm_config**
	5. Enable/Disable PWM Output
	6. Pin Control (If needed)
						| 
						V
	+----------------------------------+
	| rockchip_pwm_config              |
	+----------------------------------+
	1. Calculate Period/Duty Values
	2. Configure One-Shot (If needed)
	3. Manage Lock Mechanism (If needed)
	4. Write to Hardware Registers
						|
						V
	+----------------------------------+
	| PWM Hardware Registers           |
	+----------------------------------+
				(Period, Duty, Polarity, etc.)
*/

static int rockchip_pwm_enable(struct pwm_chip *chip,
			       struct pwm_device *pwm,
			       bool enable)
{
	struct rockchip_pwm_chip *pc = to_rockchip_pwm_chip(chip);
	u32 enable_conf = pc->data->enable_conf;
	int ret;
	u32 val;

	//printk(KERN_INFO "[LIGHT] Enable/Disable PWM\n");

	if (enable) 
	{
		ret = clk_enable(pc->clk);
		if (ret)
			return ret;
	}

	val = readl_relaxed(pc->base + pc->data->regs.ctrl);
	val &= ~pc->data->enable_conf_mask;

	if (PWM_OUTPUT_CENTER & pc->data->enable_conf_mask) 
	{
		if (pc->center_aligned)
			val |= PWM_OUTPUT_CENTER;
	}

	if (enable) {
		val |= enable_conf;
		if (pc->oneshot)
			val &= ~PWM_CONTINUOUS;
	} else {
		val &= ~enable_conf;
	}

	writel_relaxed(val, pc->base + pc->data->regs.ctrl);
	if (pc->data->vop_pwm)
		pc->vop_pwm_en = enable;

	if (!enable)
		clk_disable(pc->clk);

	return 0;
}

static int rockchip_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			      const struct pwm_state *state)
{
	struct rockchip_pwm_chip *pc;
	struct pwm_state curstate;
	struct pwm_state strip_state;

	ktime_t start_time, end_time;//, loop_time;
	ktime_t t1,t2,t3,t4,t5,t6,t7;

	unsigned long flags, d0, d1;
	void __iomem *ctrl_regs, *ctrl_regs_base, *duty_regs;
	bool enabled;

	int ret, bit_index;
	u16 i, k, l;
	u64 div;
	u32 ctrl, crtl_lock_enabled; //, mask;
	u32 time_to_tell_the_time, time_for_first_loop, time_to_run_delay_command, time_to_convert_time, sleep_time;

	const bool pb_green[24] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
	unsigned long pb_all[LEDS * LED_BITS];

	printk(KERN_INFO "[LIGHT] Entering main PWM apply function...");

	pc = to_rockchip_pwm_chip(chip);

	t1 = ktime_get();
	t2 = ktime_get();
	t3 = ktime_get();

	time_to_tell_the_time = ktime_to_ns(ktime_sub(t3, t2));
	printk(KERN_INFO "TIME (for ktime_get): %u",time_to_tell_the_time);

	ndelay( 10 );

	t4 = ktime_get();
	ndelay( 10 );
	ndelay( 10 );
	ndelay( 10 );
	ndelay( 10 );
	ndelay( 10 );
	ndelay( 10 );
	ndelay( 10 );
	ndelay( 10 );
	ndelay( 10 );
	ndelay( 10 );
		ndelay( 10 );
	ndelay( 10 );
	ndelay( 10 );
	ndelay( 10 );
	ndelay( 10 );
	ndelay( 10 );
	ndelay( 10 );
	ndelay( 10 );
	ndelay( 10 );
	ndelay( 10 );
	t5 = ktime_get();

	printk(KERN_INFO "TIME (for 20x ndelay): %llu", ktime_to_ns(ktime_sub(t5,t4)));

	t6 = ktime_get();
	time_to_run_delay_command = ( ktime_to_ns(ktime_sub(t5,t4)) - 200 - time_to_tell_the_time ) / 20;
	t7 = ktime_get();

	printk(KERN_INFO "TIME (for 1x ndelay): %u",time_to_run_delay_command);

	time_to_convert_time = ktime_to_ns(ktime_sub(t6,t7));
	time_for_first_loop = 1200 + time_to_tell_the_time + time_to_convert_time;
	printk(KERN_INFO "TIME (first loop, includes ktime and convert time): %u",time_for_first_loop);

	/* ENABLE PWM PERIPHERAL & APB CLOCKS*/
	ret = clk_enable(pc->pclk);
	if (ret) 
	{
		printk(KERN_INFO "[LIGHT] Failed to enable PWM APB clock");
		return ret;
	}

	ret = clk_enable(pc->clk);
	if (ret) 
	{
		printk(KERN_INFO "[LIGHT] Failed to enable PWM clock");
		return ret;
	}

	strip_state.enabled = true;
	strip_state.period = 1200;
	strip_state.duty_cycle = 0;

	pwm_get_state(pwm, &curstate);
	enabled = curstate.enabled;

	rockchip_pwm_config(chip, pwm, &strip_state);
	if (strip_state.enabled != enabled) {
		ret = rockchip_pwm_enable(chip, pwm, strip_state.enabled);
		if (ret)
			goto out;
	}

	if (strip_state.enabled)
		ret = pinctrl_select_state(pc->pinctrl, pc->active_state);

	div = (u64)pc->clk_rate * T0L;
	d0 = DIV_ROUND_CLOSEST_ULL(div, pc->data->prescaler * NSEC_PER_SEC);
	div = (u64)pc->clk_rate * T1L;
	d1 = DIV_ROUND_CLOSEST_ULL(div, pc->data->prescaler * NSEC_PER_SEC);
	ctrl = readl_relaxed(pc->base + pc->data->regs.ctrl); // read control register
	crtl_lock_enabled = ctrl | PWM_LOCK_EN;
	ctrl &= ~PWM_LOCK_EN;
	ctrl_regs_base = pc->base;
	ctrl_regs = ctrl_regs_base + pc->data->regs.ctrl;
	duty_regs = ctrl_regs_base + pc->data->regs.duty;

	//Construct master array from repeating bit array
    for (i = 0; i < LEDS * LED_BITS; i++) 
	{
        bit_index = i % 24;
        //mask = 1 << (23 - bit_index);
		pb_all[i] = pb_green[bit_index] ? d1 : d0;
		printk(KERN_INFO "%u", pb_all[i]);
    }

	local_irq_save(flags);
	start_time = ktime_get();

	for (k = 0; k < LEDS * LED_BITS; k++)
	{
		writel_relaxed(crtl_lock_enabled, ctrl_regs); // write ctrl register
		writel(pb_all[k], duty_regs); // write duty cycle value
		writel(ctrl, ctrl_regs); // write new lock enable value in ctrl register
			/*	
			if(k == 0) 
			{	// 			  1200				   start time - current time 						 delay time (~600ns)
				loop_time = ktime_get();
				sleep_time = time_for_first_loop - ktime_to_ns(ktime_sub(loop_time, start_time)) - time_to_run_delay_command;
			}
			if( sleep_time > 0 && sleep_time < 1800 ) 
			{
				if(k == 0) 
				{
					if( sleep_time > time_to_tell_the_time ) 
					{
						ndelay( sleep_time - time_to_tell_the_time );
					}
				}
				else  
				{
					ndelay( sleep_time );
				}
			}
			*/
	}

	udelay(15000);

	end_time = ktime_get();
	local_irq_restore(flags);

	strip_state.enabled = false;
	pwm_get_state(pwm, &curstate);
	enabled = curstate.enabled;
	
	rockchip_pwm_config(chip, pwm, &strip_state);
	if (strip_state.enabled != enabled) 
	{
		ret = rockchip_pwm_enable(chip, pwm, strip_state.enabled);
		if (ret)
			goto out;
	}

	/*
	writel_relaxed(crtl_lock_enabled, ctrl_regs); // write ctrl register
	writel(d0, duty_regs); // write duty cycle value
	writel(ctrl, ctrl_regs); // write new lock enable value in ctrl register

	if (strip_state.enabled)
		ret = pinctrl_select_state(pc->pinctrl, pc->active_state);
	*/

    printk(KERN_INFO "[LIGHT] Test completed in %lld ns\n", ktime_to_ns(ktime_sub(end_time, start_time)));
	printk(KERN_INFO "[LIGHT] Sleep time %u\n", sleep_time);
	/*
	clk_disable(pc->clk);
	clk_disable(pc->pclk);
	*/


out:
	clk_disable(pc->clk);
	clk_disable(pc->pclk);

	return ret;
}

static const struct pwm_ops rockchip_pwm_ops = {
	.get_state = rockchip_pwm_get_state,
	.apply = rockchip_pwm_apply,
	.owner = THIS_MODULE,
};

static const struct rockchip_pwm_data pwm_data_v1 = {
	.regs = {
		.duty = 0x04,
		.period = 0x08,
		.cntr = 0x00,
		.ctrl = 0x0c,
	},
	.prescaler = 2,
	.supports_polarity = false,
	.supports_lock = false,
	.vop_pwm = false,
	.enable_conf = PWM_CTRL_OUTPUT_EN | PWM_CTRL_TIMER_EN,
	.enable_conf_mask = BIT(1) | BIT(3),
};

static const struct rockchip_pwm_data pwm_data_v2 = {
	.regs = {
		.duty = 0x08,
		.period = 0x04,
		.cntr = 0x00,
		.ctrl = 0x0c,
	},
	.prescaler = 1,
	.supports_polarity = true,
	.supports_lock = false,
	.vop_pwm = false,
	.enable_conf = PWM_OUTPUT_LEFT | PWM_LP_DISABLE | PWM_ENABLE |
		       PWM_CONTINUOUS,
	.enable_conf_mask = GENMASK(2, 0) | BIT(5) | BIT(8),
};

static const struct rockchip_pwm_data pwm_data_vop = {
	.regs = {
		.duty = 0x08,
		.period = 0x04,
		.cntr = 0x0c,
		.ctrl = 0x00,
	},
	.prescaler = 1,
	.supports_polarity = true,
	.supports_lock = false,
	.vop_pwm = true,
	.enable_conf = PWM_OUTPUT_LEFT | PWM_LP_DISABLE | PWM_ENABLE |
		       PWM_CONTINUOUS,
	.enable_conf_mask = GENMASK(2, 0) | BIT(5) | BIT(8),
};

static const struct rockchip_pwm_data pwm_data_v3 = {
	.regs = {
		.duty = 0x08,
		.period = 0x04,
		.cntr = 0x00,
		.ctrl = 0x0c,
	},
	.prescaler = 1,
	.supports_polarity = true,
	.supports_lock = true,
	.vop_pwm = false,
	.enable_conf = PWM_OUTPUT_LEFT | PWM_LP_DISABLE | PWM_ENABLE |
		       PWM_CONTINUOUS,
	.enable_conf_mask = GENMASK(2, 0) | BIT(5) | BIT(8),
};

static const struct of_device_id rockchip_pwm_dt_ids[] = {
	{ .compatible = "rockchip,rk2928-pwm", .data = &pwm_data_v1},
	{ .compatible = "rockchip,rk3288-pwm", .data = &pwm_data_v2},
	{ .compatible = "rockchip,vop-pwm", .data = &pwm_data_vop},
	{ .compatible = "rockchip,rk3328-pwm", .data = &pwm_data_v3},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rockchip_pwm_dt_ids);

static int rockchip_pwm_get_channel_id(const char *name)
{
	int len = strlen(name);

	return name[len - 2] - '0';
}

/*
static void strip_test(struct rockchip_pwm_chip *pc)
{
    int ret, i, j;
	u32 val;
    unsigned long period = 1200;
    unsigned long duty_cycles[] = {300, 600}; 
	u32 enable_conf = pc->data->enable_conf;
    ktime_t start_time, end_time;
	bool enable = true;

	printk(KERN_INFO "[LIGHT] Preparing PWM clock\n");
    ret = clk_prepare_enable(pc->clk);
    if (ret) 
	{
        printk(KERN_INFO "Failed to enable PWM clock\n");
        return;
    }

	printk(KERN_INFO "[LIGHT] Preparing PWM APB clock\n");
    ret = clk_prepare_enable(pc->pclk);
    if (ret) 
	{
        printk(KERN_INFO "Failed to enable PWM APB clock\n");
        clk_disable_unprepare(pc->clk);
        return; 
    }

    // Enable PWM output (assuming PWM output control is available)
	printk(KERN_INFO "[LIGHT] Enabling PWM peripheral\n");

	val = readl_relaxed(pc->base + pc->data->regs.ctrl);
	val &= ~pc->data->enable_conf_mask;

	if (PWM_OUTPUT_CENTER & pc->data->enable_conf_mask) {
		if (pc->center_aligned)
			val |= PWM_OUTPUT_CENTER;
	}

	if (enable) {
		val |= enable_conf;
		if (pc->oneshot)
			val &= ~PWM_CONTINUOUS;
	} else {
		val &= ~enable_conf;
	}

	writel_relaxed(val, pc->base + pc->data->regs.ctrl);
	if (pc->data->vop_pwm)
		pc->vop_pwm_en = enable;

	if (!enable)
		clk_disable(pc->clk);

    printk(KERN_INFO "[LIGHT] Configuring base period 1200ns\n");
    writel_relaxed(period, pc->base + pc->data->regs.period);

    start_time = ktime_get();

    for (i = 0; i < 5; ++i) 
	{
        for (j = 0; j < 2; ++j) 
		{
            writel_relaxed(duty_cycles[j], pc->base + pc->data->regs.duty);
            udelay(1);
        }
    }

    end_time = ktime_get(); // Record end time

    // Disable PWM output (restore previous state)

    // Disable clocks
    clk_disable_unprepare(pc->pclk);
    clk_disable_unprepare(pc->clk);

    // Calculate and print the total time taken
    printk(KERN_INFO "[LIGHT] Test completed in %lld ns\n", ktime_to_ns(ktime_sub(end_time, start_time)));
}
*/

static int rockchip_pwm_probe(struct platform_device *pdev)
{
	const struct of_device_id *id;
	struct rockchip_pwm_chip *pc;
	struct resource *r;
	u32 enable_conf, ctrl;
	bool enabled;
	int ret, count;

	id = of_match_device(rockchip_pwm_dt_ids, &pdev->dev);
	if (!id)
		return -EINVAL;

	pc = devm_kzalloc(&pdev->dev, sizeof(*pc), GFP_KERNEL);
	if (!pc)
		return -ENOMEM;

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pc->base = devm_ioremap(&pdev->dev, r->start,
				resource_size(r));
	if (IS_ERR(pc->base))
		return PTR_ERR(pc->base);

	pc->clk = devm_clk_get(&pdev->dev, "pwm");
	if (IS_ERR(pc->clk)) {
		pc->clk = devm_clk_get(&pdev->dev, NULL);
		if (IS_ERR(pc->clk))
			return dev_err_probe(&pdev->dev, PTR_ERR(pc->clk),
					     "Can't get bus clk\n");
	}

	count = of_count_phandle_with_args(pdev->dev.of_node,
					   "clocks", "#clock-cells");
	if (count == 2)
		pc->pclk = devm_clk_get(&pdev->dev, "pclk");
	else
		pc->pclk = pc->clk;

	if (IS_ERR(pc->pclk)) {
		ret = PTR_ERR(pc->pclk);
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev, "Can't get APB clk: %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(pc->clk);
	if (ret) {
		dev_err(&pdev->dev, "Can't prepare enable bus clk: %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(pc->pclk);
	if (ret) {
		dev_err(&pdev->dev, "Can't prepare enable APB clk: %d\n", ret);
		goto err_clk;
	}

	pc->channel_id = rockchip_pwm_get_channel_id(pdev->dev.of_node->full_name);
	if (pc->channel_id < 0 || pc->channel_id >= PWM_MAX_CHANNEL_NUM) {
		dev_err(&pdev->dev, "Channel id is out of range: %d\n", pc->channel_id);
		ret = -EINVAL;
		goto err_pclk;
	}

	if (IS_ENABLED(CONFIG_PWM_ROCKCHIP_ONESHOT)) {
		pc->irq = platform_get_irq(pdev, 0);
		if (pc->irq < 0) {
			dev_err(&pdev->dev, "Get oneshot mode irq failed\n");
			ret = pc->irq;
			//goto err_pclk;
		}

		ret = devm_request_irq(&pdev->dev, pc->irq, rockchip_pwm_oneshot_irq,
				       IRQF_NO_SUSPEND | IRQF_SHARED,
				       "rk_pwm_oneshot_irq", pc);
		if (ret) {
			dev_err(&pdev->dev, "Claim oneshot IRQ failed\n");
			//printk(KERN_INFO "[LIGHT] Claim oneshot IRQ failed (thanks Rockchip)\n");
			//goto err_pclk;
		}
	}

	pc->pinctrl = devm_pinctrl_get(&pdev->dev);
	if (IS_ERR(pc->pinctrl)) {
		dev_err(&pdev->dev, "Get pinctrl failed!\n");
		ret = PTR_ERR(pc->pinctrl);
		goto err_pclk;
	}

	pc->active_state = pinctrl_lookup_state(pc->pinctrl, "active");
	if (IS_ERR(pc->active_state)) {
		dev_err(&pdev->dev, "No active pinctrl state\n");
		ret = PTR_ERR(pc->active_state);
		goto err_pclk;
	}

	platform_set_drvdata(pdev, pc);

	pc->data = id->data;
	pc->chip.dev = &pdev->dev;
	pc->chip.ops = &rockchip_pwm_ops;
	pc->chip.base = of_alias_get_id(pdev->dev.of_node, "pwm");
	pc->chip.npwm = 1;
	pc->clk_rate = clk_get_rate(pc->clk);

	if (pc->data->supports_polarity) {
		pc->chip.of_xlate = of_pwm_xlate_with_flags;
		pc->chip.of_pwm_n_cells = 3;
	}

	enable_conf = pc->data->enable_conf;
	ctrl = readl_relaxed(pc->base + pc->data->regs.ctrl);
	enabled = (ctrl & enable_conf) == enable_conf;

	pc->center_aligned =
		device_property_read_bool(&pdev->dev, "center-aligned");

	ret = pwmchip_add(&pc->chip);
	if (ret < 0) {
		dev_err(&pdev->dev, "pwmchip_add() failed: %d\n", ret);
		goto err_pclk;
	}

	/* Keep the PWM clk enabled if the PWM appears to be up and running. */
	if (!enabled)
		clk_disable(pc->clk);

	clk_disable(pc->pclk);

	//strip_test(pc);

	return 0;

err_pclk:
	clk_disable_unprepare(pc->pclk);
err_clk:
	clk_disable_unprepare(pc->clk);

	return ret;
}

static int rockchip_pwm_remove(struct platform_device *pdev)
{
	struct rockchip_pwm_chip *pc = platform_get_drvdata(pdev);

	clk_unprepare(pc->pclk);
	clk_unprepare(pc->clk);

	return pwmchip_remove(&pc->chip);
}

static struct platform_driver rockchip_pwm_driver = {
	.driver = {
		.name = "rockchip-pwm",
		.of_match_table = rockchip_pwm_dt_ids,
	},
	.probe = rockchip_pwm_probe,
	.remove = rockchip_pwm_remove,
};
#ifdef CONFIG_ROCKCHIP_THUNDER_BOOT
static int __init rockchip_pwm_driver_init(void)
{
	return platform_driver_register(&rockchip_pwm_driver);
}
subsys_initcall(rockchip_pwm_driver_init);

static void __exit rockchip_pwm_driver_exit(void)
{
	platform_driver_unregister(&rockchip_pwm_driver);
}
module_exit(rockchip_pwm_driver_exit);
#else
module_platform_driver(rockchip_pwm_driver);
#endif

MODULE_AUTHOR("Beniamino Galvani <b.galvani@gmail.com>, Helios Lyons <helios.lyons@disguise.one>");
MODULE_DESCRIPTION("Adapted Rockchip SoC PWM driver for SK6812 LEDSTRIP");
MODULE_LICENSE("GPL v2");

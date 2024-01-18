#include <linux/module.h>
#include <linux/types.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/pwm.h>
#include <linux/kernel.h>

#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,10,0)
#include <linux/mod_devicetable.h>
#endif

static struct pwm_device *pwm;
static const int pwm_id = 9; // PWM ID is 9
static const int duty_ns = 200; // Duty cycle in nanoseconds
static const int period_ns = 1000; // Period in nanoseconds

static int module_overlay_probe(struct platform_device *platdev)
{
    int ret;

    pr_info("[MODULE] Probing\n");

    pwm = pwm_request(pwm_id, "pwm");
    if (IS_ERR(pwm)) {
        pr_err("[MODULE] Failed to request PWM%d\n", pwm_id);
        return PTR_ERR(pwm);
    }

    pr_info("[MODULE] PWM requested\n");

    ret = pwm_config(pwm, duty_ns, period_ns);
    if (ret) {
        pr_err("[MODULE] Failed to configure PWM%d\n", pwm_id);
        pwm_free(pwm);
        return ret;
    }

    pr_info("[MODULE] PWM configured\n");

    pwm_enable(pwm);
    pr_info("[MODULE] PWM enabled\n");
    return 0;
}

static int module_overlay_remove(struct platform_device *platdev)
{
    pwm_disable(pwm);
    pr_info("[MODULE] PWM disabled\n");

    pwm_free(pwm);
    pr_info("[MODULE] PWM freed\n");
    return 0;
}

static const struct of_device_id module_overlay_of_match[] = {
	{ .compatible = "radxa,rock-3a", },
	{},
};
MODULE_DEVICE_TABLE(of, module_overlay_of_match);

static struct platform_driver module_overlay_driver = {
    .driver = {
        .name = "module-overlay",
        .owner = THIS_MODULE,
        .of_match_table = module_overlay_of_match,
    },
    .probe = module_overlay_probe,
    .remove = module_overlay_remove,
};

module_platform_driver(module_overlay_driver);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Flat Max");
MODULE_DESCRIPTION("buildroot.rockchip module overlay example");
MODULE_ALIAS("platform:module-overlay-rk3308");


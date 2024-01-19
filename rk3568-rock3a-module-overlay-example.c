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

    pr_info("[LEDSTRIP] Probing\n");

    pwm = pwm_request(pwm_id, "pwm9");
    if (IS_ERR(pwm)) {
        pr_err("[LEDSTRIP] Failed to request PWM%d\n", pwm_id);
        return PTR_ERR(pwm);
    }

    pr_info("[LEDSTRIP] PWM requested\n");

    ret = pwm_config(pwm, duty_ns, period_ns);
    if (ret) {
        pr_err("[LEDSTRIP] Failed to configure PWM%d\n", pwm_id);
        pwm_free(pwm);
        return ret;
    }

    pr_info("[LEDSTRIP] PWM configured\n");

    pwm_enable(pwm);
    pr_info("[LEDSTRIP] PWM enabled\n");
    return 0;
}

static int module_overlay_remove(struct platform_device *platdev)
{
    pwm_disable(pwm);
    pr_info("[LEDSTRIP] PWM disabled\n");

    pwm_free(pwm);
    pr_info("[LEDSTRIP] PWM freed\n");
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

static int __init module_overlay_init(void)
{
    pr_info("[LEDSTRIP] Module loaded\n");
    return platform_driver_register(&module_overlay_driver);
}

// Function called when the module is removed
static void __exit module_overlay_exit(void)
{
    pr_info("[LEDSTRIP] Module unloaded\n");
    platform_driver_unregister(&module_overlay_driver);
}

module_init(module_overlay_init);
module_exit(module_overlay_exit);

//module_platform_driver(module_overlay_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Max Flat / Helios Lyons");
MODULE_DESCRIPTION("buildroot.rockchip module overlay example");
MODULE_ALIAS("platform:module-overlay-rk3308");
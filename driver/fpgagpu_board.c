/*

deprecated as new setup uses real pci device instead of virtualized platform device

#include <linux/module.h>
#include <linux/platform_device.h>
#include "../include/kmd/fpgagpu_core.h"

static struct platform_device *fpgagpu_pdevs[MAX_fpgagpu_DEVICES];

static int __init fpgagpu_board_init(void)
{
    int i;
    int ret;
    pr_info("fpgagpu-Board: loading fake board with %d fpgagpus...\n", MAX_fpgagpu_DEVICES);

    for (i = 0; i < MAX_fpgagpu_DEVICES; i++) {
        fpgagpu_pdevs[i] = platform_device_register_simple("fpgagpu_device", i, NULL, 0);
        if (IS_ERR(fpgagpu_pdevs[i])) {
            pr_err("fpgagpu-Board: failed to register device %d\n", i);
            ret = PTR_ERR(fpgagpu_pdevs[i]);
            goto err_register;
        }
    }
    return 0;

err_register:
    while (--i >= 0) {
        platform_device_unregister(fpgagpu_pdevs[i]);
    }
    return ret;
}

static void __exit fpgagpu_board_exit(void)
{
    int i;
    pr_info("fpgagpu-Board: unloading fake board...\n");
    for (i = 0; i < MAX_fpgagpu_DEVICES; i++) {
        if (fpgagpu_pdevs[i]) {
            platform_device_unregister(fpgagpu_pdevs[i]);
        }
    }
}

module_init(fpgagpu_board_init);
module_exit(fpgagpu_board_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("fpgagpu Team");
MODULE_DESCRIPTION("Fake Motherboard for fpgagpu Devices");
*/
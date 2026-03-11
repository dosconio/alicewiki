#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ayano");
MODULE_DESCRIPTION("CialloModule");
MODULE_VERSION("0.1.0");

static int __init load_hook(void) {
    printk(KERN_INFO "Yano : Load Hook\n");
    return 0;
}

static void __exit unload_hook(void) {
    printk(KERN_INFO "Yano : Unload Hook\n");
}

module_init(load_hook);
module_exit(unload_hook);

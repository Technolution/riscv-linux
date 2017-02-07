#include <linux/interrupt.h>
#include <linux/ftrace.h>
#include <linux/seq_file.h>
#include <linux/types.h>
#include <linux/uaccess.h> 

#include <asm/ptrace.h>
#include <asm/sbi.h>
#include <asm/sbi-con.h>
#include <asm/smp.h>
#include <asm/config-string.h>

#include "flash.h"
struct t_flash_memory {
    volatile unsigned long * control;
    volatile unsigned long * data;
}; // TODO: Make linked list and store in private_data on open()
static struct t_flash_memory flash_memory;

int flash_open(struct inode *inode, struct file *filp) {
    return 0;
}

int flash_close(struct inode *inode, struct file *filp) {
    return 0;
}

static long flash_ioctl(struct file *p_file, unsigned int num, unsigned long param)
{
    struct flash_regmap * user_regmap;
    ssize_t address;
    unsigned long data;
    switch(num) {
    case CONTROL_READ:
        user_regmap = (struct flash_regmap *) param;
        get_user(address, &user_regmap->address);
        data = flash_memory.control[address/sizeof(data)];
        put_user(data, &user_regmap->data);
        return 0;
    case CONTROL_WRITE:
        user_regmap = (struct flash_regmap *) param;
        get_user(address, &user_regmap->address);
        get_user(data, &user_regmap->data);
        flash_memory.control[address/sizeof(data)] = data;
        return 0;
    case DATA_READ:
        user_regmap = (struct flash_regmap *) param;
        get_user(address, &user_regmap->address);
        data = flash_memory.data[address/sizeof(data)];
        put_user(data, &user_regmap->data);
        return 0;
    case DATA_WRITE:
        user_regmap = (struct flash_regmap *) param;
        get_user(address, &user_regmap->address);
        get_user(data, &user_regmap->data);
        flash_memory.data[address/sizeof(data)] = data;
        return 0;
    }

    return -EINVAL;
}

/** Register module file operation functions */
static struct file_operations flash_fops = {
  open:             flash_open,
  unlocked_ioctl:   flash_ioctl,
  release:          flash_close
};

static int flash_probe(struct platform_device *pdev)
{
    struct resource *res;
    void *base;
    
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    base = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(base)) {
        dev_err(&pdev->dev, "Could not find Flash memory space\n");
        return PTR_ERR(base);
    }
    flash_memory.control = base;
    
    printk(KERN_INFO "Flash control memory address: 0x%08X\n", base); 
    res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
    base = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(base)) {
        dev_err(&pdev->dev, "Could not find Flash memory space\n");
        return PTR_ERR(base);
    }
    printk(KERN_INFO "Flash data memory address: 0x%08X\n", base); 
    flash_memory.data = base;
    
    // Register module major
    return register_chrdev(FLASH_MAJOR, FLASH_NAME, &flash_fops);
}

static int flash_remove(struct platform_device *pdev)
{
    return 0;
}

static struct platform_driver flash_driver = {
    .probe      = flash_probe,
    .remove     = flash_remove,
    .driver     = {
        .name   = "flash",
    },
};

static int __init flash_init(void)
{
    platform_driver_register(&flash_driver);
    return 0;
}

arch_initcall(flash_init)

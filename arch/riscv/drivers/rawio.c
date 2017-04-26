/*
 * (C) COPYRIGHT 2017 TECHNOLUTION B.V., GOUDA NL
 *  =======          I                   ==          I    =
 *     I             I                    I          I
 * |    I   ===   === I ===  I ===   ===   I  I    I ====  I   ===  I ===
 * |    I  /   \ I    I/   I I/   I I   I  I  I    I  I    I  I   I I/   I
 * |    I  ===== I    I    I I    I I   I  I  I    I  I    I  I   I I    I
 * |    I  \     I    I    I I    I I   I  I  I   /I  \    I  I   I I    I
 * |    I   ===   === I    I I    I  ===  ===  === I   ==  I   ===  I    I
 * |                 +---------------------------------------------------+
 * +----+            |  +++++++++++++++++++++++++++++++++++++++++++++++++|
 *     |            |             ++++++++++++++++++++++++++++++++++++++|
 *     +------------+                          +++++++++++++++++++++++++|
 *                                                        ++++++++++++++|
 *                                                                 +++++|
 */
/**
 *    @author Sijmen Woutersen (sijmen.woutersen@technolution.nl)
 *
 *    RAW I/O driver providing direct register access to I/O through mmap
 */
#include <linux/interrupt.h>
#include <linux/ftrace.h>
#include <linux/seq_file.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>

#include <asm/ptrace.h>
#include <asm/sbi.h>
#include <asm/sbi-con.h>
#include <asm/smp.h>
#include <asm/config-string.h>

#include "rawio.h"

struct rawio_t;

/// device info (linked list item)
struct rawio_t {
    int index;
    struct platform_device *p_device;
    struct rawio_t *p_next;
};

static struct rawio_t *p_rawio_list = NULL; ///< head of linked-list containing all devices
static int index = 0;                       ///< next free index

static int rawio_open(struct inode *p_inode, struct file *p_file)
{
    int index = MINOR(p_inode->i_rdev);
    struct rawio_t *p_rawio = p_rawio_list;

    // find device
    while (p_rawio && p_rawio->index != index) p_rawio = p_rawio->p_next;
    if (p_rawio == NULL) return -ENODEV;

    // link file to device
    p_file->private_data = p_rawio;

    return 0;
}

static int rawio_mmap(struct file *p_file, struct vm_area_struct *p_vma)
{
    struct rawio_t *p_rawio = p_file->private_data;
    struct resource *p_resource = platform_get_resource(p_rawio->p_device, IORESOURCE_MEM, 0);

    if (p_resource == NULL) return -ENODEV;
    if (p_vma->vm_pgoff != 0) return -EINVAL;
    if (((p_vma->vm_end - p_vma->vm_start) & ~PAGE_MASK) != 0) return -EINVAL;
    if ((p_vma->vm_end - p_vma->vm_start) > p_resource->end - p_resource->start) return -EINVAL;

    p_vma->vm_page_prot = pgprot_noncached(p_vma->vm_page_prot);
    p_vma->vm_flags |= VM_IO;

    if (io_remap_pfn_range(p_vma, p_vma->vm_start, p_resource->start >> PAGE_SHIFT, p_vma->vm_end - p_vma->vm_start,
            p_vma->vm_page_prot)) {
        printk(KERN_WARNING "remap_pfn_range failed\n");
        return -EAGAIN;
    }

    return 0;
}

static int rawio_close(struct inode *inode, struct file *p_file)
{
    return 0;
}

static struct file_operations rawio_fops = {
    open:             rawio_open,
    mmap:             rawio_mmap,
    release:          rawio_close
};

static int rawio_probe(struct platform_device *p_device)
{
    struct rawio_t *p_rawio;
    struct resource *p_resource = platform_get_resource(p_device, IORESOURCE_MEM, 0);

    if (!p_resource) return -ENODEV;

    // alloc memory
    p_rawio = kcalloc(1, sizeof(struct rawio_t), GFP_KERNEL);
    if (p_rawio == NULL) {
        return -ENOMEM;
    }

    printk(KERN_INFO "Registering RAW I/O device: %d @ %p-%p\n", index,
           (void*)p_resource->start, (void*)p_resource->end);
    p_rawio->index = index++;
    p_rawio->p_device = p_device;

    // add to list
    p_rawio->p_next = p_rawio_list;
    p_rawio_list = p_rawio;

    return 0;
}

static int rawio_remove(struct platform_device *p_device)
{
    struct rawio_t *p_next;
    struct rawio_t **pp_rawio_list = &p_rawio_list;

    while(*pp_rawio_list) {
        if ((*pp_rawio_list)->p_device == p_device) {
            p_next = (*pp_rawio_list)->p_next;
            printk(KERN_INFO "Removing Raw I/O #%d\n", (*pp_rawio_list)->index);

            kfree(*pp_rawio_list);
            *pp_rawio_list = p_next;
        } else {
            pp_rawio_list = &((*pp_rawio_list)->p_next);
        }
    }

    return 0;
}

static struct platform_driver rawio_driver = {
    .probe      = rawio_probe,
    .remove     = rawio_remove,
    .driver     = {
        .name   = "rawio",
    },
};

static int __init rawio_init(void)
{
    int ret;

    if ((ret = platform_driver_register(&rawio_driver)) != 0) {
        printk(KERN_ERR "Cannot register raw i/o driver.\n");
        return ret;
    }

    if ((ret = register_chrdev(RAWIO_MAJOR, RAWIO_NAME, &rawio_fops)) != 0) {
        printk(KERN_ERR "Cannot register raw i/o char device.\n");
        return ret;
    }

    return 0;
}

arch_initcall(rawio_init);

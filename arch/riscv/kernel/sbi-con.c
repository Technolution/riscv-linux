#include <linux/init.h>
#include <linux/console.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/tty_driver.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>

#include <asm/sbi.h>

struct riscv_uart {
    struct tty_driver* sbi_tty_driver;
    struct tty_port sbi_tty_port;
    struct console sbi_console;
    spinlock_t sbi_tty_port_lock;
    uint32_t __iomem *reg;
    uint32_t irq;
};

/**
 * sbi_console_isr - Interrupt Service Handler
 * @irq: IRQ number
 * @data: Uart port information
 *
 * Return: IRQ_HANDLED on success and IRQ_NONE on failure
 */
static irqreturn_t sbi_console_isr(int irq, void *data)
{
    struct riscv_uart *ru = (struct riscv_uart *)data;
	int ch = sbi_console_getchar();
	if (ch < 0)
		return IRQ_NONE;

	spin_lock(&ru->sbi_tty_port_lock);
	tty_insert_flip_char(&ru->sbi_tty_port, ch, TTY_NORMAL);
	tty_flip_buffer_push(&ru->sbi_tty_port);
	spin_unlock(&ru->sbi_tty_port_lock);

	return IRQ_HANDLED;
}

static int sbi_tty_open(struct tty_struct *tty, struct file *filp)
{
	return 0;
}

static int sbi_tty_write(struct tty_struct *tty,
	const unsigned char *buf, int count)
{
	const unsigned char *end;

	for (end = buf + count; buf < end; buf++) {
		sbi_console_putchar(*buf);
	}
	return count;
}

static int sbi_tty_write_room(struct tty_struct *tty)
{
	return 1024; /* arbitrary */
}

static const struct tty_operations sbi_tty_ops = {
	.open		= sbi_tty_open,
	.write		= sbi_tty_write,
	.write_room	= sbi_tty_write_room,
};


static void sbi_console_write(struct console *co, const char *buf, unsigned n)
{
	for ( ; n > 0; n--, buf++) {
		if (*buf == '\n')
			sbi_console_putchar('\r');
		sbi_console_putchar(*buf);
	}
}

static struct tty_driver *sbi_console_device(struct console *co, int *index)
{
    struct riscv_uart *ru = (struct riscv_uart *)co->data;
	*index = co->index;
	return ru->sbi_tty_driver;
}

static int sbi_console_setup(struct console *co, char *options)
{
	return co->index != 0 ? -ENODEV : 0;
}

static int sbi_console_init(struct riscv_uart *ru)
{
	int ret;

	strcpy(ru->sbi_console.name, "sbi_console");
	ru->sbi_console.write = sbi_console_write;
	ru->sbi_console.device = sbi_console_device;
	ru->sbi_console.setup = sbi_console_setup;
	ru->sbi_console.flags = CON_PRINTBUFFER;
	ru->sbi_console.index = -1;
	ru->sbi_console.data = (void*)ru;
	register_console(&ru->sbi_console);

	ru->sbi_tty_driver = tty_alloc_driver(1,
		TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV);
	if (unlikely(IS_ERR(ru->sbi_tty_driver)))
		return PTR_ERR(ru->sbi_tty_driver);

	ru->sbi_tty_driver->driver_name = "sbi";
	ru->sbi_tty_driver->name = "ttySBI";
	ru->sbi_tty_driver->major = TTY_MAJOR;
	ru->sbi_tty_driver->minor_start = 0;
	ru->sbi_tty_driver->type = TTY_DRIVER_TYPE_SERIAL;
	ru->sbi_tty_driver->subtype = SERIAL_TYPE_NORMAL;
	ru->sbi_tty_driver->init_termios = tty_std_termios;
	tty_set_operations(ru->sbi_tty_driver, &sbi_tty_ops);

	tty_port_init(&ru->sbi_tty_port);
	tty_port_link_device(&ru->sbi_tty_port, ru->sbi_tty_driver, 0);

	ret = tty_register_driver(ru->sbi_tty_driver);
	if (unlikely(ret))
		goto out_tty_put;

	return ret;

out_tty_put:
	put_tty_driver(ru->sbi_tty_driver);
	return ret;
}

static void sbi_console_exit(struct riscv_uart *ru)
{
	tty_unregister_driver(ru->sbi_tty_driver);
	put_tty_driver(ru->sbi_tty_driver);
}

static int sbi_probe(struct platform_device *pdev) {
    struct riscv_uart *ru;
    struct resource *res;
    void *base;
    uint32_t irq;
    int ret;
    int err;

    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    base = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(base)) {
        dev_err(&pdev->dev, "Could not find uart memory space\n");
        return PTR_ERR(base);
    }

    ru = devm_kzalloc(&pdev->dev, sizeof(*ru), GFP_KERNEL);
    ru->sbi_tty_port_lock = __SPIN_LOCK_UNLOCKED(ru->sbi_tty_port_lock);
    if (!ru)
        return -ENOMEM;

    res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
    if (res == NULL) {
        dev_err(&pdev->dev, "Could not find uart irq\n");
        return PTR_ERR(base);
    }
    irq = res->start;
    ru->reg = base;

    platform_set_drvdata(pdev, ru);
    ret = sbi_console_init(ru);

    if (ret == 0) {
        dev_info(&pdev->dev, "loaded SBI uart\n");
    } else {
        dev_warn(&pdev->dev, "failed to add SBI uart (%d)\n", ret);
    }

    // only when completely initialized, request the IRQ
    err = devm_request_irq(&pdev->dev, irq, sbi_console_isr,
                               IRQF_NO_THREAD,
                               "sbi_console", ru);
    if (err) {
        dev_err(&pdev->dev, "Unable to request irq %d\n", irq);
        return err;
    }

    return ret;
}

static int sbi_remove(struct platform_device *pdev) {
    struct riscv_uart *ru;

    ru = platform_get_drvdata(pdev);
    sbi_console_exit(ru);
    // free not needed, handled by the devm framework

    return 0;
}

static struct platform_driver sbi_driver = {
    .probe      = sbi_probe,
    .remove     = sbi_remove,
    .driver     = {
        .name   = "sbi",
    },
};

module_platform_driver(sbi_driver);

MODULE_DESCRIPTION("RISC-V SBI console driver");
MODULE_LICENSE("GPL");

#ifdef CONFIG_EARLY_PRINTK

static struct console early_console_dev __initdata = {
	.name	= "early",
	.write	= sbi_console_write,
	.flags	= CON_PRINTBUFFER | CON_BOOT,
	.index	= -1
};

static int __init setup_early_printk(char *str)
{
	if (early_console == NULL) {
		early_console = &early_console_dev;
		register_console(early_console);
	}
	return 0;
}

early_param("earlyprintk", setup_early_printk);

#endif

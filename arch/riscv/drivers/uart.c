/*
 * (C) COPYRIGHT 2016 TECHNOLUTION B.V., GOUDA NL
 *   =======          I                   ==          I    =
 *      I             I                    I          I
 * |    I   ===   === I ===  I ===   ===   I  I    I ====  I   ===  I ===
 * |    I  /   \ I    I/   I I/   I I   I  I  I    I  I    I  I   I I/   I
 * |    I  ===== I    I    I I    I I   I  I  I    I  I    I  I   I I    I
 * |    I  \     I    I    I I    I I   I  I  I   /I  \    I  I   I I    I
 * |    I   ===   === I    I I    I  ===  ===  === I   ==  I   ===  I    I
 * |                 +---------------------------------------------------+
 * +----+            |  +++++++++++++++++++++++++++++++++++++++++++++++++|
 *      |            |             ++++++++++++++++++++++++++++++++++++++|
 *      +------------+                          +++++++++++++++++++++++++|
 *                                                         ++++++++++++++|
 *                                                                  +++++|
 */

/**
  * @file
  * @author     sjors.hettinga <sjors.hettinga@technolution.eu>
  * @brief      Uart driver, decoupled form the SBI
  */


#include <linux/init.h>
#include <linux/console.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/tty_driver.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>

struct riscv_uart {
    struct tty_driver* rv_uart_tty_driver;
    struct tty_port rv_uart_tty_port;
    struct console rv_uart_console;
    spinlock_t rv_uart_tty_port_lock;
    volatile uint32_t __iomem *reg;
    uint32_t irq;
};

/**
 * rv_uart_console_isr - Interrupt Service Handler
 * @irq: IRQ number
 * @data: Uart port information
 *
 * Return: IRQ_HANDLED on success and IRQ_NONE on failure
 */
static irqreturn_t rv_uart_console_isr(int irq, void *data)
{
    struct riscv_uart *ru = (struct riscv_uart *)data;
    uint8_t ch;
    int bytes_available = ru->reg[10];
    if (bytes_available == 0) { // get the number of bytes
        return IRQ_NONE;
    } else {
        ch = ru->reg[8]; // get the character

        spin_lock(&ru->rv_uart_tty_port_lock);
        tty_insert_flip_char(&ru->rv_uart_tty_port, ch, TTY_NORMAL);
        tty_flip_buffer_push(&ru->rv_uart_tty_port);
        spin_unlock(&ru->rv_uart_tty_port_lock);
    }

    return IRQ_HANDLED;
}

static int rv_uart_tty_open(struct tty_struct *tty, struct file *filp)
{
    return 0;
}

static int rv_uart_put_string( volatile uint32_t __iomem *reg, const unsigned char* buf, int count ) {
    const unsigned char *end;
    int hw_buffer_space = 0;

    for (end = buf + count; buf < end; buf++) {
        while (hw_buffer_space < 1) hw_buffer_space = reg[7];
        reg[4] = *buf;
        hw_buffer_space--;
    }

    return count;
}

static int rv_uart_tty_write(struct tty_struct *tty,
    const unsigned char *buf, int count)
{
    struct riscv_uart *ru = (struct riscv_uart *)tty->driver->driver_state;
    return rv_uart_put_string(ru->reg, buf, count);
}

static int rv_uart_tty_write_room(struct tty_struct *tty)
{
    return 1024; /* arbitrary */
}

static const struct tty_operations rv_uart_tty_ops = {
    .open       = rv_uart_tty_open,
    .write      = rv_uart_tty_write,
    .write_room = rv_uart_tty_write_room,
};


static void rv_uart_console_write(struct console *co, const char *buf, unsigned n)
{
    struct riscv_uart *ru = (struct riscv_uart *)co->data;
    rv_uart_put_string(ru->reg, buf, n);
}

static struct tty_driver *rv_uart_console_device(struct console *co, int *index)
{
    struct riscv_uart *ru = (struct riscv_uart *)co->data;
    *index = co->index;
    return ru->rv_uart_tty_driver;
}

static int rv_uart_console_setup(struct console *co, char *options)
{
    return co->index != 0 ? -ENODEV : 0;
}

static int rv_uart_console_init(struct riscv_uart *ru)
{
    int ret;

    strcpy(ru->rv_uart_console.name, "RVuart_console");
    ru->rv_uart_console.write = rv_uart_console_write;
    ru->rv_uart_console.device = rv_uart_console_device;
    ru->rv_uart_console.setup = rv_uart_console_setup;
    ru->rv_uart_console.flags = CON_PRINTBUFFER;
    ru->rv_uart_console.index = -1;
    ru->rv_uart_console.data = (void*)ru;
    register_console(&ru->rv_uart_console);

    ru->rv_uart_tty_driver = tty_alloc_driver(1,
        TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV);
    if (unlikely(IS_ERR(ru->rv_uart_tty_driver)))
        return PTR_ERR(ru->rv_uart_tty_driver);

    ru->rv_uart_tty_driver->driver_name = "RVuart";
    ru->rv_uart_tty_driver->driver_state = (void*)ru;
    ru->rv_uart_tty_driver->name = "ttyRVuart";
    ru->rv_uart_tty_driver->major = TTY_MAJOR;
    ru->rv_uart_tty_driver->minor_start = 0;
    ru->rv_uart_tty_driver->type = TTY_DRIVER_TYPE_SERIAL;
    ru->rv_uart_tty_driver->subtype = SERIAL_TYPE_NORMAL;
    ru->rv_uart_tty_driver->init_termios = tty_std_termios;
    tty_set_operations(ru->rv_uart_tty_driver, &rv_uart_tty_ops);

    tty_port_init(&ru->rv_uart_tty_port);
    tty_port_link_device(&ru->rv_uart_tty_port, ru->rv_uart_tty_driver, 0);

    ret = tty_register_driver(ru->rv_uart_tty_driver);
    if (unlikely(ret))
        goto out_tty_put;

    return ret;

out_tty_put:
    put_tty_driver(ru->rv_uart_tty_driver);
    return ret;
}

static void rv_uart_console_exit(struct riscv_uart *ru)
{
    unregister_console(&ru->rv_uart_console);
    tty_unregister_driver(ru->rv_uart_tty_driver);
    put_tty_driver(ru->rv_uart_tty_driver);
}

static int rv_uart_probe(struct platform_device *pdev) {
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
    ru->rv_uart_tty_port_lock = __SPIN_LOCK_UNLOCKED(ru->rv_uart_tty_port_lock);
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
    ret = rv_uart_console_init(ru);

    if (ret == 0) {
        dev_info(&pdev->dev, "loaded rv_uart uart\n");
    } else {
        dev_warn(&pdev->dev, "failed to add rv_uart uart (%d)\n", ret);
    }

    // only when completely initialized, request the IRQ
    err = devm_request_irq(&pdev->dev, irq, rv_uart_console_isr,
                               IRQF_NO_THREAD,
                               "rv_uart_console", ru);
    if (err) {
        dev_err(&pdev->dev, "Unable to request irq %d\n", irq);
        return err;
    }

    return ret;
}

static int rv_uart_remove(struct platform_device *pdev) {
    struct riscv_uart *ru;

    ru = platform_get_drvdata(pdev);
    rv_uart_console_exit(ru);
    // free not needed, handled by the devm framework

    return 0;
}

static struct platform_driver rv_uart_driver = {
    .probe      = rv_uart_probe,
    .remove     = rv_uart_remove,
    .driver     = {
        .name   = "sbi",
    },
};

module_platform_driver(rv_uart_driver);

MODULE_DESCRIPTION("RISC-V UART driver");
MODULE_LICENSE("GPL");

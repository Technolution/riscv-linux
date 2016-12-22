
#include <linux/init.h>
#include <linux/console.h>

#include <asm/sbi.h>

static void sbi_console_write(struct console *co, const char *buf, unsigned n)
{
    for ( ; n > 0; n--, buf++) {
        if (*buf == '\n')
            sbi_console_putchar('\r');
        sbi_console_putchar(*buf);
    }
}

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

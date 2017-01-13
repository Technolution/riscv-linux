/**
  * @file
  * @author     Tom van Leeuwen <tom.van.leeuwen@technolution.eu>
  * @brief      Frenox Ethernet driver
  */

/** Documentation:
  * 
  * http://free-electrons.com/doc/network-drivers.pdf
  * dummy.c
  */


#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/moduleparam.h>
#include <linux/rtnetlink.h>
#include <net/rtnetlink.h>
#include <linux/u64_stats_sync.h>

#include "frenox_eth.h"

#define DRV_NAME        "frenox_eth"
#define DRV_VERSION     "1.0"
#define PHY_READCMD     0b0110
#define PHY_WRITECMD    0b0101
#define PHY_TURNAROUND  0b10
#define PHY_ADDR        0b10010

#undef USE_TX_ISR

struct pcpu_dstats {
        u64                     tx_packets;
        u64                     tx_bytes;
        struct u64_stats_sync   syncp;
};

struct frenox_priv {
    struct net_device_stats stats;
    struct sk_buff *skb;
    volatile uint32_t __iomem *reg;
    int rx_irq;
    int tx_irq;
    spinlock_t lock;
};
    

/**
 * frenox_eth_rx_isr - Interrupt Service Handler
 * @irq: IRQ number
 * @data: Ethernet device information
 *
 * Return: IRQ_HANDLED on success and IRQ_NONE on failure
 */
static irqreturn_t frenox_eth_rx_isr(int irq, void *data)
{
    struct net_device *dev = (struct net_device *)data;
    struct sk_buff *skb;
    
    struct frenox_priv *priv;
    volatile uint8_t __iomem *buf;
    int packet_len; // TODO: Ensure it can't overflow (in VHDL?)
    int packet_available;
    
    priv = netdev_priv(dev);
    buf = ((uint8_t *)priv->reg) + FRENOX_ETH_MAPPING_RX_BUFFER_OFFSET;
    
    if (unlikely(dev == NULL)) {
        printk(KERN_WARNING "frenox_eth_rx_isr: ISR called but device not initialized!\n");
        return IRQ_NONE;
    }
    spin_lock(&priv->lock);
    packet_available = priv->reg[FRENOX_ETH_MAPPING_CONTROL_RX_NEW_PKT_ADDRESS/4];
    if (packet_available == 0) {
        printk(KERN_WARNING "frenox_eth_rx_isr: ISR called but no packet available!\n");
        spin_unlock(&priv->lock);
        return IRQ_NONE;
    } 
    
    packet_len = priv->reg[FRENOX_ETH_MAPPING_CONTROL_RX_LEN_ADDRESS/4] - 4; //Subtract CRC
    skb = netdev_alloc_skb(dev, packet_len + NET_IP_ALIGN);
    if (!skb) {
        dev->stats.rx_dropped++;
        printk(KERN_ERR "frenox_eth_rx_isr: no memory available for packet!\n");
        spin_unlock(&priv->lock);
        return IRQ_RETVAL(1);
    }
    
    skb_reserve(skb, NET_IP_ALIGN);
    
    memcpy(skb->data, buf, packet_len);
    //skb_copy_to_linear_data(skb, buf, packet_len);
    skb_put(skb, packet_len);

    skb->protocol = eth_type_trans(skb, dev);
    skb_checksum_none_assert(skb);

    dev->stats.rx_packets++;
    dev->stats.rx_bytes += packet_len;
    // Send it to the etheret stack.
    netif_rx(skb);
    
    priv->reg[FRENOX_ETH_MAPPING_CONTROL_RX_ACK_PKT_ADDRESS/4] = 1;
    spin_unlock(&priv->lock);
    
    return IRQ_HANDLED;

}

#ifdef USE_TX_ISR
static irqreturn_t frenox_eth_tx_isr(int irq, void *data)
{
    struct net_device *dev = (struct net_device *)data;
    volatile uint32_t __iomem *reg;
    struct frenox_priv *priv;
    
    priv = netdev_priv(dev);
    
    spin_lock(&priv->lock);
    uint32_t done = priv->reg[FRENOX_ETH_MAPPING_CONTROL_TX_DONE_ADDRESS/4];
    // Clear interrupt anyway.
    priv->reg[FRENOX_ETH_MAPPING_CONTROL_TX_DONE_ADDRESS/4] = 1;
    spin_unlock(&priv->lock);
    netif_wake_queue(dev);
    
    if(done == 0) {
        printk(KERN_WARNING "frenox_eth_tx_isr: ISR called but transmission not completed!\n");
        return IRQ_NONE;
    }
    return IRQ_HANDLED;
}
#endif
/**
 * frenox_mdio_write - Write to MDIO register
 * @dev:        Frenox_eth net device
 * @address:    MDIO address
 * @data:       MDIO data
 */
static int frenox_mdio_write(struct net_device *dev, uint32_t address, uint32_t data) {
    struct frenox_priv *priv;
    volatile uint32_t __iomem *mdio;
    uint32_t command;
    
    priv = netdev_priv(dev);
    mdio = priv->reg + FRENOX_ETH_MAPPING_MDIO_OFFSET/4;

    command = (PHY_WRITECMD << 12) | (PHY_ADDR << 7) | (address << 2) | PHY_TURNAROUND;


    /* Wait until previous operation is done. */
    spin_lock(&priv->lock);
    while(mdio[1] & (1<<16));
    // Load data 
    mdio[3] = (command << 16) | (data & 0xFFFF);
    while(mdio[1] & (1<<16));
    spin_unlock(&priv->lock);
    return 0;
}
  
/**
 * frenox_mdio_read - Write to MDIO register
 * @dev:        Frenox_eth net device
 * @address:    MDIO address
 * @data:       Pointer to store the MDIO data.
 */
static int frenox_mdio_read(struct net_device *dev, uint32_t address, uint32_t *data) {
    struct frenox_priv *priv;
    volatile uint32_t __iomem *mdio;
    uint32_t command;
    
    priv = netdev_priv(dev);
    mdio = priv->reg + FRENOX_ETH_MAPPING_MDIO_OFFSET/4;
  
    command = (PHY_TURNAROUND << 14) | (address << 9) | (PHY_ADDR << 4) | PHY_READCMD;
  
    spin_lock(&priv->lock);
    /* Wait until previous operation is done. */
    while(mdio[1] & (1<<16));
    
    /* Start reading */
    mdio[2] = command & 0xFFFF;
    mdio[0] = (1<<0) | (1<<1);
    
    /* Wait until previous operation is done. */
    while(mdio[1] & (1<<16));
    
    *data = mdio[1] & 0xFFFF;
    spin_unlock(&priv->lock);
    
    return 0;
}

static netdev_tx_t frenox_eth_xmit(struct sk_buff *skb, struct net_device *dev)
{
    struct frenox_priv *priv;
    size_t curr_word;

    priv = netdev_priv(dev);
    // We can only transmit one packet concurrently. Immediately stop the queue.
    // If we stop the queue later, it is possible that the ISR is already expired.
    
    spin_lock(&priv->lock);
#ifndef USE_TX_ISR
    while(priv->reg[FRENOX_ETH_MAPPING_CONTROL_TX_BUSY_ADDRESS/4]);
#endif
    
    if(priv->reg[FRENOX_ETH_MAPPING_CONTROL_TX_BUSY_ADDRESS/4]) {
        printk(KERN_WARNING "Frenox_eth busy while xmit called again\n");
        spin_unlock(&priv->lock);
        return NETDEV_TX_BUSY;
    }
#ifdef USE_TX_ISR
    netif_stop_queue(dev);
#endif
    
    //printk("Transmitting %d bytes to %02X:%02X:%02X:%02X:%02X:%02X\n", skb->len, (int)skb->data[0], (int)skb->data[1], (int)skb->data[2], int)skb->data[3], (int)skb->data[4], (int)skb->data[5]);
    memcpy(priv->reg + FRENOX_ETH_MAPPING_TX_BUFFER_OFFSET/4, skb->data, skb->len);
    priv->reg[FRENOX_ETH_MAPPING_CONTROL_TX_LEN_ADDRESS/4] = skb->len;
    priv->reg[FRENOX_ETH_MAPPING_CONTROL_TX_SEND_NOW_ADDRESS/4] = 1;
    spin_unlock(&priv->lock);

    dev_kfree_skb(skb);

    
    return NETDEV_TX_OK;
}

static int frenox_eth_dev_init(struct net_device *dev)
{
    return 0;
}

static void frenox_eth_dev_uninit(struct net_device *dev)
{
    free_percpu(dev->dstats);
}


static const struct net_device_ops frenox_eth_netdev_ops = {
    .ndo_init               = frenox_eth_dev_init,
    .ndo_uninit             = frenox_eth_dev_uninit,
    .ndo_start_xmit         = frenox_eth_xmit,
    .ndo_set_mac_address    = eth_mac_addr,
};

static void frenox_eth_get_drvinfo(struct net_device *dev,
                              struct ethtool_drvinfo *info)
{
    strlcpy(info->driver, DRV_NAME, sizeof(info->driver));
    strlcpy(info->version, DRV_VERSION, sizeof(info->version));
}

static const struct ethtool_ops frenox_eth_ethtool_ops = {
    .get_drvinfo            = frenox_eth_get_drvinfo,
};

static void frenox_eth_setup(struct net_device *dev)
{
    ether_setup(dev);

    /* Initialize the device structure. */
    dev->netdev_ops = &frenox_eth_netdev_ops;
    dev->ethtool_ops = &frenox_eth_ethtool_ops;
    dev->destructor = free_netdev;

    memcpy(dev->dev_addr, "\x02\x13\xE6\x01\x02\x03", ETH_ALEN); /* TODO: Read from or write to TL_REG_BUS!*/
}

static int frenox_eth_validate(struct nlattr *tb[], struct nlattr *data[])
{
    if (tb[IFLA_ADDRESS]) {
        if (nla_len(tb[IFLA_ADDRESS]) != ETH_ALEN) {
            printk(KERN_ERR "frenox_eth_validate error: Incorrect network address length!\n");
            return -EINVAL;
        }
        if (!is_valid_ether_addr(nla_data(tb[IFLA_ADDRESS]))) {
            printk(KERN_ERR "frenox_eth_validate error: Incorrect network address format!\n");
            return -EADDRNOTAVAIL;
        }
    }
    return 0;
}

static struct rtnl_link_ops frenox_eth_link_ops __read_mostly = {
        .kind           = DRV_NAME,
        .setup          = frenox_eth_setup,
        .validate       = frenox_eth_validate,
};


static int frenox_eth_init(struct net_device *dev)
{
    int err;
    struct frenox_priv *priv;
    priv = netdev_priv(dev);
    
    rtnl_lock();
    dev->rtnl_link_ops = &frenox_eth_link_ops;
    printk("Registering netdev\n");
    err = register_netdevice(dev);
    printk("netdev register completed.\n");
    rtnl_unlock();
    if (err < 0) {
        printk(KERN_ERR "frenox_eth_init unable to register device!%d\n", err);
        return err;
    }
    
    /* Use hard-coded PHY configuration. */
    
    /* MDIO reg 22: Select page 0 */
    frenox_mdio_write(dev, 22, 0);
    /* MDIO reg 0: Copper Control. Set to 1Gbps full duplex (but since autoneg is enabled, it doesn't matter)*/
    frenox_mdio_write(dev, 0, (1<<12) | (1<<8) | (1<<6));
    /* MDIO reg 4: Auto-negotiation advertisement. Don't advertise 10 and 100 Mbit. */
    frenox_mdio_write(dev, 4, (1<<0));
    /* MDIO reg 9: Advertise 1Gbps full- and halfduplex. Prefer slave. */
    frenox_mdio_write(dev, 9, (1<<9) | (1<<8));
    /* MDIO reg 16: PHY specific control register. Enable auto-MDI-MDIX. No energy savings. */
    frenox_mdio_write(dev, 16, (1<<6) | (1<<5));
    /* MDIO reg 27: Extended PHY specific register. Force GMII to copper */
    frenox_mdio_write(dev, 27, (1<<15) | 0b1111);
    /* MDIO reg 0: Copper Control. Same as before, but also execute software reset.*/
    frenox_mdio_write(dev, 0, (1<<15) | (1<<12) | (1<<8) | (1<<6));
    
    /* Clear the incoming packets before we enable the interrupts */
    priv->reg[FRENOX_ETH_MAPPING_CONTROL_RX_ACK_PKT_ADDRESS/4] = 1;
    
    return err;
}   

static void frenox_eth_exit(struct net_device *dev)
{
}

static int frenox_eth_probe(struct platform_device *pdev) {
    struct net_device *dev;
    struct resource *res;
    struct frenox_priv *priv;
    void *base;
    int ret;
    int err;
    int irq_last;


    dev = alloc_netdev(sizeof(struct frenox_priv), "frenox_eth%d", NET_NAME_UNKNOWN, frenox_eth_setup);
    if (!dev) {
        return -ENOMEM;
    }
    priv = netdev_priv(dev);
    memset(priv, 0, sizeof(struct frenox_priv));
    spin_lock_init(&priv->lock);
    
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    base = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(base)) {
        dev_err(&pdev->dev, "Could not find Ethernet memory space\n");
        return PTR_ERR(base);
    }
    priv->reg = base;

    
    res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
    if (res == NULL) {
        dev_err(&pdev->dev, "Could not find eth irq\n");
        return PTR_ERR(base);
    }
    
    priv->rx_irq = res->start;
    priv->tx_irq = res->start + 1;
    
    platform_set_drvdata(pdev, dev);
    ret = frenox_eth_init(dev);

    if (ret == 0) {
        dev_info(&pdev->dev, "loaded frenox_eth\n");
    } else {
        dev_warn(&pdev->dev, "failed to add frenox_eth (%d)\n", ret);
    }

    
    // only when completely initialized, request the IRQ
    err = devm_request_irq(&pdev->dev, priv->rx_irq, frenox_eth_rx_isr,
                               IRQF_NO_THREAD,
                               "frenox_eth_rx", dev);
    if (err) {
        dev_err(&pdev->dev, "Unable to request irq %d\n", priv->rx_irq);
        return err;
    }
    
#ifdef USE_TX_ISR
    err = devm_request_irq(&pdev->dev, priv->tx_irq, frenox_eth_tx_isr,
                               IRQF_NO_THREAD,
                               "frenox_eth_tx", dev);
    if (err) {
        dev_err(&pdev->dev, "Unable to request irq %d\n", priv->tx_irq);
        return err;
    }
#endif

    return ret;
}

static int frenox_eth_remove(struct platform_device *pdev) {
    struct net_device *dev;
    dev = platform_get_drvdata(pdev);
    frenox_eth_exit(dev);
    // free not needed, handled by the devm framework

    return 0;
}

static struct platform_driver frenox_eth_driver = {
    .probe      = frenox_eth_probe,
    .remove     = frenox_eth_remove,
    .driver     = {
        .name   = "frenox_eth",
    },
};

module_platform_driver(frenox_eth_driver);

MODULE_DESCRIPTION("Frenox Ehternet driver");
MODULE_LICENSE("GPL");


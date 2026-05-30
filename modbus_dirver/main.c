/**
 ******************************************************************************
 * @file    modbus_uart_driver.c
 * @brief   A7 Linux kernel driver — /dev/modbus_uart
 *
 *  Exposes shared RETRAM circular buffer as a tty-like char device.
 *  Userspace can open /dev/modbus_uart and use it exactly like a serial port:
 *    write() → puts bytes into a7_to_m4 buffer → M4 sends out UART7
 *    read()  → gets bytes from m4_to_a7 buffer ← M4 received from slave
 *
 *  Works with libmodbus:
 *    modbus_new_rtu("/dev/modbus_uart", 9600, 'N', 8, 1)
 *
 *  Build:  make (see Makefile)
 *  Load:   insmod modbus_uart_driver.ko
 ******************************************************************************
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/timer.h>
#include <linux/slab.h>

#define DRIVER_NAME     "modbus_uart"
#define CLASS_NAME      "modbus_uart"

/* ── Must match shared_buffer.h exactly ─────────────────────────────────── */
#define SHARED_BASE         0x38000000UL
#define SHARED_MAGIC        0xABCD1234UL
#define CIRC_BUF_SIZE       512U
#define CIRC_BUF_MASK       (CIRC_BUF_SIZE - 1U)

#define CIRC_COUNT(h,t)     ((h)-(t))
#define CIRC_SPACE(h,t)     (CIRC_BUF_SIZE - ((h)-(t)))
#define CIRC_IDX(x)         ((x) & CIRC_BUF_MASK)

struct SharedHeader {
    uint32_t magic;
    uint32_t a7_to_m4_head;
    uint32_t a7_to_m4_tail;
    uint32_t m4_to_a7_head;
    uint32_t m4_to_a7_tail;
    uint32_t m4_status;
    uint32_t reserved[2];
} __attribute__((packed));

#define HDR_SIZE            sizeof(struct SharedHeader)
#define A7_TO_M4_OFFSET     HDR_SIZE
#define M4_TO_A7_OFFSET     (HDR_SIZE + CIRC_BUF_SIZE)
#define TOTAL_MAP_SIZE      (HDR_SIZE + CIRC_BUF_SIZE * 2 + 4096)

/* ── Driver state ────────────────────────────────────────────────────────── */
static dev_t            dev_num;
static struct cdev      mu_cdev;
static struct class    *mu_class;
static struct device   *mu_device;
static void __iomem    *retram;

static DECLARE_WAIT_QUEUE_HEAD(read_wq);
static DECLARE_WAIT_QUEUE_HEAD(write_wq);

static struct timer_list poll_timer;

/* ── Inline accessors ────────────────────────────────────────────────────── */
static inline struct SharedHeader *hdr(void)
    { return (struct SharedHeader *)retram; }

static inline uint8_t *a7_to_m4_buf(void)
    { return (uint8_t *)retram + A7_TO_M4_OFFSET; }

static inline uint8_t *m4_to_a7_buf(void)
    { return (uint8_t *)retram + M4_TO_A7_OFFSET; }

/* ── open ────────────────────────────────────────────────────────────────── */
static int mu_open(struct inode *inode, struct file *file)
{
    if (ioread32(&hdr()->magic) != SHARED_MAGIC) {
        pr_warn("modbus_uart: M4 not ready (magic mismatch)\n");
        return -ENODEV;
    }
    return 0;
}

static int mu_release(struct inode *inode, struct file *file) { return 0; }

/* ── write: A7 → M4 (Modbus request) ────────────────────────────────────── */
static ssize_t mu_write(struct file *file, const char __user *buf,
                         size_t count, loff_t *ppos)
{
    if (count == 0) return 0;
    if (count > CIRC_BUF_SIZE) return -EINVAL;

    /* Wait for space in a7_to_m4 buffer */
    uint32_t head, tail;
    if (file->f_flags & O_NONBLOCK) {
        head = ioread32(&hdr()->a7_to_m4_head);
        tail = ioread32(&hdr()->a7_to_m4_tail);
        if (CIRC_SPACE(head, tail) < count)
            return -EAGAIN;
    } else {
        if (wait_event_interruptible(write_wq, ({
                head = ioread32(&hdr()->a7_to_m4_head);
                tail = ioread32(&hdr()->a7_to_m4_tail);
                CIRC_SPACE(head, tail) >= count; })))
            return -ERESTARTSYS;
    }

    /* Copy from userspace into circular buffer */
    uint8_t tmp[CIRC_BUF_SIZE];
    if (copy_from_user(tmp, buf, count)) return -EFAULT;

    uint8_t *dst = a7_to_m4_buf();
    for (size_t i = 0; i < count; i++)
        iowrite8(tmp[i], dst + CIRC_IDX(head + i));

    /* Barrier then bump head — M4 sees new data */
    wmb();
    iowrite32(head + count, &hdr()->a7_to_m4_head);

    return (ssize_t)count;
}

/* ── read: M4 → A7 (Modbus response) ────────────────────────────────────── */
static ssize_t mu_read(struct file *file, char __user *buf,
                        size_t count, loff_t *ppos)
{
    uint32_t head, tail, avail;

    if (file->f_flags & O_NONBLOCK) {
        head  = ioread32(&hdr()->m4_to_a7_head);
        tail  = ioread32(&hdr()->m4_to_a7_tail);
        avail = CIRC_COUNT(head, tail);
        if (avail == 0) return -EAGAIN;
    } else {
        if (wait_event_interruptible(read_wq, ({
                head  = ioread32(&hdr()->m4_to_a7_head);
                tail  = ioread32(&hdr()->m4_to_a7_tail);
                CIRC_COUNT(head, tail) > 0; })))
            return -ERESTARTSYS;
        avail = CIRC_COUNT(head, tail);
    }

    if (avail > count) avail = count;

    /* Copy from circular buffer to userspace */
    uint8_t tmp[CIRC_BUF_SIZE];
    uint8_t *src = m4_to_a7_buf();
    for (uint32_t i = 0; i < avail; i++)
        tmp[i] = ioread8(src + CIRC_IDX(tail + i));

    if (copy_to_user(buf, tmp, avail)) return -EFAULT;

    /* Barrier then bump tail */
    rmb();
    iowrite32(tail + avail, &hdr()->m4_to_a7_tail);

    return (ssize_t)avail;
}

/* ── poll ────────────────────────────────────────────────────────────────── */
static __poll_t mu_poll(struct file *file, poll_table *wait)
{
    __poll_t mask = 0;
    poll_wait(file, &read_wq,  wait);
    poll_wait(file, &write_wq, wait);

    uint32_t rh = ioread32(&hdr()->m4_to_a7_head);
    uint32_t rt = ioread32(&hdr()->m4_to_a7_tail);
    uint32_t wh = ioread32(&hdr()->a7_to_m4_head);
    uint32_t wt = ioread32(&hdr()->a7_to_m4_tail);

    if (CIRC_COUNT(rh, rt) > 0)           mask |= EPOLLIN  | EPOLLRDNORM;
    if (CIRC_SPACE(wh, wt) >= 8)          mask |= EPOLLOUT | EPOLLWRNORM;
    return mask;
}

/* ── Timer: wakes blocked readers/writers every 5ms ─────────────────────── */
static void poll_timer_cb(struct timer_list *t)
{
    wake_up_interruptible(&read_wq);
    wake_up_interruptible(&write_wq);
    mod_timer(&poll_timer, jiffies + msecs_to_jiffies(5));
}

/* ── File ops ────────────────────────────────────────────────────────────── */
static const struct file_operations mu_fops = {
    .owner   = THIS_MODULE,
    .open    = mu_open,
    .release = mu_release,
    .read    = mu_read,
    .write   = mu_write,
    .poll    = mu_poll,
};

/* ══════════════════════════════════════════════════════════════════════════
 *  Module init / exit
 * ══════════════════════════════════════════════════════════════════════════*/
static int __init mu_init(void)
{
    int ret;

    retram = ioremap(SHARED_BASE, TOTAL_MAP_SIZE);
    if (!retram) {
        pr_err("modbus_uart: ioremap failed\n");
        return -ENOMEM;
    }

    ret = alloc_chrdev_region(&dev_num, 0, 1, DRIVER_NAME);
    if (ret < 0) goto err_unmap;

    cdev_init(&mu_cdev, &mu_fops);
    mu_cdev.owner = THIS_MODULE;
    ret = cdev_add(&mu_cdev, dev_num, 1);
    if (ret < 0) goto err_unreg;

    mu_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(mu_class)) { ret = PTR_ERR(mu_class); goto err_cdev; }

    mu_device = device_create(mu_class, NULL, dev_num, NULL, DRIVER_NAME);
    if (IS_ERR(mu_device)) { ret = PTR_ERR(mu_device); goto err_class; }

    /* Poll timer wakes blocked read()/write() callers */
    timer_setup(&poll_timer, poll_timer_cb, 0);
    mod_timer(&poll_timer, jiffies + msecs_to_jiffies(5));

    pr_info("modbus_uart: /dev/modbus_uart ready (RETRAM 0x%08lX)\n",
            SHARED_BASE);
    return 0;

err_class:  class_destroy(mu_class);
err_cdev:   cdev_del(&mu_cdev);
err_unreg:  unregister_chrdev_region(dev_num, 1);
err_unmap:  iounmap(retram);
    return ret;
}

static void __exit mu_exit(void)
{
    del_timer_sync(&poll_timer);
    device_destroy(mu_class, dev_num);
    class_destroy(mu_class);
    cdev_del(&mu_cdev);
    unregister_chrdev_region(dev_num, 1);
    iounmap(retram);
    pr_info("modbus_uart: unloaded\n");
}

module_init(mu_init);
module_exit(mu_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RETRAM shared buffer UART bridge — /dev/modbus_uart");
MODULE_VERSION("1.0");
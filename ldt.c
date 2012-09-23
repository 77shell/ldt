/*
 *	LDT - Linux Driver Template
 *
 *	Copyright (C) 2012 Constantine Shulyupin http://www.makelinux.net/
 *
 *	Dual BSD/GPL License
 *
 *
 *	The driver demonstrates usage of following Linux facilities:
 *
 *	Linux kernel module
 *	file_operations
 *		read and write
 *		blocking read
 *		poll
 *		mmap
 *		ioctl
 *	kfifo
 *	completion
 *	interrupt
 *	tasklet
 *	timer
 *	work
 * 	kthread
 *	misc device
 *	proc fs
 *	platform_driver and platform_device in another module
 */

#include <asm/io.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/timer.h>
#include <linux/kfifo.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/serial_reg.h>

static int bufsize = PFN_ALIGN(16 * 1024);
static void *in_buf;
static void *out_buf;

static int irq = 4;
module_param(irq, int, 0);

static int loopback;
module_param(loopback, int, 0);

int portn = 0x3f8; /* serial */
module_param(portn, int, 0);

/*
 * Prints execution context: hard interrupt, soft interrupt or scheduled task
 */

#define print_context()	\
	printk(KERN_DEBUG"%s:%d %s %s %x\n", __file__, __LINE__, __func__, \
			in_irq()?"hardirq":current->comm,preempt_count());

#define check(a) \
( ret=a,((ret<0)?tracef("%s:%i %s FAIL\n\t%i=%s\n",__FILE__,__LINE__,__FUNCTION__,ret,#a):0),ret)

#define trace(a) \
do { printk("%s:%i %s calling %s\n",__FILE__,__LINE__,__FUNCTION__,#a);a; printk("done\n"); } while (0)

static int isr_counter;
static int ldt_work_counter;

#define FIFO_SIZE 128		/* should be power of two */
static DEFINE_KFIFO(in_fifo, char, FIFO_SIZE);
static DEFINE_KFIFO(out_fifo, char, FIFO_SIZE);

static DECLARE_WAIT_QUEUE_HEAD(ldt_readable);

spinlock_t fifo_lock;

/*	ldt_received - called with data received from HW port
 *	Called from interrupt or emulated function
 */
void ldt_received(void *data, int size)
{
	kfifo_in_spinlocked(&in_fifo, data, size, &fifo_lock);
	wake_up_interruptible(&ldt_readable);
}

/*	ldt_port_put - emulates sending and receiving data to loop-back HW port
 *
 */
void ldt_port_put(void *data, int size)
{
#ifdef USE_UART
	if ( inb(portn+UART_LSR) & UART_LSR_THRE) {
		outb(*(char*)data,portn + UART_TX);
	}
#endif
#ifdef USE_SW_LOOPBACK
	/*
	 * emulate loop back port
	 */
	ldt_received(data, size);
#endif
}

static DECLARE_COMPLETION(ldt_complete);

/* Fictive label _entry is used for tracing */

void ldt_work_func(struct work_struct *work)
{
_entry:;
       once(print_context());
       ldt_work_counter++;
}

DECLARE_WORK(ldt_work, ldt_work_func);

void ldt_tasklet_func(unsigned long d)
{
	int ret;
	char data_out, data_in;
_entry:
	once(print_context());
	trvx(inb(portn+UART_LSR));
	ret = kfifo_out_spinlocked(&out_fifo, &data_out, sizeof(data_out), &fifo_lock);
	if (ret) {
		trl_();
		trvd(data_out);
		ldt_port_put(&data_out, sizeof(data_out));
	}

#ifdef USE_UART
	if ( inb(portn+UART_LSR) & UART_LSR_DR ) {
		data_in = inb(portn + UART_RX);
		trl_();trvd(data_in);
		ldt_received(&data_in, sizeof(data_in));
	}
#endif
	schedule_work(&ldt_work);
	complete(&ldt_complete);
}

DECLARE_TASKLET(ldt_tasklet, ldt_tasklet_func, 0);

irqreturn_t ldt_isr(int irq, void *dev_id, struct pt_regs *regs)
{
_entry:
	trl();
	once(print_context());
	isr_counter++;
	tasklet_schedule(&ldt_tasklet);
	//return IRQ_NONE;	/* not our IRQ */
	return IRQ_HANDLED; /* our IRQ */
}

struct timer_list ldt_timer;
void ldt_timer_func(unsigned long data)
{
_entry:
	/*
	 *      this timer is used just to fire tasklet, when there is no interrupt
	 */
	//tasklet_schedule(&ldt_tasklet);
	mod_timer(&ldt_timer, jiffies + HZ / 100);
}

DEFINE_TIMER(ldt_timer, ldt_timer_func, 0, 0);

static int ldt_open(struct inode *inode, struct file *file)
{
_entry:
	trl_();
	trvx(file->f_flags & O_NONBLOCK);
	return 0;
}

static int ldt_release(struct inode *inode, struct file *file)
{
_entry:
	return 0;
}

static DEFINE_MUTEX(read_lock);

static ssize_t ldt_read(struct file *file, char __user * buf, size_t count, loff_t * ppos)
{
	int ret;
	unsigned int copied;
_entry:
	// TODO: implement blocking I/O
	if (kfifo_is_empty(&in_fifo)) {
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto exit;
		} else {
			trlm("waiting");
			ret = wait_event_interruptible(ldt_readable, !kfifo_is_empty(&in_fifo));
			if (ret == -ERESTARTSYS) {
				trlm("interrupted");
				ret = -EINTR;
				goto exit;
			}
		}
	}
	if (mutex_lock_interruptible(&read_lock)) {
		trlm("interrupted");
		return -EINTR;
	}
	ret = kfifo_to_user(&in_fifo, buf, count, &copied);
	mutex_unlock(&read_lock);
exit:
	return ret ? ret : copied;
}

static DEFINE_MUTEX(write_lock);

static ssize_t ldt_write(struct file *file, const char __user * buf, size_t count, loff_t * ppos)
{
	int ret;
	unsigned int copied;
_entry:
	// TODO wait_event_interruptible ... ldt_writeable
	if (mutex_lock_interruptible(&write_lock)) {
		return -EINTR;
	}
	ret = kfifo_from_user(&out_fifo, buf, count, &copied);
	mutex_unlock(&write_lock);
	return ret ? ret : copied;
}

static unsigned int ldt_poll(struct file *file, poll_table * pt)
{
	unsigned int mask = 0;
_entry:
	poll_wait(file, &ldt_readable, pt);
	//poll_wait(file, ldt_writeable, pt); // TODO

	if (!kfifo_is_empty(&in_fifo)) {
		mask |= POLLIN | POLLRDNORM;
	}
	mask |= POLLOUT | POLLWRNORM;
	//mask |= POLLHUP; // on output eof
	//mask |= POLLERR; // on output error
	trl_();
	trvx(mask);
	return mask;
}

/*
 *	pages_flag - set or clear a flag for sequence of pages
 *
 *	more generic solution instead SetPageReserved, ClearPageReserved etc
 */

void pages_flag(struct page *page, int pages, int mask, int value)
{
	for (; pages; pages--, page++)
		if (value)
			__set_bit(mask, &page->flags);
		else
			__clear_bit(mask, &page->flags);
}

static int ldt_mmap(struct file *filp, struct vm_area_struct *vma)
{
	void *buf = NULL;
_entry:
	if (vma->vm_flags & VM_WRITE)
		buf = in_buf;
	else if (vma->vm_flags & VM_READ)
		buf = out_buf;
	if (!buf)
		return -EINVAL;
	if (remap_pfn_range(vma, vma->vm_start, virt_to_phys(buf) >> PAGE_SHIFT,
			    vma->vm_end - vma->vm_start, vma->vm_page_prot)) {
		trlm("remap_pfn_range failed");
		return -EAGAIN;
	}
	return 0;
}

long ldt_ioctl(struct file *f, unsigned int cmnd, unsigned long arg)
{
	void __user *user = (void *)arg;
_entry:
	trl_();
	trvx_(cmnd);
	trvx(arg);
	trace_ioctl(cmnd);
	if (_IOC_DIR(cmnd) == _IOC_WRITE) {
		copy_from_user(in_buf, user, _IOC_SIZE(cmnd));
		memcpy(out_buf, in_buf, bufsize);
		memset(in_buf, 0, bufsize);
	}
	if (_IOC_DIR(cmnd) == _IOC_READ) {
		copy_to_user(user, out_buf, _IOC_SIZE(cmnd));
		memset(out_buf, 0, bufsize);
	}
	switch (_IOC_TYPE(cmnd)) {
	case 'A':
		switch (_IOC_NR(cmnd)) {
		case 0:
			break;
		}
		break;
	}
	return 0;
}

struct file_operations ldt_fops = {
	.owner = THIS_MODULE,
	.open = ldt_open,
	.release = ldt_release,
	.read = ldt_read,
	.write = ldt_write,
	.poll = ldt_poll,
	.mmap = ldt_mmap,
	.unlocked_ioctl = ldt_ioctl,
};

static struct miscdevice ldt_miscdev = {
	MISC_DYNAMIC_MINOR,
	KBUILD_MODNAME,
	&ldt_fops,
};

int ldt_thread_sub(void *data)
{
	int ret = 0;
_entry:
	/*
	   perform here a useful work in task context
	 */
	return ret;
}

int ldt_thread(void *data)
{
	int ret = 0;
_entry:
	print_context();
	allow_signal(SIGINT);
	while (!kthread_should_stop()) {
		ret = check(wait_for_completion_interruptible(&ldt_complete));
		if (ret == -ERESTARTSYS) {
			trlm("interrupted");
			ret = -EINTR;
			break;
		}
		trllog();
		ret = ldt_thread_sub(data);
	}
	return ret;
}

static struct task_struct *thread;

static __devinit int ldt_probe(struct platform_device *pdev)
{
	int ret;
	char *data = NULL;
	struct resource *r;
_entry:
	print_context();
	trl_();
	trvs_(__DATE__);
	trvs_(__TIME__);
	trvs_(KBUILD_MODNAME);
	trl_();
	trvp_(pdev);
	trvd_(irq);
	trvd_(bufsize);
	trln();
	if (!(in_buf = alloc_pages_exact(bufsize, GFP_KERNEL | __GFP_ZERO))) {
		ret = -ENOMEM;
		goto exit;
	}
	pages_flag(virt_to_page(in_buf), PFN_UP(bufsize), PG_reserved, 1);
	if (!(out_buf = alloc_pages_exact(bufsize, GFP_KERNEL | __GFP_ZERO))) {
		ret = -ENOMEM;
		goto exit;
	}
	pages_flag(virt_to_page(out_buf), PFN_UP(bufsize), PG_reserved, 1);
	//ret = register_chrdev (0, KBUILD_MODNAME, &ldt_fops);
	if (pdev) {
		data = pdev->dev.platform_data;
		r = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
		if (!irq) {
			irq = r->start;
		}
	}
	trvp(data);
	trvs(data);
	ret = check(misc_register(&ldt_miscdev));
	//ret = check(register_chrdev (0, KBUILD_MODNAME, &ldt_fops));
	if (ret < 0)
		goto exit;
	trvd(ldt_miscdev.minor);
	isr_counter = 0;
	if (portn) {
		//release_region(portn, 8);
		if ( !request_region(portn, 8, KBUILD_MODNAME)) {
			printk(KERN_WARNING"portn is already used\n");
		}
	}
	if (irq) {
		ret = check(request_irq(irq, (void *)ldt_isr, IRQF_SHARED, KBUILD_MODNAME, THIS_MODULE));
		if (ret > -1) {
			outb(UART_IER_RDI | UART_IER_RLSI , portn + UART_IER);
			//outb(UART_FCR_R_TRIG_11 | UART_FCR_ENABLE_FIFO | UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT, portn + UART_FCR);
			outb(UART_FCR_ENABLE_FIFO | UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT, portn + UART_FCR);
			outb(UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2, portn + UART_MCR);
			if (loopback)
				outb(inb(portn+UART_MCR) | UART_MCR_LOOP, portn + UART_MCR);
		}
	}
	trvd(ret);
	trvx(inb(portn+UART_IER));
	trvx(inb(portn+UART_IIR));
	trvx(inb(portn+UART_FCR));
	trvx(inb(portn+UART_LCR));
	trvx(inb(portn+UART_MCR));
	proc_create(KBUILD_MODNAME, 0, NULL, &ldt_fops);
	mod_timer(&ldt_timer, jiffies + HZ / 10);
	thread = kthread_run(ldt_thread, NULL, "%s", KBUILD_MODNAME);
	if (IS_ERR(thread)) {
		ret = PTR_ERR(thread);
	}
exit:
	trl_();
	trvd(ret);
	return ret;
}

static int __devexit ldt_remove(struct platform_device *pdev)
{
_entry:
	trace(remove_proc_entry(KBUILD_MODNAME, NULL));

	trace(misc_deregister(&ldt_miscdev));
	if (!IS_ERR(thread)) {
		trace(send_sig(SIGINT, thread, 1));
		trace(kthread_stop(thread));
	}
	trace(del_timer(&ldt_timer));
	if (irq) {
		outb(0, portn + UART_IER);
		outb(0, portn + UART_FCR);
		outb(0, portn + UART_FCR);
		inb(portn + UART_RX);
		trace(free_irq(irq, THIS_MODULE));
	}
	tasklet_kill(&ldt_tasklet);
	if (in_buf) {
		pages_flag(virt_to_page(in_buf), PFN_UP(bufsize), PG_reserved, 0);
		free_pages_exact(in_buf, bufsize);
	}
	if (out_buf) {
		pages_flag(virt_to_page(out_buf), PFN_UP(bufsize), PG_reserved, 0);
		free_pages_exact(out_buf, bufsize);
	}
	trvd(isr_counter);
	trvd(ldt_work_counter);
	return 0;
}

#ifdef USE_PLATFORM_DEVICE

/*
 * Following code requires platform_device (ldt_plat_dev.*) to work
 */

static struct platform_driver ldt_driver = {
	.driver.name = "ldt_device_name",
	.driver.owner = THIS_MODULE,
	.probe = ldt_probe,
	.remove = __devexit_p(ldt_remove),
};

#ifdef module_platform_driver
module_platform_driver(ldt_driver);
#else

/*
 *	for Linux kernel releases before v3.1-12 without macro module_platform_driver
 */

static int ldt_init(void)
{
	int ret = 0;
_entry:
	ret = platform_driver_register(&ldt_driver);
	return ret;
}

static void ldt_exit(void)
{
_entry:
	platform_driver_unregister(&ldt_driver);
}

module_init(ldt_init);
module_exit(ldt_exit);
#endif // module_platform_driver

#else // ! USE_PLATFORM_DEVICE

/*
 * Standalone module initialization to run without platform_device
 */

static int ldt_init(void)
{
	int ret = 0;
_entry:
	/*
	 * Call probe function directly, bypassing platform_device infrastructure
	 */
	ret = ldt_probe(NULL);
	return ret;
}

static void ldt_exit(void)
{
	int res;
_entry:
	res = ldt_remove(NULL);
}

module_init(ldt_init);
module_exit(ldt_exit);
#endif

MODULE_DESCRIPTION("LDT - Linux Driver Template");
MODULE_AUTHOR("Constantine Shulyupin <const@makelinux.net>");
MODULE_LICENSE("Dual BSD/GPL");

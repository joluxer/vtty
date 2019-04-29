#include <linux/module.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/mutex.h>
#include <linux/miscdevice.h>
#include <linux/circ_buf.h>
#include <linux/sched.h>
#include <linux/poll.h>

#include "vtty.h"

// FIXME tty_port_tty_get refcounting needed?

// borrowed from serial_core.h
#define VTTY_XMIT_SIZE  PAGE_SIZE
#define WAKEUP_CHARS 256

#define vtty_circ_empty(circ)	   ((circ)->head == (circ)->tail)
#define vtty_circ_clear(circ)	   ((circ)->head = (circ)->tail = 0)

#define vtty_circ_chars_pending(circ)   \
	(CIRC_CNT((circ)->head, (circ)->tail, VTTY_XMIT_SIZE))

#define vtty_circ_chars_free(circ)      \
	(CIRC_SPACE((circ)->head, (circ)->tail, VTTY_XMIT_SIZE))
// --

#define VTTY_MAX 	(16)

static struct tty_driver *vtty_driver;

#ifndef TCGETS2
#error "This code is not adapted to run on an arch lacking TCGETS2/termios2"
#endif

union vtty_oob_data {
	struct ktermios termios;
	struct termios2 user_termios; // used only for sizeof
	unsigned int modem_lines;
	int break_val;
};

// protects creation/destruction of tty ports
// per-port data is protected by the tty spinlock
static struct mutex portlock;

struct vtty_port {
	struct tty_port port;
	struct circ_buf xmit;
	struct device *dev;
	struct tty_struct *tty;

	int oob_tag;
	union vtty_oob_data oob_data;
	size_t oob_size;

	unsigned int modem_state;

	wait_queue_head_t read_wait, write_wait, mi_wait; // read/write from vtmx perspective
	wait_queue_head_t oob_wait; // vtty-side ioctl => vtmx-side read
};
static struct vtty_port ports[VTTY_MAX];

static int vtty_open(struct tty_struct *tty, struct file *filp)
{
	int ret = 0;
	struct vtty_port *vtty = &ports[tty->index];
	mutex_lock(&portlock);
	if(vtty->dev) {
		vtty->tty = tty;
		set_bit(TTY_THROTTLED, &tty->flags); 	// just like pty_open

	} else {
		// race between vtmx_release() & vtty_open()
		ret = -EIO;
	}

	mutex_unlock(&portlock);
	return ret;
}

static void vtty_close(struct tty_struct *tty, struct file *filp)
{
	struct vtty_port *vtty = &ports[tty->index];
	mutex_lock(&portlock);
	vtty->tty = NULL;
	mutex_unlock(&portlock);
	return;
}

static int vtty_write(struct tty_struct *tty, const unsigned char *buf, int count)
{
	// the TTY layer manages -EAGAIN and (non-)blocking writes
	struct vtty_port *port = &ports[tty->index];
	struct circ_buf *circ = &port->xmit;
	unsigned long flags;
	int ret = 0;
	spin_lock_irqsave(&port->port.lock, flags);

	while (1) {
		int c = CIRC_SPACE_TO_END(circ->head, circ->tail, VTTY_XMIT_SIZE);
		if (count < c)
			c = count;

		if (c == 0) {
			break;
		}
		memcpy(circ->buf + circ->head, buf, c);
		circ->head = (circ->head + c) & (VTTY_XMIT_SIZE - 1);
		buf += c;
		count -= c;
		ret += c;
	}

	wake_up_interruptible_sync_poll(&port->read_wait, POLLIN | POLLRDNORM);
	spin_unlock_irqrestore(&port->port.lock, flags);
	
	printk("Written %d (cste=%d)\n", ret, CIRC_SPACE_TO_END(circ->head, circ->tail, VTTY_XMIT_SIZE));
	return ret;
}

static int vtty_write_room(struct tty_struct *tty)
{
	struct vtty_port *port = &ports[tty->index];
	struct circ_buf *circ = &port->xmit;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&port->port.lock, flags);
	ret = vtty_circ_chars_free(circ);
	spin_unlock_irqrestore(&port->port.lock, flags);
	printk("Write room = %d\n", ret);
	return ret;
}

static int vtty_chars_in_buffer(struct tty_struct *tty)
{
	struct vtty_port *port = &ports[tty->index];
	struct circ_buf *circ = &port->xmit;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&port->port.lock, flags);
	ret = vtty_circ_chars_pending(circ);
	spin_unlock_irqrestore(&port->port.lock, flags);
	return ret;
}


// call with port spinlock
static int vtty_wait_oob(struct vtty_port *port, unsigned long *pflags)
{
	DEFINE_WAIT(wait);

	while(port->oob_size != 0) {
		if (signal_pending(current)) 
			return -ERESTARTSYS;

		prepare_to_wait(&port->oob_wait, &wait, TASK_INTERRUPTIBLE);
		spin_unlock_irqrestore(&port->port.lock, *pflags);
		schedule();
		finish_wait(&port->oob_wait, &wait);
		spin_lock_irqsave(&port->port.lock, *pflags);
	}

	return 0;
}

static void vtty_do_queue_oob(struct vtty_port *port, int tag, void *data, size_t len)
{
	port->oob_tag = tag;
	memcpy(&port->oob_data, data, port->oob_size = len);
	wake_up_interruptible_sync_poll(&port->read_wait, POLLIN | POLLRDNORM);
}

static int vtty_queue_oob(struct vtty_port *port, int tag, void *data, size_t len)
{
	int ret = 0;
	unsigned long flags;
	spin_lock_irqsave(&port->port.lock, flags);
	ret = vtty_wait_oob(port, &flags);

	if(ret == 0)
		vtty_do_queue_oob(port, tag, data, len);

	spin_unlock_irqrestore(&port->port.lock, flags);
	return ret;
}

static void vtty_set_termios(struct tty_struct *tty, struct ktermios *old_termios)
{
	struct vtty_port *port = &ports[tty->index];

	// this may sleep
	vtty_queue_oob(port, TAG_SET_TERMIOS, &tty->termios, sizeof(tty->termios));
}

static void vtty_throttle(struct tty_struct *tty)
{
	printk("throttle!\n");

}

static void vtty_unthrottle(struct tty_struct *tty)
{
	struct vtty_port *port = &ports[tty->index];
	printk("unthrottle\n");
	wake_up_interruptible_sync_poll(&port->write_wait, POLLOUT | POLLWRNORM);
	set_bit(TTY_THROTTLED, &tty->flags); 	// just like pty_unthrottle
}

static int vtty_tiocmget(struct tty_struct *tty)
{
	struct vtty_port *port = &ports[tty->index];
	unsigned long flags;
	unsigned int ret;
	spin_lock_irqsave(&port->port.lock, flags);
	ret = port->modem_state;
	spin_unlock_irqrestore(&port->port.lock, flags);
	return ret;
}

static int vtty_tiocmset(struct tty_struct *tty, unsigned int set, unsigned int clear)
{
	struct vtty_port *port = &ports[tty->index];
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&port->port.lock, flags);
	ret = vtty_wait_oob(port, &flags);

	if(ret == 0) {
		port->modem_state = (port->modem_state | set) & (~clear);
		printk("new modem state %x (%x %x)\n", port->modem_state, set, clear);
		vtty_do_queue_oob(port, TAG_SET_MODEM, &port->modem_state, sizeof(port->modem_state));
	}

	spin_unlock_irqrestore(&port->port.lock, flags);
	return ret;
}

// Sorry, this ioctl is - by design - next to useless.
//
// First, it is not possible to convert it to poll/select
// Second, it's susceptible to race conditions. The interesting modem lines
// may change between TIOCMGET and TIOCMIWAIT calls, leaving the caller with
// an infinite wait.
//
// See a 12-year old thread
// https://linux-serial.vger.kernel.narkive.com/wKd7uZgP/new-ioctl-for-modem-control-lines-monitoring#post1
//
static int vtty_tiocmiwait(struct tty_struct *tty, unsigned long arg)
{
	struct vtty_port *port = &ports[tty->index];
	unsigned long flags;
	int ret = 0;
	unsigned int old_state;
	DEFINE_WAIT(wait);
	spin_lock_irqsave(&port->port.lock, flags);
	old_state = port->modem_state;

	for(;;) {
		if((port->modem_state ^ old_state) & arg)
			break;

		if (signal_pending(current)) {
			ret = -ERESTARTSYS;
			break;
		}
	
		prepare_to_wait(&port->mi_wait, &wait, TASK_INTERRUPTIBLE);
		spin_unlock_irqrestore(&port->port.lock, flags);
		schedule();
		finish_wait(&port->mi_wait, &wait);
		spin_lock_irqsave(&port->port.lock, flags);

	}

	spin_unlock_irqrestore(&port->port.lock, flags);
	return ret;
}

static int vtty_ioctl(struct tty_struct *tty, unsigned int cmd, unsigned long arg)
{
	if(cmd == TIOCMIWAIT) {
		return vtty_tiocmiwait(tty, arg);
	} 

	return -ENOIOCTLCMD;
}
	
static int vtty_get_icount(struct tty_struct *tty, struct serial_icounter_struct *icount)
{
	return -EINVAL;
}
	
static int vtty_break_ctl(struct tty_struct *tty, int state)
{
	struct vtty_port *port = &ports[tty->index];
	// state = -1 	-> on
	// state = 0 	-> off
	// state > 0 	-> on for X ms
	vtty_queue_oob(port, TAG_BREAK_CTL, &state, sizeof(state));
	return 0;
}

static const struct tty_operations vtty_ops = {
	.open = vtty_open,
	.close = vtty_close,
	.write = vtty_write,
	.write_room = vtty_write_room,
	.chars_in_buffer = vtty_chars_in_buffer,
	.set_termios = vtty_set_termios,
	.throttle = vtty_throttle,
	.unthrottle = vtty_unthrottle,
	.tiocmget = vtty_tiocmget,
	.tiocmset = vtty_tiocmset,
	.ioctl = vtty_ioctl,
	.get_icount = vtty_get_icount,
	.break_ctl = vtty_break_ctl,
};

static int vtty_create_port(int index)
{
	struct vtty_port *port = &ports[index];
	struct device *dev;
	unsigned long page;
	int ret = 0;
	
	tty_port_init(&port->port);
	tty_buffer_set_limit(&port->port, 8192);
	
	page = get_zeroed_page(GFP_KERNEL);
	if (!page) {
		ret = -ENOMEM;
		goto fail_destroy;
	}

	port->xmit.buf = (unsigned char *) page;
	vtty_circ_clear(&port->xmit);

	dev = tty_port_register_device(&port->port, vtty_driver, index, NULL);

	if(IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		goto fail_free;
	}

	init_waitqueue_head(&port->read_wait);
	init_waitqueue_head(&port->write_wait);
	init_waitqueue_head(&port->mi_wait);
	init_waitqueue_head(&port->oob_wait);
	port->dev = dev;
	return 0;

fail_free:
	free_page((unsigned long)port->xmit.buf);
	port->xmit.buf = NULL;

fail_destroy:
	tty_port_destroy(&port->port);
	memset(port, 0, sizeof(*port));
	return ret;
}

// called with portlock
static void vtty_destroy_port(int index)
{
	struct vtty_port *port = &ports[index];
//	unsigned long flags;
//	spin_lock_irqsave(&port->port.lock, flags);
//	spin_unlock_irqrestore(&port->port.lock, flags);

	if(port->tty) {
		set_bit(TTY_IO_ERROR, &port->tty->flags);

		// FIXME do some waking up?
		// FIXME locking?
		set_bit(TTY_OTHER_CLOSED, &port->tty->flags);

		// again wake up?
		tty_vhangup(port->tty);
	}

	tty_unregister_device(vtty_driver, index);

	free_page((unsigned long)port->xmit.buf);
	port->xmit.buf = NULL;

	port->dev = NULL;
	tty_port_destroy(&port->port);
	memset(port, 0, sizeof(*port));
}

static int vtty_find_free_port(void)
{
	int i;
	for(i=0;i < VTTY_MAX;i++)
		if(!ports[i].dev)
			return i;
	return -ENOSPC;
}

static int vtmx_open(struct inode *nodp, struct file *filp)
{
	int rv = 0;
	int index;
	mutex_lock(&portlock);
	index = vtty_find_free_port();
	if(index < 0) {
		rv = index;
		goto fail_unlock;
	}

	rv = vtty_create_port(index);
	if(rv < 0)
		goto fail_unlock;

	filp->private_data = &ports[index];
	mutex_unlock(&portlock);

	nonseekable_open(nodp, filp);
	return rv;

fail_unlock:
	mutex_unlock(&portlock);
	return rv;
}

static int vtmx_release (struct inode *nodp, struct file *filp)
{
	int idx;
	mutex_lock(&portlock);
	idx = ((struct vtty_port*)filp->private_data) - ports;
	vtty_destroy_port(idx);
	mutex_unlock(&portlock);
	return 0;
}

static ssize_t vtmx_read (struct file *filp, char __user *ptr, size_t size, loff_t *off)
{
	struct vtty_port *port = filp->private_data;
	struct circ_buf *circ = &port->xmit;
	unsigned long flags;
	int ret = 0;
	DEFINE_WAIT(wait);

	if(size < 1 + sizeof(union vtty_oob_data)) 
		return -EMSGSIZE; // don't bother with clueless userspace apps

	spin_lock_irqsave(&port->port.lock, flags);

#if 0
	// FIXME tty->stopped (handling xon/xoff)
	// FIXME if (file->f_flags & O_NONBLOCK) {
	if(vtty_circ_empty(circ)/* || uart_tx_stopped */) {
		//?
		ret = 0;
		goto out;
	}
#endif
	printk("read(%d)\n", (int)size);
	while(size > 0) {
		int c;
		printk("(size=%d ret=%d)\n", (int)size, (int)ret);
		if(ret == 0) {
			// oob
			if(port->oob_size > 0) {
				char tag = (char)port->oob_tag;
				int copystatus;
				printk("-> oob %d\n", tag);
				if(copy_to_user(ptr, &tag, 1)) {
					ret = -EFAULT;
					break;
				}

				ptr++;
				ret++;
				size--;

				if(tag == TAG_SET_TERMIOS) {
					copystatus = kernel_termios_to_user_termios((struct termios2 __user*)ptr, &port->oob_data.termios);
					ret += sizeof(struct termios2);
				} else {
					copystatus = copy_to_user(ptr, &port->oob_data, port->oob_size);
					ret += port->oob_size;
				}

				if(copystatus) {
					ret = -EFAULT;
					break;
				}
				
				port->oob_size = 0;
				wake_up_interruptible_sync(&port->oob_wait);
				break;
			}
		}

		// normal data, or wait for events
		c = CIRC_CNT_TO_END(circ->head, circ->tail, VTTY_XMIT_SIZE);
		printk("-> circ=%d\n", c);

		if(c == 0) {
			if(ret > 0)
				break;

			if(filp->f_flags & O_NONBLOCK) {
				ret = -EAGAIN;
				break;
			}

			if (signal_pending(current)) {
				if (ret == 0)
					ret = -ERESTARTSYS;
				break;
			}

			prepare_to_wait(&port->read_wait, &wait, TASK_INTERRUPTIBLE);
			spin_unlock_irqrestore(&port->port.lock, flags);
			schedule();
			finish_wait(&port->read_wait, &wait);
			spin_lock_irqsave(&port->port.lock, flags);
			continue;
		}

		if(ret == 0) {
			// insert tag byte first
			char tag = TAG_UART_RX;
			if(copy_to_user(ptr, &tag, 1)) {
				ret = -EFAULT;
				break;
			}

			ret = 1;
			++ptr;
			--size;
		}
		
		if(c > size)
			c = size;

		if(copy_to_user(ptr, circ->buf + circ->tail, c)) {
			ret = -EFAULT;
			break;
		}

		ret += c;
		ptr += c;
		size -= c;
		circ->tail = (circ->tail + c) & (VTTY_XMIT_SIZE - 1);
	}

	spin_unlock_irqrestore(&port->port.lock, flags);

	mutex_lock(&portlock);
	if (vtty_circ_chars_pending(circ) < WAKEUP_CHARS) {
		if(port->tty) {
			printk("tty_wakeup\n");
			tty_wakeup(port->tty);
		} else
			printk("no tty_wakeup\n");
	}
	
	mutex_unlock(&portlock);
	
	return ret;
}

// pty write:
// tty_write
// 	do_tty_write
// 		n_tty_write <- blocking happens at this level
// 			if driver write (pty_write) returns 0
// 			regardless of throttle status
//
// 			so in theory, write could be possible after select(write) returns false?
//
// 			pty_write
// 				tty_insert_flip_string
//
// 			wait_woken
// 				(pty special case: woken via n_tty_check_unthrottle in n_tty_read)
// 				(woken via tty_wakeup, tty_write_unlock at the end of do_tty_write)
//
// tty_poll
// 	n_tty_poll
// 		marks POLLOUT if all of the following are true:
// 		! tty_is_writelocked (not interesting?)
// 		tty_chars_in_buffer < WAKEUP_CHARS (result varies by serial driver)
//			pty_chars_in_buffer returns 0
//			in non-pty cases, this state is signalled with tty_wakeup
// 		tty_write_room > 0 (result varies by serial driver) <= THIS
// 			pty_write_room returns tty_buffer_space_avail(link)
//
//
// in serial drivers, tty_wakeup is normally called by tx isr when tx queue drops below WAKEUP_CHARS
// in pty, tty_wakeup is called when the other side unthrottles
//
//
// podsumowanie:
// 	warstwa tty do pollout używa write_room > 0 && chars_in_buffer < WAKEUP_CHARS (tty_wakeup)
// 	pty używa jako write_room tty_buffer_space_avail(link)
// 		ta funkcja zwraca u nas 0 wcześniej niż bufory się faktycznie zatkają
// 		i sporo wcześniej niż zrobi się throttle
// 		dodatkowo ma hacka na to żeby unthrottle było wołane częściej niż zwykle i w ten sposób budzi waitqueue
//
// 	inne drivery serial *raczej* funkcjonują według chars_in_buffer i tty_wakeup
// 		serial: write_room == 0 -> chars_in_buffer == sizeof(buffer) > WAKEUP_CHARS
//
// my wpychamy dane do tty i callbacka dostajemy tylko w formie throttle/unthrottle
//
// wariant 1:
// 	poll() zwraca stan bitu throttle
// 	write() musi samo natychmiast ustawiać ten bit jeśli write() się nie powiedzie, inaczej select() będzie kłamał
// 		uwaga, problem z lockami
//
// wariant 2:
// 	poll zwraca tty_buffer_space_avail(), co jest trochę konserwatywne, ale ok
// 	na potrzeby budzenia musimy zrobić sztuczki z permanentnie ustawionym bitem throttle [jak pty]. czy to czegoś nie psuje?
//

static ssize_t vtmx_write (struct file *filp, const char __user *ptr, size_t size, loff_t *off)
{
	struct vtty_port *port = filp->private_data;
	char temp_buffer[256];
	ssize_t written = 0;
	int ret = 0;
	DEFINE_WAIT(wait);
	printk("enter vtmx_write (%d %d)\n", (int)size, (int)tty_buffer_space_avail(&port->port));

	while(size > 0) {
		size_t tmp_size = size;
		if(tmp_size > sizeof(temp_buffer))
			tmp_size = sizeof(temp_buffer);

		if (copy_from_user(temp_buffer, ptr, tmp_size)) {
			ret = -EFAULT;
			break;
		}

		if(tty_buffer_space_avail(&port->port) == 0)
			tmp_size = 0;

		if(0 && test_bit(TTY_THROTTLED, &port->tty->flags)) {
			// required for consistency with poll(), but even the tty layer doesn't follow this
			printk("vtmx write throttled\n");
			tmp_size = 0;
		} else {
			tmp_size = tty_insert_flip_string(&port->port, temp_buffer, tmp_size);
			if(tmp_size == 0 && 0) {
				// ldisc driver will throttle us soon, but not now
				// we must immediately mark the port as throttled
				// so that poll() works properly
				printk("vtmx write buffer space %d, self-throttling\n", tty_buffer_space_avail(&port->port));
				set_bit(TTY_THROTTLED, &port->tty->flags); // FIXME
			}
		}

		if(tmp_size == 0) {
			if(written > 0)
				break;

			if(filp->f_flags & O_NONBLOCK) {
				ret = -EAGAIN;
				break;
			}

			if (signal_pending(current)) {
				if (ret == 0)
					ret = -ERESTARTSYS;
				break;
			}

			prepare_to_wait(&port->write_wait, &wait, TASK_INTERRUPTIBLE);
//			spin_unlock_irqrestore(&port->port.lock, flags);
			schedule();
			finish_wait(&port->write_wait, &wait);
//			spin_lock_irqsave(&port->port.lock, flags);
			continue;
		}


		written += tmp_size;
		size -= tmp_size;
		ptr += tmp_size;
	}

	if(written > 0)
		tty_flip_buffer_push(&port->port);

	printk("leave vtmx_write %d %d %d\n", (int)written, (int)ret, (int)tty_buffer_space_avail(&port->port));
	if(ret)
		return ret;

	return written;
}

// FIXME POLLHUP ?
static unsigned int vtmx_poll(struct file *filp, poll_table *wait)
{
	unsigned int mask = 0;
	struct vtty_port *port = filp->private_data;
	struct circ_buf *circ = &port->xmit;
	unsigned long flags;

	poll_wait(filp, &port->read_wait, wait);
	poll_wait(filp, &port->write_wait, wait);

	spin_lock_irqsave(&port->port.lock, flags);
	// is there anything to read?
	if(CIRC_CNT_TO_END(circ->head, circ->tail, VTTY_XMIT_SIZE) > 0)
		mask |= POLLIN | POLLRDNORM;
	
	// any oob data?
	if(port->oob_size > 0) 
		mask |= POLLIN | POLLRDNORM | POLLPRI;

//	if(!test_bit(TTY_THROTTLED, &port->tty->flags)) 
	if(tty_buffer_space_avail(&port->port) > 0) 
		mask |= POLLOUT | POLLWRNORM;

	printk("poll() (%d, %d) return %x\n", (int)CIRC_CNT_TO_END(circ->head, circ->tail, VTTY_XMIT_SIZE), (int)port->oob_size, (int)mask);
	spin_unlock_irqrestore(&port->port.lock, flags);

	return mask;
}

static int vtty_modem_state_set(struct vtty_port *port, unsigned int __user *arg)
{
	unsigned long flags;
	int ret;
	unsigned int mstate;
	spin_lock_irqsave(&port->port.lock, flags);
	ret = get_user(mstate, arg);
	if(!ret) {
		// FIXME handle dcd change?
		wake_up_interruptible_sync(&port->mi_wait);
	}

	spin_unlock_irqrestore(&port->port.lock, flags);
	return ret;
}

static long vtmx_ioctl(struct file * filp, unsigned int cmd, unsigned long arg)
{
	struct vtty_port *port = filp->private_data;

	switch(cmd) {
	case VTMX_GET_VTTY_NUM: 
		return put_user(port - ports, (unsigned int __user *)arg);

	case VTMX_SET_MODEM_LINES:
		return vtty_modem_state_set(port, (unsigned int __user*)arg);
	}
	return -ENOIOCTLCMD;
}
/*
	ssize_t (*read) (struct file *, char __user *, size_t, loff_t *);
	ssize_t (*write) (struct file *, const char __user *, size_t, loff_t *);
	unsigned int (*poll) (struct file *, struct poll_table_struct *);
	long (*unlocked_ioctl) (struct file *, unsigned int, unsigned long);
*/
static struct file_operations vtmx_fops = {
	.owner 		= THIS_MODULE,
	.llseek		= no_llseek,
	.read		= vtmx_read,
	.write		= vtmx_write,
	.poll		= vtmx_poll,
	.unlocked_ioctl	= vtmx_ioctl,
	.open		= vtmx_open,
	.release	= vtmx_release,

};

static struct miscdevice vtmx_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "vtmx",
	.fops = &vtmx_fops
};



static int __init vtty_init(void)
{
	int ret = misc_register(&vtmx_miscdev);
	if(ret)
		goto fail;

	vtty_driver = tty_alloc_driver(VTTY_MAX,
			TTY_DRIVER_RESET_TERMIOS |
			TTY_DRIVER_REAL_RAW |
			TTY_DRIVER_DYNAMIC_DEV |
			TTY_DRIVER_HARDWARE_BREAK);

	if(!vtty_driver) {
		ret = -ENOMEM;
		goto fail_deregister;
	}

	vtty_driver->driver_name = "vtty";
	vtty_driver->name = "ttyV";
	vtty_driver->type = TTY_DRIVER_TYPE_SERIAL;
	vtty_driver->subtype = SERIAL_TYPE_NORMAL;
	vtty_driver->init_termios = tty_std_termios;

	tty_set_operations(vtty_driver, &vtty_ops);
	ret = tty_register_driver(vtty_driver);
	if(ret)
		goto fail_put;

	mutex_init(&portlock);

	return ret;

fail_put:
	put_tty_driver(vtty_driver);

fail_deregister:
	misc_deregister(&vtmx_miscdev);

fail:
	return ret;
}

static void __exit vtty_exit(void)
{
	mutex_destroy(&portlock);
	misc_deregister(&vtmx_miscdev);
}

module_init(vtty_init);
module_exit(vtty_exit);
MODULE_LICENSE("GPL");
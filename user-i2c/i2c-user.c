#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include "user-i2c.h"

/* one-hot encoding for fast checking multiple states */
enum i2c_user_states {
	I2C_IDL  = 1<<0,
	I2C_W4X  = 1<<1,
	I2C_W4R  = 1<<2,
	I2C_W4I  = 1<<3,
	I2C_W4WT = 1<<4,
	I2C_W4RT = 1<<5,
	I2C_NXT  = 1<<6,
	I2C_RDY  = 1<<7,
	I2C_ABT  = 1<<8,
	I2C_END  = 1<<9
};

struct i2c_user_bus {
	wait_queue_head_t wlist;
	struct i2c_adapter adapter;
	unsigned long functionality;
	enum i2c_user_states state;
	int nmsg;
	int imsg;
	struct i2c_msg *msg;
};

enum i2c_user_states check_i2c_transtyp(struct i2c_msg *msg)
{
	if (msg->flags & I2C_M_RD) {
		return I2C_W4RT;
	} else {
		return I2C_W4WT;
	}
}

static int i2c_user_xfer(struct i2c_adapter *adap, struct i2c_msg *msg, int n)
{
	/* send the message to the reader and wait for writer/reader to finish it */
	/* multiple msgs in one xfer only with ioctl method (it means repeated start condition, unless stop flag) */
	struct i2c_user_bus *ctx = adap->algo_data;
	int rc;
	enum i2c_user_states prevstate;
	rc = wait_event_interruptible(ctx->wlist, ctx->state & (I2C_IDL | I2C_W4R | I2C_W4I | I2C_END));
	if (rc) return rc;
	if (ctx->state == I2C_END) return -EIO;
	ctx->msg = msg;
	ctx->nmsg = n;
	ctx->imsg = 0;
	/* force the STOP in the last msg in this xfer */
	msg[n-1].flags |= I2C_M_STOP;
	switch ((prevstate=ctx->state)) {
		case I2C_W4R:
			ctx->nmsg = 1;
			/* fall through */
		case I2C_W4I:
			ctx->state = check_i2c_transtyp(ctx->msg);
			break;
		default:
			ctx->state = I2C_W4X;
	}
	wake_up(&ctx->wlist);
	rc = wait_event_interruptible(ctx->wlist, ctx->state & ( I2C_RDY | I2C_ABT | I2C_END));
	ctx->msg = NULL;
	if (rc) {
		ctx->state = prevstate;
		wake_up(&ctx->wlist);
		return rc;
	}
	if (ctx->state == I2C_END) return -EIO;
	rc = ctx->state == I2C_RDY ? ctx->imsg : -EIO;
	ctx->state = I2C_IDL;
	wake_up(&ctx->wlist);
	return rc;
}

static u32 i2c_user_func(struct i2c_adapter *adap)
{
	struct i2c_user_bus *ctx = adap->algo_data;
	return ctx->functionality;
}

static const struct i2c_algorithm i2c_user_algo = {
	.master_xfer	= i2c_user_xfer,
	.functionality	= i2c_user_func,
};

static int i2c_user_nrdev = 0;

static int i2c_user_open(struct inode *inode, struct file *file)
{
	/* create i2c bus instance, save the instance context in file* */
	struct i2c_adapter *adap;
	int rc;
	struct i2c_user_bus *ctx = kzalloc(sizeof(struct i2c_user_bus), GFP_KERNEL);
	if (!ctx) return -ENOMEM;
	init_waitqueue_head(&ctx->wlist);
	ctx->state = I2C_IDL;
	file->private_data = ctx;
	///////////////////////////////////////////
	adap = &ctx->adapter;
	i2c_set_adapdata(adap, ctx);
	adap->owner = THIS_MODULE;
	adap->class = I2C_CLASS_DEPRECATED;
	snprintf(adap->name, sizeof(adap->name), "i2c-user (#%d)", i2c_user_nrdev);
	adap->algo = &i2c_user_algo;
	adap->algo_data = ctx;
	/* what's my parent? inode? file? */
	//adap->dev.parent = &pdev->dev;

	rc = i2c_add_adapter(adap);
	if (rc)
		kzfree(ctx);
	else
		i2c_user_nrdev++;
	//////////////////////////////////////////
	/* default functionality, can override with ioctl */
	ctx->functionality = I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
	return rc;
}

/* using read/write, even a simple shell script could simulate a I2C bus... */
static ssize_t i2c_user_read(struct file *file, char __user *buf, size_t size, loff_t *off)
{
	/* block until i2c-xfer starts */
	struct i2c_user_bus *ctx = file->private_data;
	int len, rc;
	if (ctx->state == I2C_W4RT) {
		ctx->state = I2C_ABT;
		wake_up(&ctx->wlist);
		return -EIO;
	} else if (ctx->state == I2C_W4WT) {
		/* handle write transaction payload */
		len = ctx->msg[ctx->imsg].len;
		if (len>size) len=size;
		if (copy_to_user(buf, ctx->msg[ctx->imsg].buf, len) != len) {
			return -EFAULT;
		}
		ctx->msg[ctx->imsg].len = len;
		if (++ctx->imsg >= ctx->nmsg) {
			ctx->state = I2C_RDY;
		} else {
			ctx->state = I2C_NXT;
		}
		wake_up(&ctx->wlist);
		return len;
	} else {
		/* handle addr stage */
		struct {
			uint8_t addr;
			uint8_t len;
		} i2chdr;
		enum i2c_user_states prevstate;
		rc = wait_event_interruptible(ctx->wlist, ctx->state & (I2C_IDL | I2C_W4X | I2C_END));
		if (rc) return rc;
		if (ctx->state == I2C_END) return -EIO;
		if ((prevstate = ctx->state) == I2C_IDL) {
			ctx->state = I2C_W4R;
			wake_up(&ctx->wlist);
			rc = wait_event_interruptible(ctx->wlist, ctx->state & (I2C_W4RT | I2C_W4WT | I2C_END));
			if (rc) {
				ctx->state = prevstate;
				wake_up(&ctx->wlist);
				return rc;
			}
			if (ctx->state == I2C_END) return -EIO;
		} else {
			ctx->nmsg = 1;
			ctx->state = check_i2c_transtyp(ctx->msg);
			wake_up(&ctx->wlist);
		}
		i2chdr.addr = i2c_8bit_addr_from_msg(ctx->msg);
		i2chdr.len = ctx->msg->len;
		len = sizeof i2chdr;
		if (len>size) len=size;
		if (copy_to_user(buf, &i2chdr, len) != len) {
			return -EFAULT;
		}
		return len;
	}
	return 0;
}

/* write is always non-blocking */
static ssize_t i2c_user_write(struct file *file, const char __user *buf, size_t size, loff_t *off)
{
	/* send the received data in the blocked xfer and unblock it */
	struct i2c_user_bus *ctx = file->private_data;
	int len;
	if (ctx->state == I2C_W4WT) {
		ctx->state = I2C_ABT;
		wake_up(&ctx->wlist);
		return -EIO;
	} else if (ctx->state == I2C_W4RT) {
		/* handle read transaction payload */
		len = ctx->msg[ctx->imsg].len;
		if (len>size) len=size;
		
		if (copy_from_user(ctx->msg[ctx->imsg].buf, buf, len) != len) {
			return -EFAULT;
		}
		ctx->msg[ctx->imsg].len = len;
		if (++ctx->imsg >= ctx->nmsg) {
			ctx->state = I2C_RDY;
		} else {
			ctx->state = I2C_NXT;
		}
		wake_up(&ctx->wlist);
		return len;
	} else {
		/* the other states are not applicable for write() */
		return -EIO;
	}
	return 0;
}

static int i2c_user_close(struct inode *inode, struct file *file)
{
	/* terminate any i2c-xfer and destroy the i2c bus instance */
	struct i2c_user_bus *ctx = file->private_data;
	ctx->state = I2C_END;
	wake_up(&ctx->wlist);
	file->private_data = NULL;
	i2c_del_adapter(&ctx->adapter);
	i2c_user_nrdev--;
	kzfree(ctx);
	return 0;
}

static long i2c_user_xferioctl(struct i2c_user_bus *ctx, struct i2c_umsg __user *msg)
{
	enum i2c_user_states prevstate;
	unsigned long len;
	int rc = wait_event_interruptible(ctx->wlist, ctx->state & (I2C_IDL | I2C_W4X | I2C_NXT | I2C_END));
	if (rc) return rc;
	if (ctx->state == I2C_END) return -EIO;
	if ((prevstate = ctx->state) == I2C_IDL) {
		ctx->state = I2C_W4I;
		wake_up(&ctx->wlist);
		rc = wait_event_interruptible(ctx->wlist, ctx->state & (I2C_W4RT | I2C_W4WT | I2C_END));
		if (rc) {
			ctx->state = prevstate;
			wake_up(&ctx->wlist);
			return rc;
		}
		if (ctx->state == I2C_END) return -EIO;
	} else {
		ctx->state = check_i2c_transtyp(ctx->msg+ctx->imsg);
		wake_up(&ctx->wlist);
	}
	len = sizeof *msg;
	len -= copy_to_user(msg, ctx->msg+ctx->imsg, len);
	if (len) return -EFAULT; // need another state, so caller can re-try?
	return 0;
}

static long i2c_user_ioctl(struct file *file, unsigned num, long unsigned arg)
{
	long rc;
	struct i2c_user_bus *ctx = file->private_data;
	switch (num) {
		case UI2C_XFER:
			rc = i2c_user_xferioctl(ctx, (struct i2c_umsg __user *)arg);
			break;
		case UI2C_SET_FUNC:
			ctx->functionality = arg;
			rc = 0;
			break;
		default:
			return -ENOSYS; /* no such ioctl */
	}
	return rc;
}

static const struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = i2c_user_open,
	.llseek = no_llseek,
	.release = i2c_user_close,
	.read = i2c_user_read,
	.write = i2c_user_write,
	.unlocked_ioctl = i2c_user_ioctl
};

static struct miscdevice miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "i2c-user",
	.fops = &fops
};

static int __init i2c_user_init(void)
{
	/* the adapter will only be added when the user attaches to the char device */
	printk(KERN_INFO "starting user-i2c module\n");
	return misc_register(&miscdev);
}

static void __exit i2c_user_exit(void)
{
	misc_deregister(&miscdev);
}

module_init(i2c_user_init);
module_exit(i2c_user_exit);

MODULE_AUTHOR("Ronald Hoogenboom");
MODULE_DESCRIPTION("Usermode i2c bus driver");
MODULE_LICENSE("GPL v2");
/*
vim:path=.,/usr/src/kernels/5.6.13-100.fc30.x86_64/include,/usr/include,,
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/semaphore.h>
#include <linux/kfifo.h>
#include <linux/uaccess.h> /*copy to user, copy from user */
#include <linux/cdev.h>
#include <linux/device.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Carlos Bilbao");
MODULE_DESCRIPTION("Linux kernel module simulating a FIFO file via char file at /dev");

#define MAX_KBUF     	 4096
#define MAX_CBUFFER_LEN  4096
#define DEVICE_NAME      "fifodev" /* Dev name as it appears in /proc/devices */

/* FIFO Resources */
struct kfifo cbuffer;		/* Circular buffer (4096 bytes)*/
int prod_count = 0; 		/* # Threads that openned /proc for writting (producers) */
int cons_count = 0; 		/* # Threads that openned /proc for reading (consumers) */
struct semaphore mtx; 		/* Guarantees mutual exclusion at critical sections */
struct semaphore sem_prod;      /* Waiting queue for producers */
struct semaphore sem_cons; 	/* Waiting queue for consumers */
int nr_prod_waiting = 0;	/* # Producer threads waiting*/
int nr_cons_waiting = 0;	/* # Consumer threads waiting */

/* Device Resources */
dev_t start;			/* Starting (major,minor) pair for the driver */
struct cdev* chardev = NULL;    /* Cdev structure associated with the driver */
struct class *dev_class;        /* Device class */

//* --------------------- FIFO Functions --------------------- *//

/* Auxiliar function ~ home-made kernel-level cond_wait()*/
static int cond_wait(int *prod)
{
	struct semaphore *aux;
	int *counter;

	aux = (*prod == 0) ? &sem_cons : &sem_prod;
	counter = (*prod == 0) ? &nr_cons_waiting : &nr_prod_waiting;

	/* As cond_wait() : unlock, block thread at queue, lock */
        up(&mtx);

	if (down_interruptible(aux)){
                if(down_interruptible(&mtx));
                *counter = *counter - 1;
                up(&mtx);
                return -EINTR;
        }

	if (down_interruptible(&mtx)) return -EINTR;

	return 0;
}

/* Invoked when doing open() at /dev entry */
static int fifoproc_open(struct inode *in, struct file *f)
{
	int isProducer;

	/* lock */
	if (down_interruptible(&mtx)) return -EINTR;

	if (f->f_mode & FMODE_READ){ /* Consumidor */
		cons_count++;
		isProducer = 0;

		/* cond_signal(prod) */
		if (nr_prod_waiting > 0) {
			nr_prod_waiting--;
			up(&sem_prod);
		}

		while (prod_count == 0){
			nr_cons_waiting++;
			if (cond_wait(&isProducer)) return -EINTR;
		}
	}
	else { /* Productor */
		prod_count++;
		isProducer = 1;

		if (nr_cons_waiting > 0){
			nr_cons_waiting--;
			up(&sem_cons);
		}

		while (cons_count == 0){
			nr_prod_waiting++;
			if (cond_wait(&isProducer)) return -EINTR;
		}
	}

	/* unlock */
	up(&mtx);

 return 0;
}

/* Invoked when doing close() at /dev entry */
static int fifoproc_release(struct inode *i, struct file *f)
{
	/* lock */
	if (down_interruptible(&mtx)) return -EINTR;

	if (f->f_mode & FMODE_READ){ /* Consumidor */
		cons_count--;

		if (nr_prod_waiting > 0){ /* Evita errores EPIPE */
			nr_prod_waiting--;
			up(&sem_prod);
		}
	}
	else { /* Productor */
		prod_count--;

		if (nr_cons_waiting > 0){
			nr_cons_waiting--;
			up(&sem_cons);
		}
	}

	if (prod_count == 0 && cons_count == 0) kfifo_reset(&cbuffer);
	up(&mtx);

	return 0;
}

/* Invoked when doing read() at /dev entry */
static ssize_t fifoproc_read(struct file *f, char *buff, size_t size, loff_t *l)
{
	char kbuffer[MAX_KBUF];
	int isProducer = 0, len;

	/* lock */
	if (down_interruptible(&mtx)) return -EINTR;

	/* Wants to read more than it is available 
	   Spectre: array_index_nospec()
	*/
	while (kfifo_len(&cbuffer) == 0 && prod_count > 0){
		nr_cons_waiting++;
		if(cond_wait(&isProducer)) return -EINTR;
	}

	if (kfifo_is_empty(&cbuffer)){
		up(&mtx);
		return 0;
	}

	len = (size >= kfifo_len(&cbuffer))? kfifo_len(&cbuffer) : size;

	len = kfifo_out(&cbuffer, kbuffer, len);

	if (nr_prod_waiting > 0){
		--nr_prod_waiting;
		up(&sem_prod);
	}

	/*unlock */
	up(&mtx);

	if (copy_to_user(buff,kbuffer,len)) return -ENOMEM;

 return len;
}

/* Invoked when doing write() at /dev entry */
static ssize_t fifoproc_write(struct file *f,const char *buff, size_t size, loff_t *l)
{
	char kbuffer[MAX_KBUF];
	int isProducer = 1;

	if (size > MAX_KBUF) return -ENOMEM;

	if (copy_from_user(kbuffer, buff,size)) return -ENOMEM;

	/* lock */
	if (down_interruptible(&mtx)) return -EINTR;

	while (kfifo_avail(&cbuffer) < size && cons_count > 0){
		nr_prod_waiting++;
		if(cond_wait(&isProducer)) return -EINTR;
	}

	kfifo_in(&cbuffer, kbuffer,size);

	if (cons_count == 0) {
		up(&mtx);
		return -EPIPE;
	}

	if (nr_cons_waiting > 0){
                --nr_cons_waiting;
                up(&sem_cons);
        }

	up(&mtx);

  return size;
}

/* Operations my module can handle.
 Don't use echo or cat, etc, use fifotest.c !! */
const struct file_operations fops = {
	.read    =    fifoproc_read,
	.open    =    fifoproc_open,
	.write   =    fifoproc_write,
	.release =    fifoproc_release,
};

int modulo_fifo_init(void)
{
	int ret = 0, major, minor; /* major associated with device driver, minor with character device */

	if ((ret = alloc_chrdev_region(&start, 0, 1, DEVICE_NAME)) || (kfifo_alloc(&cbuffer, MAX_CBUFFER_LEN, GFP_KERNEL))) {
		ret = -ENOMEM;
		printk(KERN_INFO "Couldn't create the /dev entry \n");
	}
	else {
		if((chardev = cdev_alloc()) == NULL) return -ENOMEM;

		cdev_init(chardev,&fops);

		if((ret = cdev_add(chardev,start,1))) return -ENOMEM;

		major = MAJOR(start);
		minor = MINOR(start);

		/* Initialize stuff*/
		sema_init(&mtx,1); /* As mutex */
		sema_init(&sem_prod, 0); /* As waiting queues */
		sema_init(&sem_cons, 0);

		/* Create device class */
		dev_class = class_create(THIS_MODULE, DEVICE_NAME);
		if (IS_ERR(dev_class)) {
			printk(KERN_INFO "Failed to create device class\n");
			return PTR_ERR(dev_class);
		}

		/* Create device file */
		if (device_create(dev_class, NULL, start, NULL, DEVICE_NAME) == NULL) {
			printk(KERN_INFO "Failed to create device file\n");
			class_destroy(dev_class);
			return -ENOMEM;
		}

		printk(KERN_INFO "Module %s charged: major = %d, minor = %d\n", DEVICE_NAME,major,minor);
	}

	return ret;
}

void modulo_fifo_exit(void) 
{
	if (chardev) cdev_del(chardev);

	/* Unregister the device */
	unregister_chrdev_region(start, 1);

	/* Free the memory */
	kfifo_free(&cbuffer);

	/* Destroy device file and class */
	device_destroy(dev_class, start);
	class_destroy(dev_class);

   	printk(KERN_INFO "Module %s disconnected \n", DEVICE_NAME);
}

module_init(modulo_fifo_init);
module_exit(modulo_fifo_exit);

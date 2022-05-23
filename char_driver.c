#include <linux/module.h>	/* for modules */
#include <linux/fs.h>		/* file_operations */
#include <linux/uaccess.h>	/* copy_(to,from)_user */
#include <linux/init.h>	/* module_init, module_exit */
#include <linux/slab.h>	/* kmalloc */
#include <linux/cdev.h>	/* cdev utilities */
#include <linux/moduleparam.h> /* moduleparam */

#define MYDEV_NAME "mycdrv"

#define ramdisk_size (size_t) (16 * PAGE_SIZE) // ramdisk size 

//NUM_DEVICES defaults to 3 unless specified during insmod
static int NUM_DEVICES = 3;

#define CDRV_IOC_MAGIC 'Z'
#define ASP_CLEAR_BUF _IOW(CDRV_IOC_MAGIC, 1, int)

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct ASP_mycdrv {
	struct cdev cdev;
	char* ramdisk;
	struct semaphore sem;
	int devNo;
	size_t ram_size;
	size_t end_of_buf;
	int count;
}mycdrv_obj;

// define major number
static int major_no = 500;
// minor number defined later per device

// functions for MODULE of the driver
// Using ScullBasic and DDIntro driver slides

static int mycdrv_open(struct inode *inode, struct file *file)
{
	mycdrv_obj* ptr;
	// load the file pointer data to the device identified by inode
	ptr = container_of(inode->i_cdev, struct ASP_mycdrv, cdev);
	file->private_data = ptr;

	// synchronization using semaphore
	// down_interruptible is the sem_wait version for a kernel module
	if(down_interruptible(&ptr->sem))
		return -ERESTARTSYS;
	// access resource and increment count
	ptr->count++;
	up(&ptr->sem);  // kernel equivalent to sem_post

	pr_info("OPENED device: %s:\n\n", MYDEV_NAME);
	return 0;
}

static int mycdrv_release(struct inode *inode, struct file *file)
{
	mycdrv_obj* ptr;
	ptr = (mycdrv_obj*)file->private_data;

	// similar to mycdrv_open
	// only decrement the count when protected by a semaphore
	if(down_interruptible(&ptr->sem))
		return -ERESTARTSYS;
	ptr->count--;
	up(&ptr->sem);

	pr_info("CLOSED device: %s:\n\n", MYDEV_NAME);
	return 0;
}

static ssize_t mycdrv_read(struct file *file, char __user * buf, size_t lbuf, loff_t * ppos)
{
	mycdrv_obj* ptr;
	int nbytes = 0;
	
	ptr = (mycdrv_obj*)file->private_data;
	// protect further operation with a semaphore
	if(down_interruptible(&ptr->sem))
		return -ERESTARTSYS;

	if ((lbuf + *ppos) > ptr->ram_size) {
		pr_info("trying to read past end of device,"
			"aborting because this is just a stub!\n");
		up(&ptr->sem);
		return 0;
	}

	nbytes = lbuf - copy_to_user(buf, ptr->ramdisk + *ppos, lbuf);
	*ppos += nbytes;
	
	up(&ptr->sem); // release semaphore

	pr_info("READING function, nbytes=%d, pos=%d\n", nbytes, (int)*ppos);
	return nbytes;
}

static ssize_t mycdrv_write(struct file *file, const char __user * buf, size_t lbuf, loff_t * ppos)
{
	mycdrv_obj* ptr;
	int nbytes = 0;
	
	// initialization similar to mycdrv_read
	ptr = (mycdrv_obj*)file->private_data;
	if(down_interruptible(&ptr->sem))
		return -ERESTARTSYS;

	if ((lbuf + *ppos) > ptr->ram_size) {
		pr_info("trying to read past end of device,"
			"aborting because this is just a stub!\n");
		return 0;
	}

	// change in write() is reversing source and destination in copy_to_user function
	nbytes = lbuf - copy_from_user(ptr->ramdisk + *ppos, buf, lbuf);
	*ppos += nbytes;
	
	// if end of buffer is greater than ppos, truncate it to ppos
	if(ptr->end_of_buf < *ppos) {
		ptr->end_of_buf = *ppos;
	}
	
	up(&ptr->sem); // releasing semaphore

	pr_info("WRITING function, nbytes=%d, pos=%d\n", nbytes, (int)*ppos);
	return nbytes;
}

static loff_t mycdrv_lseek(struct file *file, loff_t offset, int orig)
{
	mycdrv_obj* ptr;
	size_t size_original, size_new, idx;
	char* ramdisk_new;

	ptr = (mycdrv_obj*)file->private_data;
	if(down_interruptible(&ptr->sem))
		return -ERESTARTSYS;
	
	switch(orig)
	{
		// points to beginning of file
		case SEEK_SET:
			file->f_pos = offset;
			break;

		// current file offset position
		case SEEK_CUR:
			file->f_pos += offset;
			break;

		// points to end of file
		case SEEK_END:
			pr_info("Reallocating device size\n");
			file->f_pos = ptr->end_of_buf + offset;

			size_original = ptr->ram_size;
			size_new = ptr->ram_size + (offset * sizeof(char));

			// allocate extra memory obtained from offset value
			if ((ramdisk_new = (char*)kzalloc(size_new, GFP_KERNEL)) != NULL) {
				// load data from ramdisk to new buffer
				for (idx=0; idx<size_original; idx++)
					ramdisk_new[idx] = ptr->ramdisk[idx];

				// deallocate the memory of ramdisk
				kfree(ptr->ramdisk);

				// load new ramdisk and size to existing variables
				ptr->ramdisk = ramdisk_new;
				ptr->ram_size = size_new;
			}
			
			else {
				pr_info("Could not reallocate memory\n");}
			break;

		default:
			pr_info("Please enter valid offset/position\n");
			break;
	}
	
	// if negative offset is obtained, make it zero
	file->f_pos = file->f_pos >=0 ? file->f_pos : 0;
		
	up(&ptr->sem);
	return 0;
}

static long mycdrv_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	mycdrv_obj* ptr;
	ptr = (mycdrv_obj*)file->private_data;

	switch(cmd) {
		case ASP_CLEAR_BUF:
			pr_info("Clearing buffer\n");
			// lock the region before clearing
			if(down_interruptible(&ptr->sem))
				return -ERESTARTSYS;
			memset((void *)ptr->ramdisk, 0, ptr->ram_size); // resetting upto ram_size in buffer to zero
			ptr->end_of_buf = 0; // setting end of buffer to zero
			file->f_pos = 0; // reset file pointer back to zero
			up(&ptr->sem); // releasing the lock on region
			break;
		
		default:
			// Handling error conditions
			return -1;
	}
	return 0;
}

static const struct file_operations mycdrv_fops = {
	.owner = THIS_MODULE,
	.read = mycdrv_read,
	.write = mycdrv_write,
	.open = mycdrv_open,
	.release = mycdrv_release,
	.llseek = mycdrv_lseek,
	.unlocked_ioctl = mycdrv_ioctl,
};

// defining global variables for init and exit functions
static dev_t first; // variable that identifies each device using both major and minor number
struct class* dev_cls;
mycdrv_obj* device; 

static int __init my_init(void)
{
	unsigned int minor_no = 0; // assigning a variable to handle minor number since major number is handled globally
	int j; // has to be declared initially only 
	
	if(alloc_chrdev_region(&first, minor_no, NUM_DEVICES, MYDEV_NAME) != 0) {
		pr_info("Error: create dynamic major number\n");
		return -1;
	}
	major_no = MAJOR(first); 
	
	if((dev_cls = class_create(THIS_MODULE, MYDEV_NAME)) == NULL) {
		pr_info("Error: creating class\n");
		return -1;
	}

	device = (mycdrv_obj*)kzalloc(NUM_DEVICES * sizeof(mycdrv_obj), GFP_KERNEL);
	
	// loop over all devices for creating nodes and allocating disk space
	for (j=0; j< NUM_DEVICES; j++) {
		dev_t dev_num = MKDEV(major_no, j); // MKDEV uses major and minor number combination to uniquely identify a device
		mycdrv_obj* dptr = &device[j];
		dptr->cdev.owner = THIS_MODULE;
		cdev_init(&dptr->cdev, &mycdrv_fops);
		dptr->ram_size = ramdisk_size;
		dptr->devNo = j; // assign minor number to each device
		dptr->count = 0; // init count to zero
		dptr->end_of_buf = 0;
		dptr->ramdisk = (char*)kzalloc(ramdisk_size, GFP_KERNEL); // allocate memory to each device
		cdev_add(&dptr->cdev, dev_num, 1); // device is live
		// initialize semaphore with value 1
		sema_init(&dptr->sem, 1); 
		device_create(dev_cls, NULL, dev_num, NULL, MYDEV_NAME "%d", j);
		pr_info("Succeeded in registering character device %s%d\n", MYDEV_NAME, j);
	}
	return 0;
}

static void __exit my_exit(void)
{
	int k;
	for (k=0; k<NUM_DEVICES; k++) {
		mycdrv_obj* dptr = &device[k];
		cdev_del(&dptr->cdev);
		kfree(dptr->ramdisk); // deallocate disk space for device n
		device_destroy(dev_cls, MKDEV(major_no, k));
		pr_info("device node %d removed\n", k);
	}
	kfree(device); // deallocate devices
	class_destroy(dev_cls); // remove class
	unregister_chrdev_region(first, NUM_DEVICES);
	pr_info("devices unregistered\n");
}

module_init(my_init);
module_exit(my_exit);
module_param(NUM_DEVICES, int, S_IRUGO);

MODULE_AUTHOR("Sai Bhargav Mandavilli");
MODULE_DESCRIPTION("Character device driver");
MODULE_ALIAS("char_driver");
MODULE_LICENSE("GPL v2");

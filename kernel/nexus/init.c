/* init.c - startup/shutdown, device constructor/destructor, refcounting */

/* 
 * Nexus - convergently encrypting virtual disk driver for the OpenISR (R)
 *         system
 * 
 * Copyright (C) 2006-2007 Carnegie Mellon University
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as published
 * by the Free Software Foundation.  A copy of the GNU General Public License
 * should have been distributed along with this program in the file
 * LICENSE.GPL.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/mm.h>
#include <linux/device.h>
#include <linux/wait.h>
#include <linux/interrupt.h>
#include "defs.h"

static struct class *class;
int blk_major;

/**
 * struct state - singleton tracking global, modifiable state
 * @lock       : lock for the other fields
 * @devs       : list of non-shutdown &nexus_dev (for nexus_open())
 * @all_devs   : list of all &nexus_dev (for ident uniqueness)
 * @devnums    : bitmap of allocated device numbers (for minornum allocation)
 * @cache_pages: count of &struct page allocated to all &nexus_dev caches
 **/
static struct state {
	spinlock_t lock;
	struct list_head devs;
	struct list_head all_devs;
	DECLARE_BITMAP(devnums, DEVICES);
	unsigned long cache_pages;
} state;

#ifdef DEBUG
unsigned debug_mask;
module_param(debug_mask, int, 0);
MODULE_PARM_DESC(debug_mask, "initial bitmask for debug messages");
#endif

/**
 * nexus_dev_get - increment the device refcount
 * 
 * It is an error to use this when the refcount may have already reached
 * zero.  (The kref infrastructure does not handle this case.)
 **/
void nexus_dev_get(struct nexus_dev *dev)
{
	debug(DBG_REFCOUNT, "dev_get, refs %d",
			atomic_read(&dev->kdevice->kobj.kref.refcount));
	if (kdevice_get(dev->kdevice) == NULL)
		BUG();
}

/**
 * nexus_dev_put - decrement the device refcount
 * @unlink: true if we should remove the sysfs entries
 * 
 * This function must be called with @unlink true exactly once per device.
 * The dev lock MUST NOT be held.
 **/
void nexus_dev_put(struct nexus_dev *dev, int unlink)
{
	debug(DBG_REFCOUNT, "dev_put, refs %d, unlink %d",
			atomic_read(&dev->kdevice->kobj.kref.refcount),
			unlink);
	BUG_ON(in_atomic());
	if (unlink)
		kdevice_unregister(dev->kdevice);
	else
		kdevice_put(dev->kdevice);
}

/**
 * user_get - increment the count of active blockdev users
 **/
void user_get(struct nexus_dev *dev)
{
	BUG_ON(!mutex_is_locked(&dev->lock));
	dev->need_user++;
	debug(DBG_REFCOUNT, "need_user now %u", dev->need_user);
}

/**
 * user_put - decrement the count of active blockdev users
 **/
void user_put(struct nexus_dev *dev)
{
	BUG_ON(!mutex_is_locked(&dev->lock));
	if (!--dev->need_user)
		wake_up_interruptible(&dev->waiting_users);
	debug(DBG_REFCOUNT, "need_user now %u", dev->need_user);
}

/**
 * shutdown_dev - disconnect the chardev from its blockdev
 * @force: if false, fail if the blockdev has active users
 **/
int shutdown_dev(struct nexus_dev *dev, int force)
{
	BUG_ON(!mutex_is_locked(&dev->lock));
	if (dev_is_shutdown(dev))
		return -ENXIO;
	if (!force && dev->need_user != 0) {
		return -EBUSY;
	}
	debug(DBG_CTR|DBG_CHARDEV, "Shutting down chardev");
	spin_lock(&state.lock);
	list_del_init(&dev->lh_devs);
	spin_unlock(&state.lock);
	shutdown_usermsg(dev);
	return 0;
}

/**
 * nexus_sync - block until the Nexus device is idle
 *
 * For use by the "action" sysfs attribute.  Blocks until the chunkdata dev
 * reference is no longer held and the requests queue is empty, which means
 * that no chunks are in I/O or otherwise queued for processing.  (Of course,
 * as soon as this function returns, another chunk may be queued for I/O.)
 * Note that we wait for all pending activity to finish -- both reads and
 * writes.
 * 
 * Returns -ERESTARTSYS if a signal is received while waiting.
 **/
int nexus_sync(struct nexus_dev *dev)
{
	DEFINE_WAIT(wait);
	int do_sched;
	
	spin_lock_bh(&dev->requests_lock);
	while (test_bit(__DEV_HAVE_CD_REF, &dev->flags) ||
				!list_empty(&dev->requests)) {
		prepare_to_wait(&dev->waiting_idle, &wait, TASK_INTERRUPTIBLE);
		do_sched = test_bit(__DEV_HAVE_CD_REF, &dev->flags) ||
					!list_empty(&dev->requests);
		spin_unlock_bh(&dev->requests_lock);
		if (do_sched)
			schedule();
		finish_wait(&dev->waiting_idle, &wait);
		if (signal_pending(current))
			return -ERESTARTSYS;
		spin_lock_bh(&dev->requests_lock);
	}
	spin_unlock_bh(&dev->requests_lock);
	return 0;
}

/**
 * class_populate - add standard attributes to our class
 * 
 * On failure, the class may be semi-populated, but that will be cleaned up
 * when it is deleted.  All attributes will be deleted on unregistration.
 **/
static int class_populate(void)
{
	int i;
	int err;
	
	for (i=0; class_attrs[i].attr.name != NULL; i++) {
		err=class_create_file(class, &class_attrs[i]);
		if (err)
			return err;
	}
	return 0;
}

/**
 * kdevice_populate - add standard attributes to a kdevice
 * 
 * On failure, the device may be semi-populated, but that will be cleaned up
 * when the device is deleted.  All attributes will be deleted when the
 * kdevice is unregistered.
 **/
static int kdevice_populate(kdevice_t *kdevice)
{
	int i;
	int err;
	
	for (i=0; kdevice_attrs[i].attr.name != NULL; i++) {
		err=kdevice_create_file(kdevice, &kdevice_attrs[i]);
		if (err)
			return err;
	}
	return 0;
}

/**
 * get_system_page_count - return the total number of memory pages
 **/
static unsigned long get_system_page_count(void)
{
	struct sysinfo s;
	
	si_meminfo(&s);
	BUG_ON(s.mem_unit != PAGE_SIZE);
	return s.totalram;
}

/**
 * ident_exists - return true if some Nexus device has the given @ident
 *
 * state.lock must be held.
 **/
static int ident_exists(char *ident)
{
	struct nexus_dev *dev;
	
	list_for_each_entry(dev, &state.all_devs, lh_all_devs) {
		if (!strcmp(ident, dev->ident))
			return 1;
	}
	return 0;
}

/**
 * nexus_open - handle open() of a Nexus block device node
 *
 * We fail the request if the corresponding character device has already been
 * shut down.
 **/
static block_open_method(nexus_open, handle)
{
	struct nexus_dev *dev;
	int found=0;
	
	spin_lock(&state.lock);
	list_for_each_entry(dev, &state.devs, lh_devs) {
		if (dev == block_open_get_dev(handle)) {
			found=1;
			/* Since it's still in the devs list, we know that
			   chardev still holds a reference. */
			nexus_dev_get(dev);
			break;
		}
	}
	spin_unlock(&state.lock);
	if (!found)
		return -ENODEV;
	
	debug(DBG_CTR, "nexus_open");
	if (mutex_lock_interruptible(&dev->lock)) {
		nexus_dev_put(dev, 0);
		return -ERESTARTSYS;
	}
	user_get(dev);
	mutex_unlock(&dev->lock);
	return 0;
}

/**
 * nexus_release - handle close() of a Nexus block device node
 *
 * The kernel guarantees that calls to nexus_release() will always be paired
 * with calls to nexus_open().
 **/
static block_release_method(nexus_release, handle)
{
	struct nexus_dev *dev=block_release_get_dev(handle);
	
	debug(DBG_CTR, "nexus_release");
	/* Our return value is ignored, so we must use the uninterruptible
	   variant */
	mutex_lock(&dev->lock);
	user_put(dev);
	mutex_unlock(&dev->lock);
	nexus_dev_put(dev, 0);
	return 0;
}

/**
 * alloc_devnum - allocate a device number from the pool
 **/
static int alloc_devnum(void)
{
	int num;

	spin_lock(&state.lock);
	num=find_first_zero_bit(state.devnums, DEVICES);
	if (num != DEVICES)
		__set_bit(num, state.devnums);
	spin_unlock(&state.lock);
	if (num == DEVICES)
		return -1;
	else
		return num;
}

/**
 * free_devnum - free a previously-allocated device number
 **/
static void free_devnum(int devnum)
{
	spin_lock(&state.lock);
	__clear_bit(devnum, state.devnums);
	spin_unlock(&state.lock);
}

/**
 * nexus_add_disk - register a newly-created device with the block layer
 * 
 * We have to call add_disk() from a workqueue callback in order to prevent
 * deadlock.  This is the function that does so.  The function which sets
 * up the callback must first grab a dev reference, which nexus_add_disk()
 * will put after the add_disk().
 **/
static void nexus_add_disk(work_t *work_struct)
{
	struct nexus_dev *dev=container_of(work_struct, struct nexus_dev,
				cb_add_disk);
	
	debug(DBG_CTR, "Adding gendisk");
	add_disk(dev->gendisk);
	nexus_dev_put(dev, 0);
}

/**
 * nexus_open_bdev - bind a block device to a &nexus_dev given its path
 * 
 * open_bdev_exclusive() doesn't check permissions on the device node it's
 * opening, so we have to do it ourselves, here.  In order to prevent a
 * symlink attack, we save the dev_t from the permission check and verify that
 * the device node we eventually open matches that value.
 **/
static struct block_device *nexus_open_bdev(struct nexus_dev *dev,
			char *devpath)
{
	struct block_device *bdev;
	struct nameidata nd;
	struct inode *inode;
	dev_t devt;
	int err;
	
	/* path_lookup() is apparently willing to look up a zero-length
	   path.  open_bdev_exclusive() would catch this later, but that
	   doesn't seem like a good idea. */
	if (devpath[0] == 0) {
		err=-EINVAL;
		goto bad;
	}
	err=path_lookup(devpath, LOOKUP_FOLLOW, &nd);
	if (err)
		goto bad;
	inode=nd_path_dentry(nd)->d_inode;
	err=check_inode_permission(inode, MAY_READ|MAY_WRITE, &nd);
	if (err)
		goto bad_release;
	/* Prevent symlink attack from char device to block device */
	if (!S_ISBLK(inode->i_mode)) {
		err=-ENOTBLK;
		goto bad_release;
	}
	devt=inode->i_rdev;
	path_release(&nd);
	
	bdev=open_bdev_exclusive(devpath, FMODE_READ|FMODE_WRITE, dev);
	if (IS_ERR(bdev)) {
		err=PTR_ERR(bdev);
		goto bad;
	}
	if (bdev->bd_dev != devt) {
		/* The device node at the given path changed between the
		   permission check and the open_bdev_exclusive().  We could
		   loop, but it's probably better to just fail. */
		err=-EAGAIN;
		goto bad_close;
	}
	return bdev;
	
bad:
	return ERR_PTR(err);
bad_release:
	path_release(&nd);
	goto bad;
bad_close:
	close_bdev_exclusive(bdev, FMODE_READ|FMODE_WRITE);
	goto bad;
}

/**
 * nexus_dev_dtr - tear down an existing Nexus device
 *
 * This is called by the release callback of the &kdevice_t embedded in
 * &struct nexus_dev, when the device's reference count goes to zero.  It
 * must be able to handle the case that the device was not fully initialized,
 * if the constructor errored out after refcounting was set up.
 **/
static void nexus_dev_dtr(kdevice_t *kdevice)
{
	struct nexus_dev *dev=kdevice_get_data(kdevice);
	
	debug(DBG_CTR, "Dtr called");
	BUG_ON(!dev_is_shutdown(dev));
	BUG_ON(!list_empty(&dev->requests));
	if (dev->gendisk) {
		if (dev->gendisk->flags & GENHD_FL_UP) {
			del_gendisk(dev->gendisk);
		} else {
			/* Disk was created but not yet added */
			put_disk(dev->gendisk);
		}
	}
	chunkdata_free_table(dev);
	thread_unregister(dev);
	if (dev->queue)
		blk_cleanup_queue(dev->queue);
	if (dev->chunk_bdev)
		close_bdev_exclusive(dev->chunk_bdev, FMODE_READ|FMODE_WRITE);
	spin_lock(&state.lock);
	list_del(&dev->lh_all_devs);
	state.cache_pages -= dev->cachesize * chunk_pages(dev);
	spin_unlock(&state.lock);
	free_devnum(dev->devnum);
	kfree(dev->kdevice);
	kfree(dev->ident);
	kfree(dev);
	module_put(THIS_MODULE);
}

static struct block_device_operations nexus_ops = {
	.owner =	THIS_MODULE,
	.open =		nexus_open,
	.release =	nexus_release,
};

/**
 * nexus_dev_ctr - create and initialize a new Nexus device
 *
 * This is called by the character device when it receives a request to
 * create a new block device.  Validation of the content of the parameters
 * is the responsibility of this function, but validation of their form (e.g.,
 * strings being null-terminated) is the responsibility of the caller.
 **/
struct nexus_dev *nexus_dev_ctr(char *ident, char *devnode, unsigned chunksize,
			unsigned cachesize, sector_t offset,
			enum nexus_crypto suite,
			enum nexus_compress default_compress,
			compressmask_t supported_compress)
{
	struct nexus_dev *dev;
	sector_t capacity;
	unsigned long pages=get_system_page_count();
	int devnum;
	int ret;
	int ok;
	
	debug(DBG_CTR, "Ctr starting");
	
	/* If the userspace process goes away right after the ctr returns, the
	   device will still exist until delayed_add_disk runs but the module
	   could be unloaded.  To get around this, we get an extra reference
	   to the module here and put it in the dtr. */
	if (!try_module_get(THIS_MODULE)) {
		ret=-ENOPKG;
		goto early_fail_module;
	}
	
	debug(DBG_CTR, "Allocating devnum");
	devnum=alloc_devnum();
	if (devnum < 0) {
		ret=-EMFILE;
		goto early_fail_devnum;
	}
	
	debug(DBG_CTR, "Allocating device struct");
	dev=kzalloc(sizeof(*dev), GFP_KERNEL);
	if (dev == NULL) {
		ret=-ENOMEM;
		goto early_fail_devalloc;
	}
	
	debug(DBG_CTR, "Allocating kdevice");
	dev->kdevice=kdevice_create(class, DEVICE_NAME "%c", 'a' + devnum);
	if (IS_ERR(dev->kdevice)) {
		ret=PTR_ERR(dev->kdevice);
		goto early_fail_kdevice;
	}
	dev->kdevice->release=nexus_dev_dtr;
	kdevice_set_data(dev->kdevice, dev);
	
	/* Now we have refcounting, so all further errors should deallocate
	   through the destructor */
	mutex_init(&dev->lock);
	INIT_LIST_HEAD(&dev->requests);
	spin_lock_init(&dev->requests_lock);
	setup_timer(&dev->requests_oom_timer, oom_timer_fn, (unsigned long)dev);
	INIT_LIST_HEAD(&dev->lh_run_requests);
	INIT_LIST_HEAD(&dev->lh_devs);
	INIT_LIST_HEAD(&dev->lh_all_devs);
	init_waitqueue_head(&dev->waiting_users);
	init_waitqueue_head(&dev->waiting_idle);
	dev->devnum=devnum;
	dev->owner=current->uid;
	
	if (strchr(ident, '/')) {
		log(KERN_ERR, "device identifier may not contain '/'");
		ret=-EINVAL;
		goto bad;
	}
	dev->ident=kstrdup(ident, GFP_KERNEL);
	if (dev->ident == NULL) {
		log(KERN_ERR, "couldn't allocate identifier string");
		ret=-ENOMEM;
		goto bad;
	}
	
	spin_lock(&state.lock);
	if (ident_exists(ident)) {
		spin_unlock(&state.lock);
		log(KERN_ERR, "specified identifier already exists");
		ret=-EEXIST;
		goto bad;
	}
	list_add_tail(&dev->lh_all_devs, &state.all_devs);
	list_add_tail(&dev->lh_devs, &state.devs);
	spin_unlock(&state.lock);
	
	if (chunksize < 512 || (chunksize & (chunksize - 1)) != 0) {
		log(KERN_ERR, "chunk size must be >= 512 and a power of 2");
		ret=-EINVAL;
		goto bad;
	}
	if (chunksize > MAX_CHUNKSIZE) {
		log(KERN_ERR, "chunk size exceeds configured maximum of %d",
					MAX_CHUNKSIZE);
		ret=-EINVAL;
		goto bad;
	}
	dev->chunksize=chunksize;
	if (cachesize < MIN_CONCURRENT_REQS * MAX_CHUNKS_PER_IO) {
		log(KERN_ERR, "cache size may not be smaller than %u",
				MIN_CONCURRENT_REQS * MAX_CHUNKS_PER_IO);
		ret=-EINVAL;
		goto bad;
	}
	/* Avoid arithmetic overflow */
	if (cachesize > pages / chunk_pages(dev) * MAX_DEV_ALLOCATION_MULT /
				MAX_DEV_ALLOCATION_DIV) {
		log(KERN_ERR, "cache size may not be larger than %u/%u of "
					"system RAM", MAX_DEV_ALLOCATION_MULT,
					MAX_DEV_ALLOCATION_DIV);
		/* Abuse of return code, but userspace needs to be able to
		   distinguish this case */
		ret=-ENOSPC;
		goto bad;
	}
	dev->cachesize=cachesize;
	spin_lock(&state.lock);
	/* The dtr subtracts this off again only if chunksize and cachesize
	   are nonzero */
	state.cache_pages += cachesize * chunk_pages(dev);
	ok=(state.cache_pages <= pages * MAX_ALLOCATION_MULT
				/ MAX_ALLOCATION_DIV);
	spin_unlock(&state.lock);
	/* This is racy wrt multiple simultaneous ctr calls, but it's
	   conservatively racy: multiple ctr calls may fail when one of them
	   should have succeeded */
	if (!ok) {
		log(KERN_ERR, "will not allocate more than %u/%u of system RAM "
					"for cache", MAX_ALLOCATION_MULT,
					MAX_ALLOCATION_DIV);
		/* Abuse of return code, but userspace needs to be able to
		   distinguish this case */
		ret=-ENOSPC;
		goto bad;
	}
	debug(DBG_CTR, "chunksize %u, cachesize %u, backdev %s, offset "
				SECTOR_FORMAT, chunksize, cachesize, devnode,
				offset);
	
	debug(DBG_CTR, "Opening %s", devnode);
	dev->chunk_bdev=nexus_open_bdev(dev, devnode);
	if (IS_ERR(dev->chunk_bdev)) {
		log(KERN_ERR, "couldn't open %s", devnode);
		ret=PTR_ERR(dev->chunk_bdev);
		dev->chunk_bdev=NULL;
		goto bad;
	}
	if (MAJOR(dev->chunk_bdev->bd_dev) == blk_major) {
		/* The specified chunk device is itself a Nexus virtual disk.
		   Allowing this could cause deadlocks, since both this device
		   and its underlying chunk store would be allocating out of
		   the same bioset (in chunkdata.c).  Also, it's
		   unreasonable. */
		log(KERN_ERR, "cannot use our own virtual device as a chunk "
					"store");
		ret=-EINVAL;
		goto bad;
	}
	/* This is how the BLKGETSIZE64 ioctl is implemented, but
	   bd_inode is labeled "will die" in fs.h */
	capacity=dev->chunk_bdev->bd_inode->i_size / 512;
	if (capacity <= offset) {
		log(KERN_ERR, "specified offset is >= disk capacity");
		ret=-EINVAL;
		goto bad;
	}
	dev->offset=offset;
	/* Make sure the capacity, after offset adjustment, is a multiple
	   of the chunksize */
	capacity=(capacity - offset) & ~(loff_t)(chunk_sectors(dev) - 1);
	debug(DBG_CTR, "Chunk partition capacity: " SECTOR_FORMAT " MB",
				capacity >> 11);
	dev->chunks=chunk_of(dev, capacity);
	
	debug(DBG_CTR, "Allocating queue");
	/* The CFQ I/O scheduler can hold request queue references for a very
	   long time: if a process (e.g. kblockd) has ever done I/O to a block
	   device, the process will hold a reference to its queue until it
	   exits.  At that time, the request queue spinlock will be locked,
	   which means we can't have freed the lock while shutting down the
	   block device.  Passing a NULL spinlock pointer to blk_init_queue()
	   allocates a spinlock along with the request queue structure, thus
	   maintaining these lifetime rules. */
	dev->queue=blk_init_queue(nexus_request, NULL);
	if (dev->queue == NULL) {
		log(KERN_ERR, "couldn't allocate request queue");
		ret=-ENOMEM;
		goto bad;
	}
	dev->queue->queuedata=dev;
	blk_queue_bounce_limit(dev->queue, BLK_BOUNCE_ANY);
	blk_queue_max_phys_segments(dev->queue, MAX_SEGS_PER_IO);
	/* By default, blk_rq_map_sg() coalesces physically adjacent pages
	   into the same segment, resulting in a segment that spans more
	   than one page but only points directly to the first struct page.
	   This works fine when scatterlist_copy() kmaps low memory but
	   will die if it kmaps high memory.  Instead, we tell blk_rq_map_sg()
	   not to cross page boundaries when coalescing segments. */
	blk_queue_segment_boundary(dev->queue, PAGE_SIZE - 1);
	/* blk_rq_map_sg() enforces a minimum boundary of PAGE_CACHE_SIZE.
	   If that ever becomes larger than PAGE_SIZE, the above call
	   won't do the right thing for us and we'll need to modify
	   scatterlist_copy() to divide each scatterlist entry into its
	   constituent pages. */
	BUILD_BUG_ON(PAGE_SIZE != PAGE_CACHE_SIZE);
	blk_queue_max_sectors(dev->queue,
				chunk_sectors(dev) * (MAX_CHUNKS_PER_IO - 1));
	
	debug(DBG_CTR, "Configuring transforms");
	dev->suite=suite;
	dev->default_compression=default_compress;
	dev->supported_compression=supported_compress;
	/* This also validates the parameters.  Perhaps the validation code
	   should be moved to the ctr instead. */
	ret=thread_register(dev);
	if (ret) {
		log(KERN_ERR, "could not configure transforms");
		goto bad;
	}
	
	debug(DBG_CTR, "Allocating chunkdata");
	ret=chunkdata_alloc_table(dev);
	if (ret) {
		log(KERN_ERR, "couldn't allocate chunkdata");
		goto bad;
	}
	
	debug(DBG_CTR, "Allocating disk");
	dev->gendisk=alloc_disk(MINORS_PER_DEVICE);
	if (dev->gendisk == NULL) {
		log(KERN_ERR, "couldn't allocate gendisk");
		ret=-ENOMEM;
		goto bad;
	}
	dev->gendisk->major=blk_major;
	dev->gendisk->first_minor=devnum*MINORS_PER_DEVICE;
	dev->gendisk->minors=MINORS_PER_DEVICE;
	sprintf(dev->gendisk->disk_name, "%s", kdevice_get_name(dev->kdevice));
	dev->gendisk->fops=&nexus_ops;
	dev->gendisk->queue=dev->queue;
	dev->gendisk->private_data=dev;
	set_capacity(dev->gendisk, capacity);
	
	/* Everything has been done except actually adding the disk.  It's
	   now safe to populate the sysfs directory (i.e., the attributes
	   will be valid) */
	debug(DBG_CTR, "Populating sysfs attributes");
	ret=kdevice_populate(dev->kdevice);
	if (ret) {
		log(KERN_ERR, "couldn't add sysfs attributes");
		goto bad;
	}
	
	debug(DBG_CTR, "Scheduling add_disk");
	/* add_disk() initiates I/O to read the partition tables, so userspace
	   needs to be able to process key requests while it is running.
	   If we called add_disk() directly here, we would deadlock. */
	WORK_INIT(&dev->cb_add_disk, nexus_add_disk);
	/* Make sure the dev isn't freed until add_disk() completes */
	nexus_dev_get(dev);
	/* We use the shared workqueue in order to prevent deadlock: if we
	   used our own threads, add_disk() would block its own I/O to the
	   partition table. */
	if (!schedule_work(&dev->cb_add_disk))
		BUG();
	
	return dev;
	
bad:
	spin_lock(&state.lock);
	list_del_init(&dev->lh_devs);
	spin_unlock(&state.lock);
	nexus_dev_put(dev, 1);
	return ERR_PTR(ret);
	/* Until we have a refcount, we can't fail through the destructor */
early_fail_kdevice:
	kfree(dev);
early_fail_devalloc:
	free_devnum(devnum);
early_fail_devnum:
	module_put(THIS_MODULE);
early_fail_module:
	return ERR_PTR(ret);
}

/**
 * nexus_init - module initialization function
 **/
static int __init nexus_init(void)
{
	int ret;
	
	debug(DBG_ANY, "===================================================");
	log(KERN_INFO, "loading, revision %s", rcs_revision);
	
	spin_lock_init(&state.lock);
	INIT_LIST_HEAD(&state.devs);
	INIT_LIST_HEAD(&state.all_devs);
	
	debug(DBG_INIT, "Initializing request handler");
	ret=request_start();
	if (ret)
		goto bad_request;
	
	debug(DBG_INIT, "Creating class");
	class=class_create(THIS_MODULE, DEVICE_NAME);
	if (IS_ERR(class)) {
		ret=PTR_ERR(class);
		log(KERN_ERR, "couldn't create class");
		goto bad_class;
	}
	
	debug(DBG_INIT, "Starting chunkdata");
	ret=chunkdata_start();
	if (ret) {
		log(KERN_ERR, "couldn't set up chunkdata");
		goto bad_chunkdata;
	}
	
	debug(DBG_INIT, "Starting threads");
	ret=thread_start();
	if (ret) {
		log(KERN_ERR, "couldn't start kernel threads");
		goto bad_thread;
	}
	
	debug(DBG_INIT, "Registering block driver");
	ret=register_blkdev(0, MODULE_NAME);
	if (ret < 0) {
		log(KERN_ERR, "block driver registration failed");
		goto bad_blkdev;
	}
	blk_major=ret;
	
	/* Okay, now all of our internal structure is set up.  We now must
	   expose the interfaces that allow others to obtain a reference to
	   us: the character device and the sysfs attributes.  Once we expose
	   either interface, we can't fail, since our code will be unloaded
	   while others have references to it.  The chardev is the really
	   important part, so we start it first; sysfs registration failures
	   can be ignored without causing too many problems. */
	
	debug(DBG_INIT, "Starting chardev");
	ret=chardev_start();
	if (ret) {
		log(KERN_ERR, "couldn't register chardev");
		goto bad_chrdev;
	}
	
	debug(DBG_INIT, "Populating sysfs class");
	ret=class_populate();
	if (ret)
		log(KERN_ERR, "couldn't add class attributes");

	return 0;

bad_chrdev:
	unregister_blkdev(blk_major, MODULE_NAME);
bad_blkdev:
	thread_shutdown();
bad_thread:
	chunkdata_shutdown();
bad_chunkdata:
	class_unregister(class);
bad_class:
	request_shutdown();
bad_request:
	return ret;
}
module_init(nexus_init);

/**
 * nexus_shutdown - module de-initialization function
 **/
static void __exit nexus_shutdown(void)
{
	log(KERN_INFO, "unloading");
	
	chardev_shutdown();
	
	unregister_blkdev(blk_major, MODULE_NAME);
	
	thread_shutdown();
	
	chunkdata_shutdown();
	
	/* Automatically unregisters attributes */
	class_unregister(class);
	
	request_shutdown();
}
module_exit(nexus_shutdown);

MODULE_AUTHOR("Benjamin Gilbert <bgilbert@cs.cmu.edu>");
MODULE_DESCRIPTION("OpenISR virtual block device");
MODULE_LICENSE("GPL");
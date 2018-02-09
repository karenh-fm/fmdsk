/*************************************************************************
 * 
 * Fusion Memory Confidential
 * __________________
 * 
 *  Fusion Memory Incorporated 
 *  All Rights Reserved.
 * 
 * NOTICE:  All information contained herein is, and remains
 * the property of Fusion Memory and its suppliers, if any.
 * The intellectual and technical concepts contained herein are 
 * proprietary to Fusion Memory and its suppliers and may be covered by
 * U.S. and Foreign Patents, patents in process, and are protected by 
 * trade secret or copyright law. Dissemination of this information or 
 * reproduction of this material is strictly forbidden unless prior 
 * written permission is obtained from Fusion Memory.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/major.h>
#include <linux/version.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/highmem.h>
#include <linux/mutex.h>
#include <linux/radix-tree.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/hdreg.h>
#include <linux/dma-contiguous.h>
#include <asm/uaccess.h>

#include "fm_dsk.h"
#include "fm_mem.h"
#include "fm_cache.h"

#define FM_DRIVER_VERSION "0.5"

//uint cache_nr_pages = 1572864; /* 6GB */
uint cache_nr_pages = 786432;  /* 3GB ... TESTING ...*/
module_param(cache_nr_pages, uint, S_IRUGO);
MODULE_PARM_DESC(cache_nr_pages, "Size of cache in nr_pages. (Default=6GB)");

uint dsk_nr_pages = 2097152;
module_param(dsk_nr_pages, uint, S_IRUGO);
MODULE_PARM_DESC(dsk_nr_pages, "Size of RAM Disk in nr_pages. (Default=8GB)");

int hiwat = 5;
module_param(hiwat, int, S_IRUGO);
MODULE_PARM_DESC(hiwat, "Cache eviction high water mark. (Default=5)");

int evict = 10;
module_param(evict, int, S_IRUGO);
MODULE_PARM_DESC(evict, "Cache eviction number of entries. (Default=10)");

static int max_part;
module_param(max_part, int, S_IRUGO);
MODULE_PARM_DESC(max_part, "Maximum number of partitions per RAM disk");

static int fmd_major_num = 0;
static LIST_HEAD(fmd_devices);
static DEFINE_MUTEX(fmd_devices_mutex); /* protects list of devices */
static DEFINE_MUTEX(fmd_mutex);

/* Defines to cleanly view bio structure changes between kernels */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0)
#define BIO_SECTOR(bio)		bio->bi_iter.bi_sector
#define BV_CUR_SECTORS(bvec) 	((bvec.bv_len) >> SECTOR_SHIFT)
#define BV_LEN(bvec)		(bvec.bv_len)
#define BV_PAGE(bvec)		(bvec.bv_page)
#define BV_OFFSET(bvec)		(bvec.bv_offset)
#else
#define BIO_SECTOR(bio)		bio->bi_sector
#define BV_CUR_SECTORS(bvec) 	((bvec->bv_len) >> SECTOR_SHIFT)
#define BV_LEN(bvec)		(bvec->bv_len)
#define BV_PAGE(bvec)		(bvec->bv_page)
#define BV_OFFSET(bvec)		(bvec->bv_offset)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,4,0)
#define	BIO_KMAP_ATOMIC(page, usr)  kmap_atomic(page)
#define	BIO_KUNMAP_ATOMIC(dst, usr) kunmap_atomic(dst)
#else
#define	BIO_KMAP_ATOMIC(page, usr)  kmap_atomic(page, usr)
#define	BIO_KUNMAP_ATOMIC(dst, usr) kunmap_atomic(dst, usr)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,8,0)
	/* rw == is_write */
#define	BIO_IS_WRITE(rw) (rw)
#define	BIO_IS_READ(rw) (!rw)
#else
#define	BIO_IS_WRITE(rw) (rw != READ)
#define	BIO_IS_READ(rw) (rw == READ)
#endif


/*-------------------------------------------------------------*/
/*----------------   Block Device Functions   -----------------*/
/*-------------------------------------------------------------*/

/* TODO: DAX support not yet added !!! This is just a placeholder. */

#if DAX_SUPPORT && !CACHE_PAGES
#ifdef CONFIG_BLK_DEV_RAM_DAX
static long fmd_direct_access(struct block_device *bdev, sector_t sector,
			void **kaddr, pfn_t *pfn)
{
	struct fmd_device_t *fmd = bdev->bd_disk->private_data;
        size_t offset = sector << 9;

	if (!fmd)
		return -ENODEV;

	*kaddr = (void __force *) fmd->virt + offset;
	//FIXME: *pfn = (fmd->phys + offset) >> PAGE_SHIFT;

	return (fmd->nr_pages * PAGE_SIZE) - offset;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,12,0)
static size_t fmd_dax_copy_from_iter(struct dax_device *dax_dev, pgoff_t pgoff,
		void *addr, size_t bytes, struct iov_iter *i)
{
	return NULL;
//	return copy_from_iter(addr, bytes, i);
}

static const struct dax_operations fmd_dax_ops = {
	.direct_access = fmd_dax_direct_access,
	//.copy_from_iter = fmd_dax_copy_from_iter,
};
#endif  /* LINUX_VERSION_CODE >= KERNEL_VERSION(4,12,0) */

#else   /* CONFIG_BLK_DEV_RAM_DAX */
#define fmd_direct_access NULL
#endif  /* CONFIG_BLK_DEV_RAM_DAX */
#endif  /* DAX_SUPPORT */

/* TODO: IOCTL support not yet added !!! This is just a placeholder. */

static int fmd_ioctl(struct block_device *bdev, fmode_t mode,
			unsigned int cmd, unsigned long arg)
{
	int error;
	struct fmd_device_t *fmd = bdev->bd_disk->private_data;
	printk(KERN_INFO "%s: %s: 0x%x\n", fmd->dev_name, __func__, cmd);

	//if (cmd != BLKFLSBUF)
		return -ENOTTY;

	/*
	 * ram device BLKFLSBUF has special semantics, we want to actually
	 * release and destroy the ramdisk data.
	 */
	mutex_lock(&fmd_mutex);
	mutex_lock(&bdev->bd_mutex);
	error = -EBUSY;
	if (bdev->bd_openers <= 1) {
		/*
		 * Kill the cache first, so it isn't written back to the
		 * device.
		 *
		 * Another thread might instantiate more buffercache here,
		 * but there is not much we can do to close that race.
		 */
		kill_bdev(bdev);
		//fmd_free_pages(fmd);
		error = 0;
	}
	mutex_unlock(&bdev->bd_mutex);
	mutex_unlock(&fmd_mutex);

	return error;
}

static const struct block_device_operations fmd_fops = {
	.owner =		THIS_MODULE,
	.ioctl =		fmd_ioctl,
#if DAX_SUPPORT && !CACHE_PAGES && LINUX_VERSION_CODE < KERNEL_VERSION(4,12,0)
#ifdef CONFIG_BLK_DEV_RAM_DAX
	.direct_access =	fmd_direct_access,
#endif
#endif
};


/*-------------------------------------------------------------*/
/*--------------------   I/O Functions   ----------------------*/
/*-------------------------------------------------------------*/

#if CACHE_PAGES
/* 
 * WRITE: 
 * Copy n bytes from src to the fmd starting at sector. Does not sleep. 
 *  
 * We don't actually write the page to the dsk at this point, 
 * instead we write the page to our internal cache. 
 * We mark the cache page dirty and flush the contents to the dsk later.
 */
static void copy_to_fmd(struct fmd_device_t *fmd, const void *src,
			sector_t sector, size_t n)
{
	struct fmd_page_t *page;
	unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	size_t copy;

	copy = min_t(size_t, n, PAGE_SIZE - offset);

	page = fmd_radix_tree_lookup_page(fmd, sector);
	BUG_ON(!page);

	memcpy(page->virt + offset, src, copy);
	fmd_radix_tree_mark_dirty_page(fmd, page);

	if (copy < n) {
		src += copy;
		sector += copy >> SECTOR_SHIFT;
		offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
		copy = n - copy;
		page = fmd_radix_tree_lookup_page(fmd, sector);
		BUG_ON(!page);

		memcpy(page->virt + offset, src, copy);
		fmd_radix_tree_mark_dirty_page(fmd, page);
	}
}

/*
 * READ: 
 * Copy n bytes to dst from the fmd cache starting at sector. Does not sleep.
 */
static void copy_from_fmd(void *dst, struct fmd_device_t *fmd,
			sector_t sector, size_t n)
{
	struct fmd_page_t *page;
	unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	size_t copy;

	copy = min_t(size_t, n, PAGE_SIZE - offset);
	page = fmd_radix_tree_lookup_page(fmd, sector);

	if (page) {  /* cache hit */
                memcpy(dst, page->virt + offset, copy);
        } else { /* cache miss */
                memset(dst, 0, copy);  /* FIXME: Page Fault*/
        }

        if (copy < n) {
		dst += copy;
		sector += copy >> SECTOR_SHIFT;
		offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
		copy = n - copy;

		page = fmd_radix_tree_lookup_page(fmd, sector);
		if (page) {  /* cache hit */
			memcpy(dst, page->virt + offset, copy);
		} else { /* cache miss */
			memset(dst, 0, copy);
		}
        }
}

/* 
 * WRITE PREP: 
 * copy_to_fmd_setup must be called before copy_to_fmd. It may sleep.
 */
static int copy_to_fmd_setup(struct fmd_device_t *fmd, sector_t sector, size_t n)
{
	unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	size_t copy;

	copy = min_t(size_t, n, PAGE_SIZE - offset);
	if (!fmd_radix_tree_insert_page(fmd, sector))
		return -ENOSPC;
	if (copy < n) {
		sector += copy >> SECTOR_SHIFT;
		if (!fmd_radix_tree_insert_page(fmd, sector))
			return -ENOSPC;
	}
	return 0;
}


/*
 * Process a single bvec of a bio.
 */
static int fmd_do_bvec(struct fmd_device_t *fmd, struct page *page,
		       unsigned int len, unsigned int off, 
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,8,0)
		       bool rw,
#else
		       int rw,
#endif
		       sector_t sector)
{
	void *mem;
	int err = 0;

	/* FIXME: Page Fault */
	if (BIO_IS_WRITE(rw)) {
		err = copy_to_fmd_setup(fmd, sector, len);
		if (err)
			goto out;
	}

	mem = BIO_KMAP_ATOMIC(page, KM_USER0);  /* map kernel's memory */
	if (BIO_IS_READ(rw)) {
		copy_from_fmd(mem + off, fmd, sector, len);
		flush_dcache_page(page);

	} else {
		flush_dcache_page(page);
		copy_to_fmd(fmd, mem + off, sector, len);
	}
	BIO_KUNMAP_ATOMIC(mem, KM_USER0);


out:
	return err;
}
#else  /* !CACHE_PAGES */

static int fmd_do_bvec(struct fmd_device_t *fmd, struct page *page,
		       unsigned int len, unsigned int off, 
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,8,0)
		       bool rw, sector_t sector)
#else
		       int rw, sector_t sector)
#endif
{
	void *mem;
	int err = 0;

	mem = BIO_KMAP_ATOMIC(page, KM_USER0);  /* map kernel's memory */
	if (BIO_IS_READ(rw)) {
		//printk(KERN_INFO "%s: %s: READ mem=0x%p virt=0x%p len=0x%x\n", fmd->name, __func__, mem + off, fmd->virt + sector, len);
		memcpy_fromio(mem + off, fmd->virt + sector, len);
	} else {
		//printk(KERN_INFO "%s: %s: WRITE virt=0x%p mem=0x%p len=0x%x\n", fmd->name, __func__, fmd->virt + sector, mem + off, len);
		memcpy_toio(fmd->virt + sector, mem + off, len);
	}
	BIO_KUNMAP_ATOMIC(mem, KM_USER0);

	return err;
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,2,0)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
static blk_qc_t
#else
static void
#endif
#else
static int
#endif
fmd_make_request(struct request_queue *q, struct bio *bio)
{
	struct block_device *bdev = bio->bi_bdev;
	struct fmd_device_t *fmd = bdev->bd_disk->private_data;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,8,0)
	bool rw;
#else
	int rw;
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0)
	struct bio_vec bvec;
	struct bvec_iter iter;
#else
	struct bio_vec *bvec;
	int iter;
#endif
	sector_t sector;
	int err = -EIO;

	sector = BIO_SECTOR(bio);
	if (bio_end_sector(bio) > get_capacity(bdev->bd_disk))
		goto out;


#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,8,0)
	rw = op_is_write(bio_op(bio));
#else
	rw = bio_rw(bio);
	if (rw == READA)
		rw = READ;
#endif

	bio_for_each_segment(bvec, bio, iter) {
		unsigned int len = BV_LEN(bvec);
		
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0)
		err = fmd_do_bvec(fmd, BV_PAGE(bvec), len, BV_OFFSET(bvec), 
				rw, iter.bi_sector << SECTOR_SHIFT);
#else
		err = fmd_do_bvec(fmd, BV_PAGE(bvec), len, BV_OFFSET(bvec), 
				rw, sector);
#endif		
		if (err)
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,3,0)
			break;
#else
			goto io_error;
#endif
		sector += len >> SECTOR_SHIFT;
	}

out:
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,3,0)
	set_bit(BIO_UPTODATE, &bio->bi_flags);
	bio_endio(bio, err);
#else
	bio_endio(bio);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
	return BLK_QC_T_NONE;
#else
	return;
#endif
io_error:
	bio_io_error(bio);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
	return BLK_QC_T_NONE;
#endif
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,2,0)
	return 0;
#endif
}


/*-------------------------------------------------------------*/
/*---------------   Initialization Functions   ----------------*/
/*-------------------------------------------------------------*/

static struct fmd_device_t *fmd_alloc_dev(int i, int dev_type)
{
	struct fmd_device_t *fmd;
	struct gendisk *disk;
	struct request_queue *q;

	printk(KERN_INFO "%s%d: %s\n", DRIVER_NAME, i, __func__);

#if CACHE_PAGES
	fmd = kzalloc(sizeof(struct fmd_device_t) + sizeof(struct fmd_cache_t), GFP_KERNEL);
#else
	fmd = kzalloc(sizeof(struct fmd_device_t), GFP_KERNEL);
#endif
	if (!fmd)
		goto out;

	fmd->num = i;
	fmd->dev_type = dev_type;
#if CACHE_PAGES
	fmd->cache = fmd + sizeof(struct fmd_device_t);
#endif
	sprintf(fmd->dev_name, "%s%d", (dev_type == FMD_DEV_TYPE_DSK) ? DEV_NAME_DSK : DEV_NAME_MEM, i);
	spin_lock_init(&fmd->lock);

	/* Create block queue */
	q = blk_alloc_queue(GFP_KERNEL);
	if (!q)
		goto out_free_dev;

	fmd->queue = q;
	q->queuedata = fmd;
	blk_queue_make_request(q, fmd_make_request);
	blk_queue_logical_block_size(q, BYTES_PER_SECTOR);
	//blk_queue_physical_block_size(q, PAGE_SIZE);
	//blk_queue_max_hw_sectors(q, 1024 /* UINT_MAX */);
	//blk_queue_bounce_limit(q, BLK_BOUNCE_ANY);

	/* Tell block layer flush capability of the q
	 * REQ_FLUSH = supports flushing
	 * REQ_FUA   = supports bypassing write cache for individual writes */
	//blk_queue_flush(q, REQ_FLUSH); 
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, q);

	/* Create gendisk structure */
	disk = alloc_disk(1);
	if (!disk)
		goto out_free_queue;

	fmd->disk = disk;
	disk->major		= fmd_major_num;
	disk->first_minor	= i;
	disk->fops		= &fmd_fops;
	disk->private_data	= fmd;
	disk->queue		= q;
	disk->flags |= GENHD_FL_EXT_DEVT;
	sprintf(disk->disk_name, "%s", fmd->dev_name);
	set_capacity(disk, dsk_nr_pages * (PAGE_SIZE / 512));  /* capacity in 512 byte sectors */

	/* Allocate or discover memory */
#if CACHE_PAGES
	/* For testing purposes, use part of the dsk for the cache.
	 * Currently only the dsk can be discovered on the test system. */
	if (fmd_memory_alloc_manual_dsk(fmd, E820_TYPE_PMEM,  dsk_nr_pages - cache_nr_pages) != 0)
		goto out_free_queue;
	if (fmd_memory_alloc_manual_cache(fmd, E820_TYPE_PMEM,  cache_nr_pages) != 0)
		goto out_free_queue;
#else
	if (fmd_memory_alloc_manual_dsk(fmd, E820_TYPE_PMEM,  dsk_nr_pages) != 0)
		goto out_free_queue;
#endif


	return fmd;

out_free_queue:
	blk_cleanup_queue(fmd->queue);
out_free_dev:
	kfree(fmd);
out:
	return NULL;
}

static void fmd_free_dev(struct fmd_device_t *fmd)
{
	printk(KERN_INFO "%s: %s\n", fmd->dev_name, __func__);

	fmd_memory_cleanup_manual(fmd);

	if (fmd->disk) {
	    del_gendisk(fmd->disk);
	    put_disk(fmd->disk);
	}
	if (fmd->queue) {
	    blk_cleanup_queue(fmd->queue);
	}
	kfree(fmd);
}

static int __init fmd_init(void)
{
	int i = 0; 
	struct fmd_device_t *fmd = NULL;

	fmd_major_num = register_blkdev(fmd_major_num, DRIVER_NAME);
	if (fmd_major_num < 0) {
		printk(KERN_ERR "%s: Failed to register, major_num=%d\n", DRIVER_NAME, fmd_major_num);
		return fmd_major_num;
	}

	INIT_LIST_HEAD(&fmd_devices);

	//TODO: Add algorithm here to detect multiple devices from multiple 
	// memory locations
	fmd = fmd_alloc_dev(i, FMD_DEV_TYPE_DSK);
	if (!fmd)
		goto out_free;

	mutex_lock(&fmd_devices_mutex);
	list_add_tail(&fmd->list, &fmd_devices);
	mutex_unlock(&fmd_devices_mutex);
	
	/* Add to kernel's list of active devices
	 * I/O can occur at this point */
	list_for_each_entry(fmd, &fmd_devices, list) {
		printk(KERN_INFO "%s: Add device %s addr 0x%llx size 0x%lx (%lu GB)\n", DRIVER_NAME, fmd->dev_name, fmd->phys, fmd->nr_pages * PAGE_SIZE, (unsigned long int) (fmd->nr_pages * PAGE_SIZE)/ (1024 * 1024 * 1024));
		add_disk(fmd->disk);
	}

	printk(KERN_INFO "%s: module loaded\n", DRIVER_NAME);
	return 0;

out_free:
	unregister_blkdev(RAMDISK_MAJOR, DRIVER_NAME);

	return -ENOMEM;
}

static void __exit fmd_exit(void)
{
	struct fmd_device_t *fmd, *next;

	list_for_each_entry_safe(fmd, next, &fmd_devices, list) {
		printk(KERN_INFO "%s%d: Remove device %s\n", DRIVER_NAME, fmd->num, fmd->disk->disk_name);
		fmd_free_dev(fmd);
	}

	unregister_blkdev(fmd_major_num, DRIVER_NAME);

	printk(KERN_INFO "%s: module unloaded\n", DRIVER_NAME);
}


module_init(fmd_init);
module_exit(fmd_exit);

MODULE_AUTHOR("Fusion Memory");
MODULE_DESCRIPTION("Fusion Memory Block Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(FM_DRIVER_VERSION);


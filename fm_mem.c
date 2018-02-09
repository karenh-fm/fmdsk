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

/*
 * fm_mem - Memory Discovery and Allocation
 */ 

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/mempool.h>
#include <linux/dma-contiguous.h>
#include <linux/blkdev.h>
#include <asm/uaccess.h>
#include "fm_dsk.h"
#include "fm_mem.h"
#include "fm_cache.h"


/* Globals used for manual memory detection */
uint64_t phys_search_start = 0x100000000;  /* 4 GB */
uint64_t phys_search_end = 0x480000000; /* 18 GB */
uint64_t phys_cur = 0x100000000;

extern int hiwat;
extern int evict;

static uint64_t fmd_locate_physical_mem(int e820_type, unsigned int nr_pages)
{
	uint64_t start = 0;

	printk(KERN_INFO "%s: %s: type %d size 0x%lx start 0x%llx end 0x%llx\n", DRIVER_NAME, __func__, e820_type, nr_pages * PAGE_SIZE, phys_cur, phys_search_end);
	while (phys_cur <= phys_search_end) {
		/* Find starting address*/
		if (start == 0) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,12,0)
			if (e820__mapped_any(phys_cur, phys_cur + 1, e820_type)) {
#else
			if (e820_any_mapped(phys_cur, phys_cur + 1, e820_type)) {
#endif
				printk(KERN_INFO "%s: %s: starting addr %llx\n", DRIVER_NAME, __func__, phys_cur);
				start = phys_cur;
			}
		} else {  /* Find ending address */
			// Stop searching if specified size has been found
			if ((phys_cur - start) == (nr_pages * PAGE_SIZE)) {
	                	printk(KERN_INFO "%s: %s: found size 0x%llx\n", DRIVER_NAME, __func__, phys_cur-start);
				return start;
			}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,12,0)
	                if (!e820__mapped_any(phys_cur, phys_cur + 1, e820_type)) {
#else
	                if (!e820_any_mapped(phys_cur, phys_cur + 1, e820_type)) {
#endif
                                if ((phys_cur - start) < (nr_pages * PAGE_SIZE)) {
					start = 0;
	                                printk(KERN_INFO "%s: %s: area too small 0x%llx\n", DRIVER_NAME, __func__, phys_cur-start);
				} else {
	                                printk(KERN_INFO "%s: %s: found size 0x%llx\n", DRIVER_NAME, __func__, phys_cur-start);
					return start;
				}
			}

		}
	        phys_cur += PAGE_SIZE;
        }
	printk(KERN_INFO "%s: %s: type %d NOT found cur=0x%llx\n", DRIVER_NAME, __func__, e820_type, phys_cur);
        return 0;
}

int fmd_memory_alloc_manual_dsk(struct fmd_device_t *fmd, int e820_type, unsigned int nr_pages)
{
        BUG_ON (!fmd);
	printk(KERN_INFO "%s: %s\n", fmd->dev_name, __func__);

        fmd->nr_pages = nr_pages;
	fmd->phys = fmd_locate_physical_mem(e820_type, nr_pages);

	if (fmd->phys == 0) {
		printk(KERN_INFO "%s: %s: ERROR: Unable to manually locate physical memory\n", fmd->dev_name, __func__);
		goto err_alloc_manual_dsk;
	}
	if (!request_mem_region(fmd->phys, fmd->nr_pages * PAGE_SIZE, DRIVER_NAME)) {
		printk(KERN_INFO "%s: %s: ERROR: Unable to request mem region\n", fmd->dev_name, __func__);
		goto err_alloc_manual_dsk;
	}
	fmd->virt = ioremap(fmd->phys, fmd->nr_pages * PAGE_SIZE);
	if (!fmd->virt) {
		printk(KERN_INFO "%s: %s: ERROR: Unable to ioremap mem region\n", fmd->dev_name, __func__);
		goto err_alloc_manual_dsk;
	}

        return 0;

err_alloc_manual_dsk:
        fmd_memory_cleanup_manual(fmd);
        return -ENOMEM;
}

int fmd_memory_alloc_manual_cache(struct fmd_device_t *fmd, int e820_type, unsigned int nr_pages)
{
	struct fmd_cache_t *cache;

        BUG_ON (!fmd || !fmd->cache);
	printk(KERN_INFO "%s: %s\n", fmd->dev_name, __func__);

	cache = (struct fmd_cache_t *) fmd->cache;
	cache->nr_pages_total = nr_pages;
	cache->phys = fmd_locate_physical_mem(e820_type, nr_pages);

	if (cache->phys == 0) {
		printk(KERN_INFO "%s: %s: ERROR: Unable to manually locate physical memory\n", fmd->dev_name, __func__);
		goto err_alloc_manual_dsk;
	}
	if (!request_mem_region(cache->phys, nr_pages * PAGE_SIZE, DRIVER_NAME)) {
		printk(KERN_INFO "%s: %s: ERROR: Unable to request mem region\n", fmd->dev_name, __func__);
		goto err_alloc_manual_dsk;
	}
	cache->virt = ioremap(cache->phys, nr_pages * PAGE_SIZE);
	if (!fmd->virt) {
		printk(KERN_INFO "%s: %s: ERROR: Unable to ioremap mem region\n", fmd->dev_name, __func__);
		goto err_alloc_manual_dsk;
	}

	/* Allocate cache page pool */
	if (fmd_pagepool_init(fmd) != 0) {
		goto err_alloc_manual_dsk;
	}

	fmd_radix_tree_init(fmd);
	fmd_evict_list_init(fmd, hiwat, evict);

        return 0;

err_alloc_manual_dsk:
        fmd_memory_cleanup_manual(fmd);
        return -ENOMEM;
}


void fmd_memory_cleanup_manual(struct fmd_device_t *fmd)
{
        struct fmd_cache_t *cache;

        BUG_ON(!fmd || !fmd->disk);
        cache = (struct fmd_cache_t *) fmd->cache;

	printk(KERN_INFO "%s: %s\n", fmd->dev_name, __func__);

	if (cache && cache->pagepool) {
	    fmd_radix_tree_free_pages(fmd);
	}

	if (fmd->phys != 0 && fmd->nr_pages != 0) {
	    release_mem_region(fmd->phys, fmd->nr_pages * PAGE_SIZE);
            fmd->phys = 0;
	    fmd->nr_pages = 0;
	}
	if (fmd->virt) {
	    iounmap(fmd->virt);
	    fmd->virt = NULL;
	}

	if (cache) {
		if (cache->phys != 0 && cache->nr_pages_total != 0) {
		    release_mem_region(cache->phys,
			cache->nr_pages_total * PAGE_SIZE);
		    cache->phys = 0;
		    cache->nr_pages_total = 0;
		    cache->nr_pages_cache = 0;
		    cache->nr_pages_pagepool = 0;
		}
		if (cache->virt) {
		    iounmap(cache->virt);
		    cache->virt = NULL;
		}
	}
}




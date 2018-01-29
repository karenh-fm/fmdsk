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
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/highmem.h>
#include <linux/mutex.h>
#include <linux/radix-tree.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/mempool.h>

#include "fm_mem.h"
#include "fm_dsk.h"
#include "fm_cache.h"

static struct page *fmd_mempool_alloc_page(struct fmd_device_t *fmd);
static void fmd_mempool_free_page(struct fmd_device_t *fmd, struct page *page);
static void fmd_radix_tree_flush_dirty_page(struct fmd_device_t *fmd, struct page *page);
static inline void fmd_evict_list_add(struct fmd_device_t *fmd, struct page *page); 
static inline void fmd_evict_list_delete(struct fmd_device_t *fmd, struct page *page); 

/*-------------------------------------------------------------*/
/*-------------------   Cache Functions   ---------------------*/
/*-------------------------------------------------------------*/

/* Use memory allocated at boot time as a cache slab.
 * Then allocate a mempool, which uses the cache slab for allocating 
 * and freeing pages.
 */
int fmd_mempool_init(struct fmd_device_t *fmd)
{
	struct fmd_cache_t *cache;

	BUG_ON(!fmd || !fmd->cache);
	cache = (struct fmd_cache_t *) fmd->cache;

	printk(KERN_INFO "%s: %s\n", fmd->dev_name, __func__);

	if (!cache->phys || !cache->virt) {
		printk(KERN_INFO "%s: %s: Physical memory not yet allocated\n",
		       fmd->dev_name, __func__);
		goto err_exit_cache_init;
	}

        /* Use pre-allocated memory as a cache slab */
        cache->slab = kmem_cache_create(cache->virt, PAGE_SIZE, 0,
                                        SLAB_HWCACHE_ALIGN, NULL);
        if (!cache->slab) {
		printk(KERN_INFO "%s: %s: Error allocating slab\n",
		       fmd->dev_name, __func__);
                goto err_exit_cache_init;
        }

        /* Allocate a mempool, which uses the cache slab for allocating
         * and freeing pages */
        cache->mempool = mempool_create_slab_pool(cache->nr_pages * PAGE_SIZE,
                                                  cache->slab);
        if (!cache->mempool) {
		printk(KERN_INFO "%s: %s: Error allocating mempool\n", 
		       fmd->dev_name, __func__);
                goto err_exit_cache_init;
        }

        /* Allocate a pool of page structs for MANUAL memory allocation */

        cache->page_slab = kzalloc(sizeof(struct page) * fmd->nr_pages,
					   GFP_KERNEL);
        if (!cache->page_slab) {
		printk(KERN_INFO "%s: %s: Error allocating page slab\n",
			       fmd->dev_name, __func__);
                goto err_exit_cache_init;
        }
        cache->page_pool = mempool_create_slab_pool(sizeof(struct page),
                                                    cache->page_slab);
        if (!cache->page_pool) {
		printk(KERN_INFO "%s: %s: Error allocating page pool\n",
		       fmd->dev_name, __func__);
                goto err_exit_cache_init;
        }

	//TODO: Map page struct to pages for faster lookup in fast path

	return 0;

err_exit_cache_init:
        fmd_mempool_cleanup(fmd);
        return -ENOMEM;
}

/* Destroy cache page pool */
void fmd_mempool_cleanup(struct fmd_device_t *fmd)
{
        struct fmd_cache_t *cache;

	BUG_ON(!fmd || !fmd->cache);
        cache = (struct fmd_cache_t *) fmd->cache;

	printk(KERN_INFO "%s: %s\n", fmd->dev_name, __func__);

	/* Free pool of page structs for MANUAL memory allocation */
	if (cache->page_pool) {
		mempool_destroy(cache->page_pool);
		cache->page_pool = NULL;
	}
	if (cache->page_slab) {
		kfree(cache->page_slab);
		cache->page_slab = NULL;
	}

	/* Free cache mempool and slab */
        if (cache->mempool) {
                mempool_destroy(cache->mempool);
                cache->mempool = NULL;
        }
        if (cache->slab) {
                kmem_cache_destroy(cache->slab);
                cache->slab = NULL;
        }
}

/* Allocate page from cache page pool */
static struct page *fmd_mempool_alloc_page(struct fmd_device_t *fmd)
{
	void *ret;
	struct page *page = NULL;
	struct fmd_cache_t *cache;

	BUG_ON(!fmd || !fmd->cache);
        cache = (struct fmd_cache_t *) fmd->cache;

	printk(KERN_INFO "%s: %s\n", fmd->dev_name, __func__);

	//TODO: Map page struct to pages during init, then only pull from page pool during runtime.
	page = (struct page *) mempool_alloc(cache->page_pool, GFP_KERNEL);
	if (!page) {
		goto exit_alloc_page;
	}

	ret = (struct page *) mempool_alloc(cache->mempool, GFP_KERNEL);
	if (!ret) {
		mempool_free(page, cache->page_pool);
		page = NULL;
		goto exit_alloc_page;
	}

	/* Populate page struct */
	set_page_address(page, ret);
	page->index = (page_address(page) - cache->virt) >> PAGE_SHIFT;


	cache->page_cnt++;

exit_alloc_page:
	return (page);
}

/* Return page back to cache page pool */
static void fmd_mempool_free_page(struct fmd_device_t *fmd, struct page *page)
{
	struct fmd_cache_t *cache;

	BUG_ON(!fmd || !fmd->cache || !page);
        cache = (struct fmd_cache_t *) fmd->cache;

	printk(KERN_INFO "%s: %s\n", fmd->dev_name, __func__);

	mempool_free(page, cache->page_pool);
        cache->page_cnt--;
}

/*-------------------------------------------------------------*/
/*-------------   Radix Tree (Page) Functions   ---------------*/
/*-------------------------------------------------------------*/

#define MAX_BATCH 16  /* Max # radix tree entries to view at once */

void 
fmd_radix_tree_init(struct fmd_device_t *fmd)
{
	struct fmd_cache_t *cache;

        BUG_ON(!fmd);
        cache = (struct fmd_cache_t *) fmd->cache;

	printk(KERN_INFO "%s: %s\n", fmd->dev_name, __func__);

	INIT_RADIX_TREE(&cache->tree, GFP_ATOMIC);
}

/* Free all pages in the radix tree, eviction list and mempool */
void 
fmd_radix_tree_free_pages(struct fmd_device_t *fmd)
{
        unsigned long pos = 0;
        int nr_pages;
        struct page *batch[MAX_BATCH];
        struct page *page;
        struct fmd_cache_t *cache;

        BUG_ON(!fmd || !fmd->cache);
        cache = (struct fmd_cache_t *) fmd->cache;

	printk(KERN_INFO "%s: %s:\n", fmd->dev_name, __func__);
        do {
                int i;
                nr_pages = radix_tree_gang_lookup(&cache->tree, (void **)batch,
                                                  pos, MAX_BATCH);

                for (i=0; i<MAX_BATCH; i++) {
                        page = batch[i];
                        BUG_ON(page->index < pos);
                        pos = page->index;
                        fmd_radix_tree_free_page(fmd, page);
                }
                pos++;
        }
        while (nr_pages == MAX_BATCH);
}

/* Free the specified page from the radix tree, eviction list and mempool */
void
fmd_radix_tree_free_page(struct fmd_device_t *fmd, struct page *page)
{
        struct fmd_cache_t *cache;
	struct page *ret;

        BUG_ON(!fmd || !fmd->cache || !page);
        cache = (struct fmd_cache_t *) fmd->cache;

	printk(KERN_INFO "%s: %s\n", fmd->dev_name, __func__);

        fmd_radix_tree_flush_dirty_page(fmd, page);

        /* Remove page from eviction list, radix_tree and cache pool */
        ret = radix_tree_delete(&cache->tree, page->index);
        BUG_ON(!ret || ret != page);
        if (page) {
		printk(KERN_INFO "%s: %s: page %ld\n", fmd->dev_name, __func__, page->index);
		fmd_evict_list_delete(fmd, page);
		fmd_mempool_free_page(fmd, page);
        }
}

/*
 * Look up and return a fmd's page for a given sector.
 */
struct page *
fmd_radix_tree_lookup_page(struct fmd_device_t *fmd, sector_t sector)
{
        pgoff_t index;
        struct page *page;
	struct fmd_cache_t *cache;

        BUG_ON(!fmd || !fmd->cache);

	cache = (struct fmd_cache_t *) fmd->cache;

	printk(KERN_INFO "%s: %s\n", fmd->dev_name, __func__);

        rcu_read_lock();
        index = sector >> PAGE_SECTORS_SHIFT;  /* sector to page index */
	printk(KERN_INFO "%s: %s: index %ld\n", fmd->dev_name, __func__, index);
        page = (struct page *) radix_tree_lookup(&cache->tree, index);
        rcu_read_unlock();

	printk(KERN_INFO "%s: %s: page %p\n", fmd->dev_name, __func__, page);
        BUG_ON(page && page->index != index);
        return page;
}

/*
 * Look up and return a cached page for a given sector.
 * If one does not previously exist in cache, allocate an empty page, 
 * insert it into the radix tree and eviction list, then return it.
 */
struct page *
fmd_radix_tree_insert_page(struct fmd_device_t *fmd, sector_t sector)
{
        pgoff_t index;
        struct page *page;
	struct fmd_cache_t *cache;
	int rval;

        BUG_ON(!fmd);
	cache = (struct fmd_cache_t *) fmd->cache;

	printk(KERN_INFO "%s: %s\n", fmd->dev_name, __func__);

        /* If page already exists in radix_tree, return it */
        page = fmd_radix_tree_lookup_page(fmd, sector);
        if (page) {
                return page;
        }

        /* Alloc page from cache mempool */
        page = fmd_mempool_alloc_page(fmd);
        if (!page) {
                return NULL;
        }

        /* Insert newly allocated page into radix_tree */
        if (radix_tree_preload(GFP_NOIO)) {
                fmd_mempool_free_page(fmd, page);
                return NULL;
        }

	spin_lock(&fmd->lock);
	index = sector >> PAGE_SECTORS_SHIFT;
	rval =  radix_tree_insert(&cache->tree, index, page);
        if (rval == -EEXIST) {
                fmd_mempool_free_page(fmd, page);
                page = radix_tree_lookup(&cache->tree, index);
                BUG_ON(!page);
                BUG_ON(page->index != index);
	} else if (rval == -ENOMEM) {
		fmd_mempool_free_page(fmd, page);
                page = NULL;
	}

	/* Insert page into eviction list */
	if (page) {
	    printk(KERN_INFO "%s: %s: page %ld\n", fmd->dev_name, __func__, page->index);
	    fmd_evict_list_add(fmd, page);
	}

	spin_unlock(&fmd->lock);
        radix_tree_preload_end();

        return page;
}

/*
 * This function is called as a result of a write to a cached page. 
 * Writes are not written directly to the disk, but are written to cache. 
 * Some future event (sync, cache eviction, driver unload) will trigger dirty 
 * pages to be flushed (written) from the cache to the disk. 
 */
inline void
fmd_radix_tree_mark_dirty_page(struct fmd_device_t *fmd, struct page *page)
{
	struct fmd_cache_t *cache;

	BUG_ON(!fmd || !fmd->cache);

	printk(KERN_INFO "%s: %s\n", fmd->dev_name, __func__);

	cache = (struct fmd_cache_t *) fmd->cache;
        radix_tree_tag_set(&cache->tree, page->index, PAGECACHE_TAG_DIRTY);
}

/*
 * This function is called as a result of some event (sync, cache eviction, 
 * driver unload) will trigger dirty pages to be flushed (written) from the 
 * cache to the disk.
 */
static void
fmd_radix_tree_flush_dirty_page(struct fmd_device_t *fmd, struct page *page)
{
        struct fmd_cache_t *cache;
        BUG_ON(!fmd || !fmd->cache || !page);

	printk(KERN_INFO "%s: %s: page %ld\n", fmd->dev_name, __func__, page->index);

        cache = (struct fmd_cache_t *) fmd->cache;
        if (radix_tree_tag_get(&cache->tree, page->index, PAGECACHE_TAG_DIRTY)) {
                /* Flush page to disk then clear tag */
                int offset = page_address(page) - fmd->virt;
                copy_page(fmd->virt + offset, cache->virt + offset);
                radix_tree_tag_clear(&cache->tree, page->index, PAGECACHE_TAG_DIRTY);
        }
}

/* 
 * This function is called as a result of a sync.
 * All dirty cache pages will be flushed (written) to the disk.
 */
void 
fmd_radix_tree_flush_dirty_pages(struct fmd_device_t *fmd)
{
        unsigned long pos = 0;
        int nr_pages;
        struct page *batch[MAX_BATCH];
        struct page *page = NULL;
        struct fmd_cache_t *cache;

        BUG_ON(!fmd);

	printk(KERN_INFO "%s: %s\n", fmd->dev_name, __func__);

	cache = (struct fmd_cache_t *) fmd->cache;
        if (!radix_tree_tagged(&cache->tree, PAGECACHE_TAG_DIRTY)) {
                return;
        }

        /* Find all dirty pages */
        do {
                int i;
                nr_pages = radix_tree_gang_lookup_tag(& cache->tree, 
                                                      (void **)batch, pos, 
                                                      MAX_BATCH, 
                                                      PAGECACHE_TAG_DIRTY);

                for (i=0; i<nr_pages; i++) {
                        page = batch[i];
                        BUG_ON(page->index < pos);
                        pos = page->index;

                        /* Flush page to disk then clear tag */
                        fmd_radix_tree_flush_dirty_page(fmd, page);
                }
		pos++;
        } while (nr_pages == MAX_BATCH);
}

/*-------------------------------------------------------------*/
/*---------------   Eviction List Functions   -----------------*/
/*-------------------------------------------------------------*/


void 
fmd_evict_list_init(struct fmd_device_t *fmd, int hiwat, int evict)
{	
	struct fmd_cache_t *cache;

	BUG_ON(!fmd);
        cache = (struct fmd_cache_t *) fmd->cache;

	printk(KERN_INFO "%s: %s\n", fmd->dev_name, __func__);

        /* Initialize variables used for cache eviction */
        cache->evict_hiwat = hiwat;
        cache->evict_num_entries = evict;
        cache->page_cnt = 0;
        INIT_LIST_HEAD(&cache->evict_list);
}

static inline void 
fmd_evict_list_add(struct fmd_device_t *fmd, struct page *page) {

	struct fmd_cache_t *cache = (struct fmd_cache_t *) fmd->cache;

	printk(KERN_INFO "%s: %s\n", fmd->dev_name, __func__);

	list_add_tail(&page->lru, &cache->evict_list);
}

static inline void 
fmd_evict_list_delete(struct fmd_device_t *fmd, struct page *page) {

	printk(KERN_INFO "%s: %s\n", fmd->dev_name, __func__);

	list_del_init(&page->lru);
}

inline unsigned char 
fmd_cache_full(struct fmd_device_t *fmd)
{
	unsigned char retval;
	struct fmd_cache_t *cache = (struct fmd_cache_t *) fmd->cache;

	retval = (cache->page_cnt >= cache->evict_hiwat);
	return retval;
}

void 
fmd_evict_pages(struct fmd_device_t *fmd)
{
        int i;
        struct page *page;
	struct fmd_cache_t *cache;

	BUG_ON(!fmd);
        cache = (struct fmd_cache_t *) fmd->cache;

	printk(KERN_INFO "%s: %s\n", fmd->dev_name, __func__);

        /* Retrieve page from head of list */
        for (i=0; i<cache->evict_num_entries; i++) {
                if (list_empty(&cache->evict_list)) {
                        break;
                }

                /* Evict pages from head of list */
                page = list_entry(cache->evict_list.next, struct page, lru);
                BUG_ON(!page);

                /* delete page (also flushes dirty page) */
                fmd_radix_tree_free_page(fmd, page);
        }
}


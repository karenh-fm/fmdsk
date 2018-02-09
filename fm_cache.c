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

#include "fm_mem.h"
#include "fm_dsk.h"
#include "fm_cache.h"

static void fmd_radix_tree_flush_dirty_page(struct fmd_device_t *fmd, struct fmd_page_t *page);
static inline void fmd_evict_list_add(struct fmd_device_t *fmd, struct fmd_page_t *page);
static inline void fmd_evict_list_delete(struct fmd_device_t *fmd, struct fmd_page_t *page);

/*-------------------------------------------------------------*/
/*-------------------   Cache Functions   ---------------------*/
/*-------------------------------------------------------------*/

/* Allocate pagepool and map each page entry to corresponding page in
 * virtual memory
 */
int fmd_pagepool_init(struct fmd_device_t *fmd)
{
    struct fmd_cache_t *cache;
    struct fmd_page_t *page;
    unsigned long i;

    BUG_ON(!fmd || !fmd->cache);
    cache = (struct fmd_cache_t *) fmd->cache;

    printk(KERN_INFO "%s: %s\n", fmd->dev_name, __func__);

    /* Use the memory at the end of the cache memory area to store the
     * page structs */
    BUG_ON (!cache->nr_pages_total || !cache->virt);
    cache->nr_pages_pagepool =
	    PAGE_ALIGN(cache->nr_pages_total * sizeof(struct fmd_page_t)) /
	    PAGE_SIZE;
    if (!cache->nr_pages_pagepool ||
	 cache->nr_pages_pagepool >= cache->nr_pages_total) {
	return -ENOMEM;
    }

    cache->nr_pages_cache = cache->nr_pages_total - cache->nr_pages_pagepool;
    cache->pagepool = cache->virt + (cache->nr_pages_cache * PAGE_SIZE);

    /* Map page structs to corresponding PAGE_SIZE'd memory chunks in cache */
    for (i=0; i < cache->nr_pages_cache; i++) {

	page = &cache->pagepool[i];
	page->index = i;
	page->virt = cache->virt + (i * PAGE_SIZE);
    }

    return 0;
}

/* Rretieve corresponding page to the given index */
static struct fmd_page_t *fmd_alloc_page(struct fmd_device_t *fmd, pgoff_t index)
{
    struct fmd_cache_t *cache;

    BUG_ON(!fmd || !fmd->cache);
    cache = (struct fmd_cache_t *) fmd->cache;

    printk(KERN_INFO "%s: %s\n", fmd->dev_name, __func__);

    if (index > cache->nr_pages_cache) {
	    return NULL;
    }

    BUG_ON (!cache->pagepool);
    return (struct fmd_page_t *) &cache->pagepool[index];
}

/*-------------------------------------------------------------*/
/*-------------   Radix Tree (Page) Functions   ---------------*/
/*-------------------------------------------------------------*/

#define MAX_BATCH 16  /* Max # radix tree entries to view at once */

void 
fmd_radix_tree_init(struct fmd_device_t *fmd)
{
	struct fmd_cache_t *cache;

        BUG_ON(!fmd || !fmd->cache);
        cache = (struct fmd_cache_t *) fmd->cache;

	printk(KERN_INFO "%s: %s\n", fmd->dev_name, __func__);

	INIT_RADIX_TREE(&cache->tree, GFP_ATOMIC);
}

/* Free all pages in the radix tree, and eviction list */
void 
fmd_radix_tree_free_pages(struct fmd_device_t *fmd)
{
        unsigned long pos = 0;
        int nr_found;
        struct fmd_page_t *batch[MAX_BATCH];
        struct fmd_page_t *page;
        struct fmd_cache_t *cache;

        BUG_ON(!fmd || !fmd->cache);
        cache = (struct fmd_cache_t *) fmd->cache;

	printk(KERN_INFO "%s: %s:\n", fmd->dev_name, __func__);
        do {
                int i;
                nr_found = radix_tree_gang_lookup(&cache->tree, (void **)batch,
                                                  pos, MAX_BATCH);
//XXX:
printk(KERN_INFO "%s: %s: nr_found=%d\n", fmd->dev_name, __func__, nr_found);
                for (i=0; i<nr_found; i++) {
                        page = batch[i];
                        WARN_ON(page->index < pos);
                        pos = page->index;
                        fmd_radix_tree_free_page(fmd, page);
                }
                pos++;
        }
        while (nr_found == MAX_BATCH);
}

/* Free the specified page from the radix tree and eviction list */
void
fmd_radix_tree_free_page(struct fmd_device_t *fmd, struct fmd_page_t *page)
{
        struct fmd_cache_t *cache;
	struct fmd_page_t *ret;

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
        }
}

/*
 * Look up and return a fmd's page for a given sector.
 */
struct fmd_page_t *
fmd_radix_tree_lookup_page(struct fmd_device_t *fmd, sector_t sector)
{
        pgoff_t index;
        struct fmd_page_t *page;
	struct fmd_cache_t *cache;

        BUG_ON(!fmd || !fmd->cache);

	cache = (struct fmd_cache_t *) fmd->cache;

	printk(KERN_INFO "%s: %s\n", fmd->dev_name, __func__);

        rcu_read_lock();
        index = sector >> PAGE_SECTORS_SHIFT;  /* sector to page index */
	printk(KERN_INFO "%s: %s: index %ld\n", fmd->dev_name, __func__, index);
        page = (struct fmd_page_t *) radix_tree_lookup(&cache->tree, index);
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
struct fmd_page_t *
fmd_radix_tree_insert_page(struct fmd_device_t *fmd, sector_t sector)
{
        pgoff_t index;
        struct fmd_page_t *page;
	struct fmd_cache_t *cache;
	int rval;

        BUG_ON(!fmd | !fmd->cache);
	cache = (struct fmd_cache_t *) fmd->cache;

	printk(KERN_INFO "%s: %s\n", fmd->dev_name, __func__);

        /* If page already exists in radix_tree, return it */
        page = fmd_radix_tree_lookup_page(fmd, sector);
        if (page) {
                return page;
        }

        /* Retrieve corresponding page for index */
	index = sector >> PAGE_SECTORS_SHIFT;
	page = fmd_alloc_page(fmd, index);
        if (!page) {
                return NULL;
        }

        /* Insert newly allocated page into radix_tree */
        if (radix_tree_preload(GFP_NOIO)) {
                return NULL;
        }

	spin_lock(&fmd->lock);
	rval =  radix_tree_insert(&cache->tree, index, page);
        if (rval == -EEXIST) {
                page = radix_tree_lookup(&cache->tree, index);
                BUG_ON(!page);
                BUG_ON(page->index != index);
	} else if (rval == -ENOMEM) {
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
fmd_radix_tree_mark_dirty_page(struct fmd_device_t *fmd, struct fmd_page_t *page)
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
fmd_radix_tree_flush_dirty_page(struct fmd_device_t *fmd, struct fmd_page_t *page)
{
        struct fmd_cache_t *cache;
        BUG_ON(!fmd || !fmd->cache || !page);

	printk(KERN_INFO "%s: %s: page %ld\n", fmd->dev_name, __func__, page->index);

        cache = (struct fmd_cache_t *) fmd->cache;
        if (radix_tree_tag_get(&cache->tree, page->index, PAGECACHE_TAG_DIRTY)) {
                /* Flush page to disk then clear tag */
                int offset = page->virt - fmd->virt;
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
        int nr_found;
        struct fmd_page_t *batch[MAX_BATCH];
        struct fmd_page_t *page = NULL;
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
                nr_found = radix_tree_gang_lookup_tag(& cache->tree,
                                                      (void **)batch, pos, 
                                                      MAX_BATCH, 
                                                      PAGECACHE_TAG_DIRTY);

                for (i=0; i<nr_found; i++) {
                        page = batch[i];
                        BUG_ON(page->index < pos);
                        pos = page->index;

                        /* Flush page to disk then clear tag */
                        fmd_radix_tree_flush_dirty_page(fmd, page);
                }
		pos++;
        } while (nr_found == MAX_BATCH);
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
fmd_evict_list_add(struct fmd_device_t *fmd, struct fmd_page_t *page) {

	struct fmd_cache_t *cache = (struct fmd_cache_t *) fmd->cache;

	printk(KERN_INFO "%s: %s\n", fmd->dev_name, __func__);

	list_add_tail(&page->lru, &cache->evict_list);
}

static inline void 
fmd_evict_list_delete(struct fmd_device_t *fmd, struct fmd_page_t *page) {

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
        struct fmd_page_t *page;
	struct fmd_cache_t *cache;

	BUG_ON(!fmd);
        cache = (struct fmd_cache_t *) fmd->cache;

	printk(KERN_INFO "%s: %s\n", fmd->dev_name, __func__);

        /* Retrieve page from head of list */
        for (i=0; i<cache->evict_num_entries; i++) {
                if (list_empty(&cache->evict_list)) {
//XXX:
printk(KERN_INFO "%s: %s: evict_list EMPTY!\n", fmd->dev_name, __func__);

                        break;
                }

                /* Evict pages from head of list */
                page = list_entry(cache->evict_list.next, struct fmd_page_t, lru);
                BUG_ON(!page);

                /* delete page (also flushes dirty page) */
                fmd_radix_tree_free_page(fmd, page);
        }
}


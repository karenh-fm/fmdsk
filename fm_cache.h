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

#ifndef FMDSK_CACHE_H
#define FMDSK_CACHE_H

struct fmd_cache_t {
    /* Physically contiguous memory used for cache.  Allocated at system boot.
     * Discovered by driver */
    phys_addr_t phys;
    void __iomem *virt;
    unsigned int nr_pages;

    /* Cache Memory pool slices the memory into page sized chunks */
    struct kmem_cache *slab;
    mempool_t   *mempool;

    /* Pool of dynamically allocated page structs,
     * Used for the MANUAL contiguous physical memory allocation method
     * when the CACHE_PAGES driver feature is enabled. 
     *  
     * NOTE: These page structs are only used within the driver and are not 
     * compatible with the kernel page struct.
     */
    void *page_slab;
    mempool_t *page_pool;

    /* Cache Radix tree used to manage pages.
     * Allows for fast page lookup and deletion of pages
     *
     * NOTE: Each block ramdisk device has a radix_tree fmd_pages of pages that
     * stores the pages containing the block device's contents. An fmd
     * page's ->index is its offset in PAGE_SIZE units. This is similar to,
     * but in no way connected with, the kernel's pagecache or buffer cache
     * (which sit above our block device).
     */ 
    struct radix_tree_root tree;

    /* Cache eviction variables */
    struct list_head evict_list;
    unsigned char evict_hiwat;
    unsigned char evict_num_entries;
    unsigned char page_cnt;
    unsigned char rsvd;
};

int fmd_mempool_init(struct fmd_device_t *fmd);
void fmd_mempool_cleanup(struct fmd_device_t *fmd);

void fmd_radix_tree_init(struct fmd_device_t *fmd);
void fmd_radix_tree_free_pages(struct fmd_device_t *fmd);
void fmd_radix_tree_free_page(struct fmd_device_t *fmd, struct page *page);
struct page *fmd_radix_tree_insert_page(struct fmd_device_t *fmd, sector_t sector);
struct page *fmd_radix_tree_lookup_page(struct fmd_device_t *fmd, sector_t sector);
inline void fmd_radix_tree_mark_dirty_page(struct fmd_device_t *fmd, struct page *page);
void fmd_radix_tree_flush_dirty_pages(struct fmd_device_t *fmd);

void fmd_evict_list_init(struct fmd_device_t *fmd, int hiwat, int evict);
inline unsigned char fmd_cache_full(struct fmd_device_t *fmd);
void fmd_evict_pages(struct fmd_device_t *fmd);

#endif /* FMDSK_CACHE_H */


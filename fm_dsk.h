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

#ifndef FM_DSK_H
#define FM_DSK_H

/* Driver features support */
#define CACHE_PAGES 0   /* 1 = support paging of flash memory via DRAM window */
			/* 0 = FUTURE: flash memory and DRAM memory two separate devices
			       NOW: Detect flash memory only and support as a single device*/
#define DAX_SUPPORT 0	/* 1 = support DAX byte addressibility (direct_access) */
			/* Do NOT enable if CACHE_PAGES == 1 */


#define FMD_DEV_TYPE_DSK 1
#define FMD_DEV_TYPE_MEM 2

#define DEV_NAME_LEN 16
#define DEV_NAME_DSK "fmdsk"
#define DEV_NAME_MEM "fmmem"
#define DRIVER_NAME  "fmdsk"

#define BYTES_PER_SECTOR	512
#define SECTOR_SHIFT		9
#define PAGE_SECTORS_SHIFT	(PAGE_SHIFT - SECTOR_SHIFT)
#define PAGE_SECTORS		(1 << PAGE_SECTORS_SHIFT)


struct fmd_device_t {
	int num;
	int dev_type;
	char dev_name[DEV_NAME_LEN];

	spinlock_t lock;
	struct list_head list;

	struct request_queue *queue;
	struct gendisk *disk;

        phys_addr_t phys;
        void __iomem *virt;
        unsigned int nr_pages;
	
	void *cache;
};


#endif /* FM_DSK_H */

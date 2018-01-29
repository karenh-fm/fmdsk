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

#ifndef FM_MEM_H
#define FM_MEM_H

#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,12,0)
#include <asm/e820/api.h>
#else
#include <asm/e820.h>
#define E820_TYPE_PMEM 7
#endif
#include "fm_dsk.h"


//TODO: Future Add logic for multiple  e820 memory types
//#define NUM_E820_TYPES 2
//int e820_types[NUM_E820_TYPES] = { E820_TYPE_PMEM, E820_TYPE_RESERVED_KERN };

int fmd_memory_alloc_manual_dsk(struct fmd_device_t *fmd, int e820_type, unsigned int nr_pages);
int fmd_memory_alloc_manual_cache(struct fmd_device_t *fmd, int e820_type, unsigned int nr_pages);
void fmd_memory_cleanup_manual(struct fmd_device_t *fmd);

#endif /* FM_MEM */

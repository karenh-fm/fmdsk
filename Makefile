# Fusion Memory Confidential
# __________________
# 
#  Fusion Memory Incorporated 
#  All Rights Reserved.
# 
# NOTICE:  All information contained herein is, and remains
# the property of Fusion Memory and its suppliers, if any.
# The intellectual and technical concepts contained herein are 
# proprietary to Fusion Memory and its suppliers and may be covered by
# U.S. and Foreign Patents, patents in process, and are protected by 
# trade secret or copyright law. Dissemination of this information or 
# reproduction of this material is strictly forbidden unless prior 
# written permission is obtained from Fusion Memory.
#

VERSION = 1.0

ifeq ($(KSRC),)
	KSRC := /lib/modules/$(shell uname -r)/build
endif

ifeq ($(KVER),)
        KVER := $(shell uname -r)
endif

MKDIR := mkdir -pv

# Enable debug symbols
ccflags-y=-g

obj-m := fmdsk.o
fmdsk-y := fm_cache.o fm_dsk.o fm_mem.o



all:
	$(MAKE) -C $(KSRC) M=$(PWD) modules

install:
	$(MKDIR) $(DESTDIR)/lib/modules/$(KVER)/kernel/drivers/block/
	install -o root -g root -m 0755 fmdsk.ko $(DESTDIR)/lib/modules/$(KVER)/kernel/drivers/block/
	depmod -a

uninstall:
	rm -f $(DESTDIR)/lib/modules/$(KVER)/kernel/drivers/block/fmdsk.ko
	depmod -a

clean:
	rm -rf *.o *.ko *.symvers *.mod.c .*.cmd Module.markers modules.order


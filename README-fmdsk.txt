Fusion Memory Block Driver

~~~~~~~~~~~~~~~~
~ Build Driver ~
~~~~~~~~~~~~~~~~

Prerequisites:
- Root or sudo priviledge is required on some systems.
- The kernel sources or header must be installed.

Instructions:
1. Change to the driver project's directory.

2. Type the following command to build the driver:
	# make

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
~ Manually Load/Unload Driver ~
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Use this method to immediately load and unload the driver from the command line.
The driver load will not automatically reload on reboot.

To load driver, type the following command:
	# modprobe fmdsk.ko [optional command line parameters]

To unload driver, type the following command:
	# modprobe -r fmdsk

~~~~~~~~~~~~~~~~~~~~~~~~~~~~
~ Install/Uninstall Driver ~
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Use this method to load the driver automatically upon system reboot.
This method will not load the driver immediately.  Use the manual driver load
method also to load the driver immediately.

To install driver, type the following commands:
	# make
	# make install

To uninstall driver, type the following command:
	# make uninstall

~~~~~~~~~~~~~~~~~~~~~~~~~~~
~ Command Line Parameters ~
~~~~~~~~~~~~~~~~~~~~~~~~~~~

Coming Soon ...

~~~~~~~~~~~~~~~~
~  Deployment  ~
~~~~~~~~~~~~~~~~

After the driver is loaded, the /dev/fmdsk0 raw device will be created.
The device may be accessed in two different ways:

1. The raw device /dev/fmdsk0 may be used in some test utilities (i.e. fio).
 
2. Mount a filesystem on the device to perform disk operations or use in test 
   utility (i.e. fio).

   - To create a partition on the disk, type the following command:
	# fdisk /dev/fmdsk0
	
	Usage:
		m - help menu
		n - create a new partition
		w - write partition information to disk

   - To install a filesystem onto the newly created partition /dev/fmdsk0p1,
     type the following command:
	# mkfs.ext4 /dev/fmdsk0p1

   - To create a mount point to mount newly created filesystem, type the
     following:
	# mkdir /mnt/fmdsk

   - To mount the newly created filesystem onto the mount point, type the
     following:
	# mount -t ext4 -o noatime  /dev/fmdsk0p1  /mnt/fmdsk

   - To umount, type the following:
	# umount /mnt/fmdsk

~~~~~~~~~~~~~~~~
~   Contact    ~
~~~~~~~~~~~~~~~~

Coming Soon ...

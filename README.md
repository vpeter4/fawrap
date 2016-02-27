fawrap
==========
A preload shared library to limit file access to only some portion
of the file. Usable for creating disk images and accessing
partitions inside them with tools like mke2fs, tune2fs, e2fsck
and populatefs.
Some system functions are overriden in library and preloaded via
the **LD_PRELOAD** environment variable when using such programs.

Usage
=====
```
  LD_PRELOAD=./fawrap.so FILE=file.img,offset,length program
```
  Program can only access data in file *file.img* from *offset* bytes with
  *length* bytes:
  
```
     ----------------------------------------------------
    |.......|0000000000000000000000000|..................|
     ----------------------------------------------------
            ^                         ^
            |offset                   |
            |                         |
             <-------- length ------->
```

Example
=====
disk image with 2 partitions, first FAT32 40MB, second EXT4 32MB
```
  dd if=/dev/zero of=disk.img bs=1M count=76 conv=fsync
  parted -s disk.img mklabel msdos
  parted -s disk.img -a min unit s mkpart primary fat32 2048 83968
  parted -s disk.img set 1 boot on
  parted -s disk.img -a min unit s mkpart primary ext4 86016 151552

  # first FAT32 partition
  shopt -s expand_aliases  # enables alias expansion in script
  alias mformat="mformat -i $DISK@@$OFFSET -h $HEADS -t $TRACKS -s $SECTORS"
  alias mcopy="mcopy -i $DISK@@$OFFSET"
  alias mmd="mmd -i $DISK@@$OFFSET"

  mformat ::
  mmd some_folder
  mcopy syslinux.cfg ::

# second EXT4 partition
  export LD_PRELOAD=./fawrap.so
  export FILE=disk.img,44040192,33554944
  mke2fs -F -q -t ext4 -m 0 disk.img
  e2fsck -n disk.img
  populatefs -U -d dir3copy disk.img
  unset LD_PRELOAD
  unset FILE
```

Advanced use
============
Last argument of FILE environment variable can be i or d.
- i: print of system calls used to access required file
- d: print of system calls used to access all the files

Credits
=======
Thanks to Marcus R. for his valuable input.

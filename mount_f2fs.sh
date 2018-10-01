#!/bin/sh

sudo rmmod f2fs
sudo insmod fs/f2fs/f2fs.ko
sudo mount -t f2fs -o pmem=/dev/pmem0 /dev/sda4 /mnt/f2fs

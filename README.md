# hybridF2FS
A varient of F2FS using hybrid logging with NVM and SSD

## License
GNU General Public License version 2 (GPLv2)

## Linux versions supported
Default Branch is 5.7, but Linux Kernel above v3.8 can adopt hybridF2FS by git merge. You can adopt our code to that versions by fixing little conflict.

## Description
F2FS is a Linux file system designed to perform well on Nand-flash based storages such as SSD. F2FS writes Data and Node(File System Metadata) in log-structured manner. It uses multi-head logging(hot-cold separation) to utilize SSD's internal parallelism. But F2FS Design is not optimized to manycore environment. This leads to lack of I/O scalability in manycore server. hybridF2FS suggests scalability solutions for F2FS. In this branch, we suggest the scalability solution for fsync().

hybridF2FS adopt NVM+DRAM hybrid structure to use NVM as metadata storage. For resolving performance degradation caused by fsync, hybridF2FS uses NVM as a node Log and File System Metadata Area. All File System Metadata Area is moved to NVM, so there is no need to flush metadata during Checkpoint. By using NVM's byte-addressability, we enable F2FS to write modified File System Metadata directly to NVM.  As a result, we can improve fsync latency and checkpointing issue in F2FS.

## Requirement List

NVM Node Logging version hybridF2FS and Atomic Range Lock + nCache version hybridF2FS is not compatible.

## HOW TO USE hybridF2FS?

Emulation with Intel PMEM Driver
```
https://software.intel.com/en-us/articles/how-to-emulate-persistent-memory-on-an-intel-architecture-server
```

### Build Kernel
```
1. sudo cp /boot/config-generic_version .config
2. make menuconfig
3. make -j number_of_cores
4. sudo make modules_install
5. sudo make install
```
### Mount File System
```
1. sudo mkfs.f2fs -f device
2. sudo mount -t f2fs -o pmem=<pmem_device> <device_to_mount> <mount_dir>
ex . sudo mount -t f2fs -o pmem=/dev/pmem0 /dev/nvme0n1 /mnt/f2fs
```

## Developer Guide
Our implementation is about F2FS. If you want to check or modify our code, use c files in fs/f2fs/. If you have questions, feel free to contact us.

### NVM node logging path
For NVM node logging, we adopted balloc.c and balloc.h from NOVA File System. and we mainly modified node.c, checkpoint.c


### How to Change F2FS after modification 
```
1. cd KERNEL_DIRECTORY
2. sudo rmmod f2fs
3. make fs/f2fs/f2fs.ko
4. sudo insmod fs/f2fs/f2fs.ko
```

## Evaluation

## Environment

|  <center></center> |  <center>Specification</center> |  
|:--------:|:--------:|
| <center>Processor</center> | <center>Intel Xeon E7-8870 v2</center> |
| <center>Number of Cores</center> | <center>120</center> |
| <center>Memory</center>  | <center>740GB</center> |
| <center>Storage</center>  | <center>Samsung EVO 970 250GB</center> |
| <center>Kernel Version</center>  | <center>Linux 5.7</center> |

### FxMark DWSL (File write with sync(O_SYNC))
<img src="https://user-images.githubusercontent.com/45027411/140457958-866462cd-8f0f-481e-823c-4d3b19e91c27.jpg" width="400" height="140"/>


## Papers about hybridF2FS

1. Chang-Gyu Lee, Hyunki Byun, Sunghyun Noh, Hyeongu Kang, and Youngjae Kim. 2019. Write optimization of log-structured flash file system for parallel I/O on manycore servers. In Proceedings of the 12th ACM International Conference on Systems and Storage> (SYSTOR '19). Association for Computing Machinery, New York, NY, USA, 21–32. DOI:https://doi.org/10.1145/3319647.3325828
Link : https://dl.acm.org/doi/10.1145/3319647.3325828


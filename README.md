# hybridF2FS

## License
GNU General Public License version 2 (GPLv2)

## Linux versions supported
Default Branch is 5.7, but Linux Kernel above v3.8 can adopt hybridF2FS by git merge. You can adopt our code to that versions by fixing little conflict.

## Description
F2FS is a Linux file system designed to perform well on Nand-flash based storages such as SSD. F2FS writes Data and Node(File System Metadata) in log-structured manner. It uses multi-head logging(hot-cold separation) to utilize SSD's internal parallelism. But F2FS Design is not optimized to manycore environment. This leads to lack of I/O scalability in manycore server. hybridF2FS suggests scalability solutions for F2FS. In this branch, we suggest the scalability solution for shared file I/O.

F2FS serialize threads when writing shared files. hybridF2FS adopt range locks on shared files to solve this serialization problem, allowing file I/O to be executed concurrently. However, we have found that even with a range lock, I/O throughput no longer increases after a certain number of cores and decreases rapidly on a manycore server.

Through extensive performance profiling, we found the cascading tree lock problem in Page Cache that serializes concurrent accesses to the file metadata structure. A mutex lock-based locking mechanism for each file metadata structure serializes I/O requests in modern Linux file systems including F2FS.
So we developed nCache into hybridF2FS, a novel file metadata cache framework using readers-writer lock that allows concurrent I/O operations for the shared file. hybridF2FS solves the I/O scalability problem in the manycore server while ensuring consistent updates.

hybridF2FS adopt NVM+DRAM hybrid structure to use NVM as metadata storage. This enables to move NAT and node logs into NVM space, which leads to reduce overhead of checkpoint. You can see more information in the NVM-logging branch.

## Requirement List

NVM Node Logging version hybridF2FS and Atomic Range Lock + nCache version hybridF2FS is not compatible.

## HOW TO USE hybridF2FS?

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
2. sudo mount -t f2fs device mount_path
```

## Developer Guide
Our implementation is about F2FS. If you want to check or modify our code, use c files in fs/f2fs/. If you have questions, feel free to contact us.

### range_lock path
For adopting range_lock into our I/O path, we modified file.c and data.c

### nCache path
For nCache, we modified node.c which is part of File Metadata Path.

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

### FxMark DWOM (shared file write)
<img src="https://user-images.githubusercontent.com/45027411/140457425-45f82029-fbad-48ef-bc58-01afc4db84fe.jpg" width="500" height="150"/>

### FxMark DRBM (shared file read)
<img src="https://user-images.githubusercontent.com/45027411/140457572-1ca9905d-4336-4069-ae97-d02ce3c34817.jpg" width="500" height="150"/>

### RocksDB db_bench (R/W mixed)
<img src="https://user-images.githubusercontent.com/45027411/140457763-c519d80c-4cbe-4271-9a78-e2fcbb81e806.jpg" width="400" height="200"/>


## Papers about hybridF2FS

1. Chang-Gyu Lee, Hyunki Byun, Sunghyun Noh, Hyeongu Kang, and Youngjae Kim. 2019. Write optimization of log-structured flash file system for parallel I/O on manycore servers. In Proceedings of the 12th ACM International Conference on Systems and Storage> (SYSTOR '19). Association for Computing Machinery, New York, NY, USA, 21–32. DOI:https://doi.org/10.1145/3319647.3325828
Link : https://dl.acm.org/doi/10.1145/3319647.3325828
2. Chang-Gyu Lee, Sunghyun Noh, Hyeongu Kang, Soon Hwang, and Youngjae Kim. 2021. Concurrent file metadata structure using readers-writer lock. In Proceedings of the 36th Annual ACM Symposium on Applied Computing (SAC '21). Association for Computing Machinery, New York, NY, USA, 1172–1181. DOI:https://doi.org/10.1145/3412841.3441992 
Link : https://dl.acm.org/doi/10.1145/3412841.3441992


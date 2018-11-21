# hybridF2FS
A varient of F2FS using hybrid logging with NVM and SSD

NMV Node Log
----
For resolving performance degradation caused by fsync, hybridF2FS uses NVM as a node Log and filesystem metadata area. As a result, we can improve fsync latency and checkpointing issue in F2FS.

Preliquisite
----
Builiding and Install Kernel
```
https://kernelnewbies.org/KernelBuild
```

Emulation with Intel PMEM Driver
```
https://software.intel.com/en-us/articles/how-to-emulate-persistent-memory-on-an-intel-architecture-server
```

Usage
----
Make module
```
make fs/f2fs/f2fs.ko
```

Install module
```
sudo insmod fs/f2fs/f2fs.ko
```

Mount with pmem device
```
sudo mount -t f2fs -o pmem=<pmem_device> <device_to_mount> <mount_dir>
```
ex.
```
sudo mount -t f2fs -o pmem=/dev/pmem0 /dev/nvme0n1 /mnt/f2fs
```

TODO
----
We have to allocate nvm area sequentially to log.

We will move to resolving scalability issues.

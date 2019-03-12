

## BoogiePop: A library for Peripheral-to-Peripheral communication

Requirements
- Linux kernel 4.20
- i40e NIC
- NVMe device
- something good motherboad



1. install netmap from https://github.com/upa/netmap

confirm i40e depends on the netmap module.
```
% lsmod|grep netmap
netmap                196608  1 i40e
```

Note that i40e NICs automatically send LLDP packets periodically.  To
disable this,
```
sudo echo lldp stop > /sys/kernel/debug/i40e/0000:17:00.0/command
```


2. compile and install

```shell-session
git clone https://github.com/upa/boogiepop
cd boogiepop
git submodule init && git submodule update

# make install unvme
cd unvme
make install && sudo make install

# make boogiepop
make

# install boogiepop kernel module
sudo insmod kmod/boogiepop.ko
```


3. set hugepages
```
#!/bin/bash

echo 2048 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
echo 0 > /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages
mkdir -p /mnt/hugepages
mount -t hugetlbfs nodev /mnt/hugepages
```


4. set NVMe device under UNVMe without iommu

```shell-session
modprobe -r vfio_pci
modprobe -r vfio_iommu_type1
modprobe -r vfio
modprobe vfio enable_unsafe_noiommu_mode=1

unvme-setup bind 17:00.0 # pci slot of NVMe
```

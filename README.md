

## BoogiePop: A library for Peripheral-to-Peripheral communication

Requirements
- Linux kernel 4.20
- i40e NIC
- NVMe device
- something good motherboad
- something good p2pmem card




### Compile and Install

```shell-session
git clone https://github.com/upa/boogiepop
cd boogiepop/
git submodule init && git submodule update
```


1. Compile and install a modified netmap. This netmap installs only
i40e and ixgbe drivers. Tested on only kernel 4.20.

```shell-session
cd boogiepop/netmap/LINUX
./configure
make
sudo make install
```


2. Compile and install Boogiepop. compiling boogiepop depends on the
netmap.

```shell-session
cd boogiepop/
make
sudo make install
```


3. Compile a modified UNVMe. This depends on the boogiepop library.

```shell-session
cd boogiepop/unvme/
make
sudo make install
```



# How to

3. set hugepages
```
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

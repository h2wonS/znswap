#!/bin/bash

DEV=$1
sudo swapoff ${DEV}

sudo nvme zns reset-zone -a ${DEV}

echo 548 | sudo tee /sys/kernel/mm/zns_swap/nr_swap_zones

#per-core policy
echo 1 | sudo tee /sys/kernel/mm/zns_swap/zns_policy

#fig 1, fig10 is no need for this
#echo 1 | sudo tee /sys/kernel/mm/zns_swap/zns_cgroup_account

#echo 400 | sudo tee /sys/kernel/mm/zns_swap/low_wmark
#echo 500 | sudo tee /sys/kernel/mm/zns_swap/high_wmark

sudo /home/hwshin/util-linux/mkswap ${DEV}

sudo /home/hwshin/util-linux/swapon ${DEV}

sudo mkdir /sys/fs/cgroup/vm
sudo mkdir /sys/fs/cgroup/vm/tasks

sudo echo deadline > /sys/block/nvme0n1/queue/scheduler
sudo cat /sys/block/nvme0n1/queue/scheduler
sudo echo 512 > /sys/block/nvme0n1/queue/max_sectors_kb

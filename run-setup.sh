#!/bin/bash

sudo swapoff /dev/nvme1n1

sudo nvme zns reset-zone -a /dev/nvme1n1

echo 548 | sudo tee /sys/kernel/mm/zns_swap/nr_swap_zones

#per-core policy
echo 1 | sudo tee /sys/kernel/mm/zns_swap/zns_policy

#fig 1, fig10 is no need for this
#echo 1 | sudo tee /sys/kernel/mm/zns_swap/zns_cgroup_account

#echo 400 | sudo tee /sys/kernel/mm/zns_swap/low_wmark
#echo 500 | sudo tee /sys/kernel/mm/zns_swap/high_wmark

sudo mkswap /dev/nvme1n1

sudo swapon /dev/nvme1n1

sudo mkdir /sys/fs/cgroup/vm
sudo mkdir /sys/fs/cgroup/vm/tasks


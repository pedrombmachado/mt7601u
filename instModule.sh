#! /bin/bash
echo "Ubuntu2015" | sudo -S modprobe mac80211
sudo insmod ./mt7601u.ko


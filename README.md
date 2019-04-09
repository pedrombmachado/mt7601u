This is a Linux driver for MediaTek MT7601U USB dongle. It was written from scratch based on the vendor GPL-driver. Unlike the vendor driver this driver uses modern Linux WiFi infrastructure and should work flawlessly with NetworkManager, wicd, wpa_supplicant and such. This driver was merged into mainline and is part of official Linux kernel since version v4.2. If you are using Linux 4.2 or later there is no need to install this driver.

### Tested on:
* Ubuntu 16.04 LTS
* Ubuntu 18.04 LTS

### Building and using
To use this driver you need to upgrade your kernel to at least **Linux 3.19**. You also have to grab a copy of the firmware from the vendor driver. Download the vendor driver (see section below) and copy file *MT7601U.bin* to */lib/firmware*:

```
$ sudo cp MT7601.bin /lib/firmware/mt7601u.bin
```
Note that name of the file in */lib/firmware* is in lowercase.

After that **make sure you have installed all packages required by your distro to build kernel modules**
```
$ sudo apt install linux-headers-$(uname -r)
$ git clone https://github.com/kuba-moo/mt7601u.git
$ cd mt7601u
$ make
$ sudo rmmod mt7601u.ko
$ sudo modprobe mac80211
$ sudo insmod ./mt7601u.ko
```

Now when you connect your device a new network interface should be created. Something like this should appear in your kernel logs:

```
[ 5515.098424] mt7601u 1-6:1.0: ASIC revision: 76010001  MAC revision: 76010500
[ 5515.100954] mt7601u 1-6:1.0: Firmware Version: 0.1.00 Build: 7640 Build time: 201302052146____
[ 5515.466817] mt7601u 1-6:1.0: Warning: unsupported EEPROM version 0d
[ 5515.466876] mt7601u 1-6:1.0: EEPROM ver:0d fae:00
[ 5515.467561] mt7601u 1-6:1.0: EEPROM country region 01 (channels 1-13)
[ 5515.713155] ieee80211 phy26: Selected rate control algorithm 'minstrel_ht'
[ 5515.718977] usbcore: registered new interface driver mt7601u
```

The warning about EEPROM version is harmless but keep an eye on the logs and if you spot any errors please report them here.

If you want the driver to load automatically you can do the following:
```
$ make && sudo make install && depmod
```
However, please remember that this installs the driver *only for your current kernel* and you will have to redo this every time your kernel is updated!

### Run the driver
```
$ sudo instModule.sh
```


#!/bin/bash
mkdir kernel4_13
cd kernel4_13
wget https://kernel.ubuntu.com/~kernel-ppa/mainline/v4.13.16/linux-headers-4.13.16-041316_4.13.16-041316.201711240901_all.deb
wget https://kernel.ubuntu.com/~kernel-ppa/mainline/v4.13.16/linux-headers-4.13.16-041316-generic_4.13.16-041316.201711240901_amd64.deb
wget https://kernel.ubuntu.com/~kernel-ppa/mainline/v4.13.16/linux-image-4.13.16-041316-generic_4.13.16-041316.201711240901_amd64.deb
sudo dpkg -i *.deb
sudo sed -i 's/GRUB_DEFAULT/#GRUB_DEFAULT/g' /etc/default/grub
sudo sed -i '1s/^/GRUB_DEFAULT="Advanced options for Ubuntu>Ubuntu, with Linux 4.13.16-041316-generic"\n/' /etc/default/grub
sudo update-grub
sudo reboot

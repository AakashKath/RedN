#!/bin/bash
sudo apt install -y createrepo
mkdir mlnx
cd mlnx
wget http://www.mellanox.com/downloads/ofed/MLNX_OFED-4.7-1.0.0.1/MLNX_OFED_LINUX-4.7-1.0.0.1-ubuntu18.04-x86_64.tgz
tar -xvf MLNX_OFED_LINUX-4.7-1.0.0.1-ubuntu18.04-x86_64.tgz
cd MLNX_OFED_LINUX-4.7-1.0.0.1-ubuntu18.04-x86_64
yes | sudo ./mlnxofedinstall --add-kernel-support --kernel 4.13.16-041316-generic --without-fw-update --force-dkms
sudo /etc/init.d/openibd restart

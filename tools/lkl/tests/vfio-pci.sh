#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0

script_dir=$(cd $(dirname ${BASH_SOURCE:-$0}); pwd)
source $script_dir/test.sh

fstype="ext4"
pciname="0000:00:03.0"
nvme_id="8086 5845"
img="ubuntu.img"
original_img="ubuntu-18.04-server-cloudimg-amd64.img"
img_url="https://cloud-images.ubuntu.com/releases/bionic/release/$original_img"
qemu_option="-machine q35,kernel-irqchip=split\
 -m 2048 -device intel-iommu,intremap=on -nographic -hdb cloud.img\
 -net nic,model=e1000 -net user,hostfwd=tcp::2222-:22\
 -drive file=nvme.img,if=none,id=D22 -device nvme,drive=D22,serial=1234\
 -serial mon:telnet::5555,server,nowait"
check_qemu="ssh -o LogLevel=quiet -o ConnectTimeout=1\
 -o StrictHostKeyChecking=no -i ./id_rsa -p 2222 lkl@localhost exit"

function wait_ssh_connection()
{
    for i in `seq 300`; do
	if $check_qemu 2> /dev/null; then
	    break
	fi
	sleep 1
    done
}

function wait_guest()
{
    while [ ! -e ${img} ]; do
	sleep 5
    done
    wait_ssh_connection
}

SSH_QEMU="ssh -i $script_dir/id_rsa -p 2222 lkl@localhost"

function prepenv()
{
    if [ ! -e cloud.img ]; then
	rm -f id_rsa id_rsa.pub
	ssh-keygen -f id_rsa -N ""
	cat <<EOF > cloud.txt
#cloud-config
users:
  - name: lkl
    ssh-authorized-keys:
      - `cat id_rsa.pub`
    sudo: ['ALL=(ALL) NOPASSWD:ALL']
    groups: sudo
    shell: /bin/bash
EOF
	sudo apt update
	sudo apt install -y cloud-image-utils
	cloud-localds cloud.img cloud.txt
    fi

    if [ ! -e nvme.img ]; then
	dd if=/dev/zero of=nvme.img bs=1G count=8
	mkfs.$fstype nvme.img
    fi

    sudo apt update
    sudo apt install -y wget pkg-config libglib2.0-dev libpixman-1-dev
    if [ ! -e qemu-5.0.0/x86_64-softmmu/qemu-system-x86_64 ]; then
	rm -rf qemu-5.0.0.tar.xz qemu-5.0.0
	wget https://download.qemu.org/qemu-5.0.0.tar.xz
	tar xf qemu-5.0.0.tar.xz
	pushd qemu-5.0.0
	./configure --target-list=x86_64-softmmu
	make -j8
	popd
    fi
    pushd qemu-5.0.0
    sudo make install
    popd

    if [ ! -e ${img} ]; then
	rm -f ${original_img}
	sudo apt update
	sudo apt install -y wget
	wget -q ${img_url}
	qemu-img resize ${original_img} 10G
	ssh-keygen -R [localhost]:2222
	sudo qemu-system-x86_64 ${qemu_option} -hda ${original_img}\
 >> qemu.log 2>&1 &
	wait_ssh_connection
	$SSH_QEMU "sudo sed -i -e 's/GRUB_CMDLINE_LINUX_DEFAULT=\"\
/GRUB_CMDLINE_LINUX_DEFAULT=\"intel_iommu=on /' \
/etc/default/grub.d/50-cloudimg-settings.cfg"
	$SSH_QEMU sudo update-grub
	$SSH_QEMU sudo poweroff
	while true; do
	    pcount=`ps aux | grep qemu\\-system\\-x86 | grep -v grep | wc -l`
	    if [ $pcount = 0 ]; then
		break
	    fi
	    sleep 1;
	done
	mv ${original_img} ${img}
    fi

    sudo qemu-system-x86_64 ${qemu_option} -hda ${img} >> qemu.log
}

function init()
{
    # initialize
    $SSH_QEMU sudo modprobe vfio-pci
    $SSH_QEMU "sh -c 'echo $nvme_id |
    	       	       sudo tee /sys/bus/pci/drivers/vfio-pci/new_id'"
    $SSH_QEMU "sh -c 'echo $pciname |
    	       	       sudo tee /sys/bus/pci/drivers/nvme/unbind'"
    $SSH_QEMU "sh -c 'echo $pciname |
    	       	       sudo tee /sys/bus/pci/drivers/vfio-pci/bind'"
    $SSH_QEMU sudo chown lkl:lkl /dev/vfio/3
    scp -i $script_dir/id_rsa -P 2222 $script_dir/vfio-pci lkl@localhost:
}

function run()
{
    if [ "$VFIO_PCI" != "yes" ]; then
	lkl_test_plan 0 "vfio-pci-nvme $fstype"
	echo "vfio not supported"
    else
	lkl_test_plan 1 "vfio-pci-nvme $fstype"
	lkl_test_run 1 init
	lkl_test_exec $SSH_QEMU ./vfio-pci -n 0000:00:03.0 -t $fstype
    fi
}

"$@"

#!/bin/bash

set -e

script_dir=$(cd $(dirname ${BASH_SOURCE:-$0}); pwd)
hijack_script=${script_dir}/../bin/lkl-hijack.sh

if ! [ -e ${script_dir}/../lib/liblkl-hijack.so ]; then
    echo "liblkl-hijack.so not found."
    exit -1;
fi

# Make a temporary directory to run tests in, since we'll be copying
# things there.
work_dir=$(mktemp -d)

# And make sure we clean up when we're done
function clear_work_dir {
    rm -rf ${work_dir}
    sudo ip link set dev lkl_ptt0 down
    sudo ip tuntap del dev lkl_ptt0 mode tap
}

trap clear_work_dir EXIT

echo "Running tests in ${work_dir}"

echo "== ip addr test=="
${hijack_script} ip addr

echo "== ip route test=="
${hijack_script} ip route

echo "== ping test=="
cp `which ping` .
${hijack_script} ./ping 127.0.0.1 -c 1
rm ./ping

echo "== ping6 test=="
cp `which ping6` .
${hijack_script} ./ping6 ::1 -c 1
rm ./ping6

echo "== TAP tests =="
if [ ! -c /dev/net/tun ]; then
    echo "/dev/net/tun required to run TAP tests."
    exit -1
fi

export LKL_HIJACK_NET_TAP=lkl_ptt0
export LKL_HIJACK_NET_IP=192.168.13.2
export LKL_HIJACK_NET_NETMASK_LEN=24

# Set up the TAP device we'd like to use
sudo ip tuntap del dev lkl_ptt0 mode tap || true
sudo ip tuntap add dev lkl_ptt0 mode tap user $USER
sudo ip link set dev lkl_ptt0 up
sudo ip addr add dev lkl_ptt0 192.168.13.1/24

# Make sure our device has the addresses we expect
addr=$(LKL_HIJACK_NET_MAC="aa:bb:cc:dd:ee:ff" ${hijack_script} ip addr) 
echo "$addr" | grep eth0
echo "$addr" | grep 192.168.13.2
echo "$addr" | grep "aa:bb:cc:dd:ee:ff"

# Copy ping so we're allowed to run it under LKL
cp `which ping` .

# Make sure we can ping the host from inside LKL
${hijack_script} ./ping 192.168.13.1 -c 1
rm ./ping

# Now let's check that the host can see LKL.
sudo arp -d 192.168.13.2
ping -i 0.2 -c 1 192.168.13.2 &
${hijack_script} sleep 1

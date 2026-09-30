#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only

script_dir=$(cd $(dirname ${BASH_SOURCE:-$0}); pwd)

source $script_dir/test.sh
source $script_dir/fs.sh

_usage()
{
    echo "usage: cptofs.sh -t <fstype>"
    exit 1
}

cptofs_file()
{
    set -e

    local tdir="$(lkl_test_cmd mktemp -d)"
    local cptofs=("${script_dir}/../cptofs" -p -t "$fstype" -i "$file")
    local cpfromfs=("${tdir}/cpfromfs" -p -t "$fstype" -i "$file")
    lkl_test_cmd ln -s "${script_dir}/../cptofs" "${tdir}/cpfromfs"

    lkl_test_cmd mkdir -p "${tdir}/round-trip"
    lkl_test_cmd echo "data" > ${tdir}/data
    lkl_test_cmd "${cptofs[@]}" "${tdir}/data" /
    lkl_test_cmd "${cpfromfs[@]}" /data "${tdir}/round-trip/"
    lkl_test_cmd diff "${tdir}/data" "${tdir}/round-trip/data"
    lkl_test_cmd rm -rf "$tdir"
}

cptofs_tree()
{
    set -e

    local tdir="$(lkl_test_cmd mktemp -d)"
    local cptofs=("${script_dir}/../cptofs" -p -t "$fstype" -i "$file")
    local cpfromfs=("${tdir}/cpfromfs" -p -t "$fstype" -i "$file")
    lkl_test_cmd ln -s "${script_dir}/../cptofs" "${tdir}/cpfromfs"

    lkl_test_cmd mkdir -p "${tdir}/round-trip"
    lkl_test_cmd mkdir -p "${tdir}/tree/full/of/stuff"
    lkl_test_cmd echo "data" > "${tdir}/tree/data"
    lkl_test_cmd echo "more" > "${tdir}/tree/full/more"
    lkl_test_cmd echo "here" > "${tdir}/tree/full/of/here"
    lkl_test_cmd "${cptofs[@]}" "${tdir}/tree" /
    lkl_test_cmd "${cpfromfs[@]}" /tree "${tdir}/round-trip"
    lkl_test_cmd diff -r "${tdir}/tree" "${tdir}/round-trip/tree"
    lkl_test_cmd rm -rf "$tdir"
}

if [ "$1" = "-t" ]; then
    shift
    fstype=$1
    shift
    [ -z "$fstype" ] && _usage
    [ -n "$1" ] && _usage
else
    _usage
fi

if [ "$LKL_HOST_CONFIG_ARGP" != "y" ]; then
    lkl_test_plan 0 "cptofs $fstype"
    echo "cptofs not available"
    exit 0
fi

if [ -z $(which mkfs.$fstype) ]; then
    lkl_test_plan 0 "cptofs $fstype"
    echo "no mkfs.$fstype command"
    exit 0
fi

if [ -z $(which diff) ]; then
    lkl_test_plan 0 "cptofs $fstype"
    echo "no diff command"
    exit 0
fi

lkl_test_plan 4 "disk $fstype"
lkl_test_run 1 prepfsimg $fstype
lkl_test_run 2 cptofs_file
lkl_test_run 3 cptofs_tree
lkl_test_run 4 cleanfsimg

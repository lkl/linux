#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
set -e

PROTO_NAME=binder

SCRIPT=$(readlink -f "$0")
DIRNAME=$(dirname "$SCRIPT")

if [ -z "{$PROTOBUF_MUTATOR_DIR}" ]; then
  echo "No PROTOBUF_MUTATOR_DIR provided for Binder fuzzer. \
    Refer to documentation at tools/lkl/fuzzers/binder/README.md"
fi

PROTOC="${PROTOBUF_MUTATOR_DIR}/build/external.protobuf/bin/protoc"

rm -f $DIRNAME/$PROTO_NAME.pb.cc $DIRNAME/$PROTO_NAME.pb.h
$PROTOC --cpp_out=$DIRNAME --proto_path=$DIRNAME $PROTO_NAME.proto

# tools/build/Makefile.build expects %.cpp
mv $DIRNAME/$PROTO_NAME.pb.cc $DIRNAME/$PROTO_NAME.pb.cpp

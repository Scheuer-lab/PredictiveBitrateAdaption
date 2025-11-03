#!/bin/bash
cd ~/openwrt-build/

export STAGING_DIR=~/openwrt-build/openwrt-sdk-24.10.3-ath79-generic_gcc-13.3.0_musl.Linux-x86_64/staging_dir
export TOOLCHAIN=$STAGING_DIR/toolchain-mips_24kc_gcc-13.3.0_musl
export TARGET=$STAGING_DIR/target-mips_24kc_musl
export CC=$TOOLCHAIN/bin/mips-openwrt-linux-gcc

# Check if header exists
if [ ! -f "$TARGET/usr/include/libnetfilter_queue/libnetfilter_queue.h" ]; then
    echo "ERROR: Header file not found at $TARGET/usr/include/libnetfilter_queue/"
    echo "Searching for header location..."
    find $STAGING_DIR -name "libnetfilter_queue.h"
    exit 1
fi

echo "Compiling nfq_dummy..."
$CC \
    -I$TARGET/usr/include \
    -I$TARGET/include \
    -o nfq_dummy nfq_dummy.c \
    -L$TARGET/usr/lib \
    -lnetfilter_queue \
    -lnfnetlink \
    -lmnl

if [ $? -eq 0 ]; then
    echo "Success! Binary created: nfq_dummy"
    file nfq_dummy
else
    echo "Compilation failed!"
    exit 1
fi
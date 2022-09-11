#!/bin/sh
./configure \
--log=yes \
--target=x86_64-linux-gcc \
--disable-optimizations \
--enable-debug \
--disable-install-docs \
--disable-libs \
--disable-tools \
--disable-docs \
--disable-unit-tests \
--enable-vp9 \
--disable-vp8 \
--enable-internal-stats \
--enable-libyuv \
--disable-vp9-encoder
#!/bin/sh
./configure \
--log=yes \
--target=x86_64-linux-gcc \
--enable-debug \
--disable-install-docs \
--disable-tools \
--disable-docs \
--disable-unit-tests \
--enable-vp9 \
--disable-vp8 \
--enable-libyuv
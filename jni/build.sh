#!/bin/bash

NDK=/home/nosferatu/Work/adt-bundle-linux/android-ndk-r9d
SYSROOT=$NDK/platforms/android-18/arch-arm/
TOOLCHAIN=$NDK/toolchains/arm-linux-androideabi-4.8/prebuilt/linux-x86_64
PATH=$NDK:$PATH

# Compile .so from .c
ndk-build NDK_DEBUG=1


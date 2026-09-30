# Builds the Linux module on Ubuntu 20.04 Focal (glibc 2.31, GCC 9), the oldest
# base SDR++ still publishes packages for, alongside Debian Bullseye (also glibc
# 2.31): a module linked here loads on both and on every newer distribution,
# where one linked on a newer system would not. Built for amd64 and, under
# emulation, arm64.
#
# mesa-common-dev is for GL/gl.h, which SDR++'s gui headers include.
# libfftw3-dev is for linking (the module records libfftw3f.so.3, which every
# SDR++ package depends on) and for tools/elf_imports.py's check. The VOLK and
# FFTW headers compiled against are the pinned ones build.sh mounts at /hdr:
# SDR++'s headers need a newer VOLK than Focal has.
FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ninja-build curl ca-certificates bzip2 xz-utils patch python3 binutils libfftw3-dev mesa-common-dev \
    && rm -rf /var/lib/apt/lists/*

# Focal's CMake (3.16) predates what the build uses; Kitware's portable one.
ARG CMAKE_VERSION=3.30.5
RUN arch=$(uname -m) \
    && curl -fsSL https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-${arch}.tar.gz \
       | tar xz -C /opt \
    && ln -s /opt/cmake-${CMAKE_VERSION}-linux-${arch}/bin/* /usr/local/bin/

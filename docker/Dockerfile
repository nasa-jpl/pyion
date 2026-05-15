# ======================================================================
# Dockerfile for building and testing pyion against a real ION-DTN node.
#
# The image bundles ION-DTN and the build toolchain. pyion itself is NOT
# compiled into the image: the test compose files (tests/*/test_network.yaml)
# bind-mount this repository and run `python3 setup.py install` at container
# start, so pyion can be rebuilt without rebuilding the image.
#
# Build input
# -----------
# This Dockerfile needs the ION-DTN source tree in its build context. Place
# it next to this file as a directory named  ion-open-source-<ION_VERSION>
# (default ION_VERSION = 4.1.4a2). See docker/README.md.
#
# Build
# -----
#   docker build -t pyion_bpv7:4.1.4a2 -f docker/Dockerfile docker/
#
# A different ION version:
#   docker build --build-arg ION_VERSION=<ver> -t pyion_bpv7:<ver> \
#                -f docker/Dockerfile docker/
# ======================================================================

FROM ubuntu:22.04

ARG ION_VERSION=4.1.4a2
ENV DEBIAN_FRONTEND=noninteractive

# --- Build toolchain + Python development headers --------------------------
# build-essential provides gcc/make and the AddressSanitizer runtime used by
# the C-layer stress tests. valgrind/gdb are included for the same reason.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        autoconf \
        automake \
        autotools-dev \
        libtool \
        python3 \
        python3-dev \
        python3-setuptools \
        valgrind \
        gdb \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# --- ION-DTN ---------------------------------------------------------------
# The ION source tree must be present in the build context (see README).
COPY ion-open-source-${ION_VERSION} /home/ion-open-source-${ION_VERSION}

# Build and install ION's public API to /usr/local. If the tree lacks a
# generated ./configure (e.g. a git checkout), regenerate it first.
RUN cd /home/ion-open-source-${ION_VERSION} \
    && ( [ -x ./configure ] || autoreconf -fi ) \
    && ./configure \
    && make \
    && make install \
    && ldconfig

# --- pyion build configuration ---------------------------------------------
# ION_HOME must point at the ION SOURCE tree: setup.py reads ION's private
# headers (ici/include, bpv7/library, ...) directly from it.
ENV ION_HOME=/home/ion-open-source-${ION_VERSION}
ENV PYION_BP_VERSION=BPv7
ENV LD_LIBRARY_PATH=/usr/local/lib

# The test compose files bind-mount the pyion repo here.
WORKDIR /home/ion-interface

# The compose files override this with their own command; keep the container
# alive when run directly.
CMD ["tail", "-f", "/dev/null"]

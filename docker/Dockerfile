# ======================================================================
# Dockerfile for building and testing pyion against a real ION-DTN node.
#
# The image is self-contained: ION-DTN is downloaded and built during the
# image build, so no local ION source tree is required.
#
# pyion itself is NOT compiled into the image. The test compose files
# (tests/*/test_network.yaml) bind-mount this repository and run
# `python3 setup.py install` at container start, so pyion can be rebuilt
# without rebuilding the image.
#
# Build
# -----
#   docker build -t pyion_bpv7:4.1.4a2 -f docker/Dockerfile docker/
#
# A different ION version (ION_TAG is the git tag in the ION-DTN repo;
# ION_VERSION names the install directory and must match the ION_HOME in
# the test compose files):
#   docker build \
#       --build-arg ION_TAG=ion-open-source-<tag> \
#       --build-arg ION_VERSION=<ver> \
#       -t pyion_bpv7:<ver> -f docker/Dockerfile docker/
# ======================================================================

FROM ubuntu:22.04

ARG ION_VERSION=4.1.4a2
ARG ION_TAG=ion-open-source-4.1.4-a.2
ARG ION_REPO=https://github.com/nasa-jpl/ION-DTN
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
        wget \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# --- ION-DTN ---------------------------------------------------------------
# Download the ION-DTN source for the requested tag and build/install its
# public API into /usr/local. The GitHub archive has no generated ./configure,
# so it is regenerated with autoreconf.
RUN wget -q -O /tmp/ion.tar.gz "${ION_REPO}/archive/refs/tags/${ION_TAG}.tar.gz" \
    && mkdir -p /home/ion-open-source-${ION_VERSION} \
    && tar -xzf /tmp/ion.tar.gz -C /home/ion-open-source-${ION_VERSION} --strip-components=1 \
    && rm /tmp/ion.tar.gz \
    && cd /home/ion-open-source-${ION_VERSION} \
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

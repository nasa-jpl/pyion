#!/usr/bin/env bash
# Build pyion wheels for CPython 3.9-3.13 inside the pinned manylinux_2_28 image.
#
# Usage: scripts/build_wheels.sh [ion-tag] [output-dir]
#   [ion-tag]     optional; must equal ION_TAG in ion-pin.env (the default)
#   [output-dir]  default: ./dist
#
# Environment:
#   CONTAINER_ENGINE   podman (default) or docker
#   MANYLINUX_IMAGE    image reference with @sha256 digest (pinned default below)
#   ION_REPO           ION-DTN repository URL
#
# Output: repaired wheels plus SHA256SUMS in the output directory.

set -euo pipefail

# Pinned build image (quay.io/pypa/manylinux_2_28_x86_64). Keep in sync with docker/Dockerfile.
DEFAULT_MANYLINUX_IMAGE="quay.io/pypa/manylinux_2_28_x86_64@sha256:9555ba6c479fa69a2aa80ad1dc57154a60d562b19df159361d9379a02beb1e36"

# ION_TAG, ION_SHA256 come from the pin file; see docs/release.md for moving versions.
. "$(dirname "$0")/../ion-pin.env"
REQUESTED_TAG="${1:-$ION_TAG}"
OUT_DIR="${2:-dist}"
CONTAINER_ENGINE="${CONTAINER_ENGINE:-podman}"
MANYLINUX_IMAGE="${MANYLINUX_IMAGE:-$DEFAULT_MANYLINUX_IMAGE}"
ION_REPO="${ION_REPO:-https://github.com/nasa-jpl/ION-DTN}"

if [[ "$REQUESTED_TAG" != "$ION_TAG" ]]; then
    echo "error: ION tag '$REQUESTED_TAG' does not match ion-pin.env ($ION_TAG)" >&2
    exit 2
fi
if [[ "$MANYLINUX_IMAGE" != *"@sha256:"* ]]; then
    echo "error: MANYLINUX_IMAGE must be pinned by digest (contain '@sha256:'): $MANYLINUX_IMAGE" >&2
    exit 2
fi

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

if ! "$CONTAINER_ENGINE" pull "$MANYLINUX_IMAGE"; then
    echo "error: could not pull build image: $MANYLINUX_IMAGE" >&2
    exit 1
fi

# Everything below runs inside the container. The source tree is mounted read-only
# and copied, so setup.py's generated files never touch the checkout.
"$CONTAINER_ENGINE" run --rm \
    -e ION_TAG="$ION_TAG" \
    -e ION_REPO="$ION_REPO" \
    -e ION_SHA256="$ION_SHA256" \
    -v "$REPO_DIR":/src:ro,Z \
    -v "$OUT_DIR":/out:Z \
    "$MANYLINUX_IMAGE" \
    bash -c '
set -euo pipefail

yum install -y autoconf automake libtool wget unzip >/dev/null

# --- Build ION from the pinned tarball into a private prefix ------------------
work=/tmp/work
rm -rf "$work" && mkdir -p "$work"
wget -q -O "$work/ion.tar.gz" "$ION_REPO/archive/refs/tags/$ION_TAG.tar.gz"
echo "$ION_SHA256  $work/ion.tar.gz" | sha256sum -c - >/dev/null || {
    echo "error: ION tarball checksum mismatch for $ION_TAG (expected $ION_SHA256)" >&2
    exit 1
}
mkdir -p "$work/ion" && tar -xzf "$work/ion.tar.gz" -C "$work/ion" --strip-components=1
cd "$work/ion"
( [ -x ./configure ] || autoreconf -fi )
./configure --prefix="$work/ion-prefix" >/dev/null
make -j"$(nproc)" >/dev/null
make install >/dev/null
ION_SRC="$work/ion"
ION_PREFIX="$work/ion-prefix"

# --- Copy pyion source so generated files stay out of the mount -------------
cp -a /src "$work/pyion"
cd "$work/pyion"

export ION_HOME="$ION_SRC"
export LD_LIBRARY_PATH="$ION_PREFIX/lib"
export PYION_BP_VERSION=BPv7

RAW=/tmp/raw
rm -rf "$RAW" && mkdir -p "$RAW"

EXCLUDES=(--exclude "libici.so.*" --exclude "libbp.so.*" --exclude "libltp.so.*"
          --exclude "libcfdp.so.*" --exclude "libdgr.so.*" --exclude "libams.so.*")

for py in cp39-cp39 cp310-cp310 cp311-cp311 cp312-cp312 cp313-cp313; do
    PY=/opt/python/$py/bin/python
    "$PY" -m pip install -q --upgrade pip build auditwheel
    "$PY" -m build --wheel --outdir "$RAW" .
done

# --- Repair (bundle nothing from ION; ION is a runtime prerequisite) --------
for whl in "$RAW"/*.whl; do
    "/opt/python/cp312-cp312/bin/auditwheel" repair --plat manylinux_2_28_x86_64 \
        "${EXCLUDES[@]}" -w /out "$whl"
done

# --- Checks on every produced wheel -----------------------------------------
for whl in /out/*.whl; do
    tmp=$(mktemp -d)
    (cd "$tmp" && unzip -q "$whl")
    if unzip -l "$whl" | grep -E "/lib(ici|bp|ltp|cfdp|dgr|ams)[^/]*\.so" >/dev/null; then
        echo "error: ION library found inside $whl" >&2
        exit 1
    fi
    for so in $(find "$tmp" -name "*.so"); do
        if readelf -d "$so" | grep -E "(RPATH|RUNPATH)" >/dev/null; then
            echo "error: RPATH/RUNPATH present in $so ($whl)" >&2
            exit 1
        fi
    done
    rm -rf "$tmp"
done

cd /out
sha256sum ./*.whl > SHA256SUMS
ls -l /out
'

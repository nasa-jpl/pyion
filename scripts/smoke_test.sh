#!/usr/bin/env bash
# Smoke test one pyion wheel against a prebuilt ION runtime.
#
# Usage: scripts/smoke_test.sh <wheel> <ion-tag>
#
# Builds ION from the same tag into a private prefix inside the manylinux image,
# installs the wheel into a fresh venv with pip, and checks:
#   1. every extension module imports with PYION_BP_VERSION and ION_HOME unset
#   2. ION_VERSION and BP_VERSION are reported
#   3. importing without the ION libraries on the linker path fails with the named
#      ImportError
#   4. a two-node BPv7 loopback over TCP delivers bundles
#   5. a wheel whose ION version differs from the installed ION fails with the
#      named version-mismatch ImportError

set -euo pipefail

WHEEL="${1:-}"
REQUESTED_TAG="${2:-}"
CONTAINER_ENGINE="${CONTAINER_ENGINE:-docker}"
MANYLINUX_IMAGE="${MANYLINUX_IMAGE:-quay.io/pypa/manylinux_2_28_x86_64@sha256:9555ba6c479fa69a2aa80ad1dc57154a60d562b19df159361d9379a02beb1e36}"
ION_REPO="${ION_REPO:-https://github.com/nasa-jpl/ION-DTN}"

if [[ -z "$WHEEL" || -z "$REQUESTED_TAG" || ! -f "$WHEEL" ]]; then
    echo "usage: $0 <wheel-file> <ion-tag>" >&2
    exit 2
fi

. "$(dirname "$0")/../ion-pin.env"
if [[ "$REQUESTED_TAG" != "$ION_TAG" ]]; then
    echo "error: ION tag '$REQUESTED_TAG' does not match ion-pin.env ($ION_TAG)" >&2
    exit 2
fi

WHEEL_ABS="$(cd "$(dirname "$WHEEL")" && pwd)/$(basename "$WHEEL")"
WHEEL_DIR="$(dirname "$WHEEL_ABS")"
WHEEL_NAME="$(basename "$WHEEL_ABS")"
# CPython tag from the wheel name (e.g. cp310); the venv must use that interpreter.
PYTAG="$(cut -d- -f3 <<<"$WHEEL_NAME")"

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"

"$CONTAINER_ENGINE" run --rm \
    -e ION_TAG="$ION_TAG" \
    -e ION_REPO="$ION_REPO" \
    -e ION_SHA256="$ION_SHA256" \
    -e ION_BUILD_VERSION="$ION_BUILD_VERSION" \
    -e WHEEL_NAME="$WHEEL_NAME" \
    -e PYTAG="$PYTAG" \
    -v "$WHEEL_DIR":/wheels:ro,Z \
    -v "$REPO_DIR/tests":/src-tests:ro,Z \
    "$MANYLINUX_IMAGE" \
    bash -c '
set -euo pipefail
yum install -y autoconf automake libtool wget >/dev/null

work=/tmp/smoke
rm -rf "$work" && mkdir -p "$work"
wget -q -O "$work/ion.tar.gz" "$ION_REPO/archive/refs/tags/$ION_TAG.tar.gz"
echo "$ION_SHA256  $work/ion.tar.gz" | sha256sum -c - >/dev/null || {
    echo "error: ION tarball checksum mismatch for $ION_TAG (expected $ION_SHA256)" >&2
    exit 1
}
mkdir -p "$work/ion" && tar -xzf "$work/ion.tar.gz" -C "$work/ion" --strip-components=1
cd "$work/ion"
( [ -x ./configure ] || autoreconf -fi )
./configure --prefix="$work/prefix" >/dev/null
make -j"$(nproc)" >/dev/null
make install >/dev/null
export LD_LIBRARY_PATH="$work/prefix/lib"

PY=/opt/python/${PYTAG}-${PYTAG}/bin/python
"$PY" -m venv "$work/venv"
"$work/venv/bin/pip" install -q "/wheels/$WHEEL_NAME"

cd /tmp
env -u PYION_BP_VERSION -u ION_HOME "$work/venv/bin/python" -c "
import pyion, _bp, _cfdp, _ltp, _mem, _mgmt
print(\"ION_VERSION=\", pyion.ION_VERSION, \"BP_VERSION=\", pyion.BP_VERSION)
assert pyion.ION_VERSION == \"$ION_BUILD_VERSION\"
assert pyion.BP_VERSION == \"BPv7\"
"

# Negative: without the ION libraries on the linker path the wheel must refuse to import.
if env -u LD_LIBRARY_PATH "$work/venv/bin/python" -c "import pyion" 2>"$work/neg.err"; then
    echo "error: import succeeded without ION libraries on the linker path" >&2
    exit 1
fi
grep -q "cannot load ION library" "$work/neg.err" || { cat "$work/neg.err" >&2; exit 1; }

# Two-node BPv7 loopback over TCP with the installed wheel on the send path.
# The loopback writes node state into its own directory, so run a copy.
cp -a /src-tests/loopback /tmp/loopback
PATH="$work/prefix/bin:$PATH" PYTHON="$work/venv/bin/python" bash /tmp/loopback/run_loopback.sh

# Negative: an installed ION whose ion.pc version differs from the build version of the wheel
# must fail the import with the named mismatch error.
sed -i "s/^Version:.*/Version: 0.0.0-mismatch/" "$work/prefix/lib/pkgconfig/ion.pc"
if "$work/venv/bin/python" -c "import pyion" 2>"$work/mismatch.err"; then
    echo "error: import succeeded despite ION version mismatch" >&2
    exit 1
fi
grep -q "ION version mismatch" "$work/mismatch.err" || { cat "$work/mismatch.err" >&2; exit 1; }
echo "smoke test passed for $WHEEL_NAME"
'

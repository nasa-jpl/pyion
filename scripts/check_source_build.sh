#!/usr/bin/env bash
# Source builds succeed with the env vars set and link the prefix ION libraries.
#
# Usage: ION_HOME=<ion-src> LD_LIBRARY_PATH=<ion-prefix>/lib PYION_BP_VERSION=BPv7 \
#        scripts/check_source_build.sh
set -euo pipefail

: "${ION_HOME:?set ION_HOME to the ION source tree}"
: "${LD_LIBRARY_PATH:?set LD_LIBRARY_PATH to the ION prefix lib directory}"
: "${PYION_BP_VERSION:=BPv7}"
export ION_HOME LD_LIBRARY_PATH PYION_BP_VERSION

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_DIR"
PY="${PYTHON:-python3}"

rm -rf build dist
"$PY" -m build --wheel --outdir dist . >/dev/null
"$PY" setup.py build_ext --inplace >/dev/null

# The built extension must link the ION libraries from the prefix, not /usr/local.
so=$(ls _bp*.so | head -1)
if ldd "$so" | grep -q "not found"; then
    echo "error: $so has unresolved ION libraries" >&2
    ldd "$so" | grep "not found" >&2
    exit 1
fi
if ! ldd "$so" | grep -q "libbp.so.* => /"; then
    echo "error: $so does not link libbp.so from a resolved path" >&2
    exit 1
fi
echo "source build check passed (linked: $(ldd "$so" | grep -E 'libbp|libici' | awk '{print $3}' | tr '\n' ' '))"

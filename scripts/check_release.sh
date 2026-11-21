#!/usr/bin/env bash
# Verify that a GitHub Release has every expected wheel and SHA256SUMS attached.
#
# Usage: scripts/check_release.sh <tag> [assets-json-file]
#   With assets-json-file, reads `gh release view --json assets` output from that file
#   instead of calling GitHub (used by tests with fixture JSON).
#
# Expected: wheels for CPython 3.9-3.13 (manylinux_2_28_x86_64) and SHA256SUMS.

set -euo pipefail

TAG="${1:-}"
FIXTURE="${2:-}"
if [[ -z "$TAG" ]]; then
    echo "usage: $0 <tag> [assets-json-file]" >&2
    exit 2
fi

if [[ -n "$FIXTURE" ]]; then
    json="$(cat "$FIXTURE")"
else
    json="$(gh release view "$TAG" --json assets)"
fi

names="$(printf '%s' "$json" | python3 -c 'import json,sys; print("\n".join(a["name"] for a in json.load(sys.stdin)["assets"]))')"

missing=0
for py in cp39-cp39 cp310-cp310 cp311-cp311 cp312-cp312 cp313-cp313; do
    if ! grep -q "^pyion-.*-${py}-.*manylinux_2_28_x86_64.*\.whl$" <<<"$names"; then
        echo "missing wheel for $py" >&2
        missing=1
    fi
done
if ! grep -qx "SHA256SUMS" <<<"$names"; then
    echo "missing SHA256SUMS" >&2
    missing=1
fi

if [[ $missing -ne 0 ]]; then
    echo "release $TAG is incomplete" >&2
    exit 1
fi
echo "release $TAG has all expected assets"

#!/usr/bin/env bash
# Run the regression steps from .github/workflows/regression.yml locally.
#
# Builds docker/Dockerfile with CONTAINER_ENGINE (default podman) and runs the stock
# C-layer concurrency suite. The sanitizer and free-threaded jobs are not reproduced here.
#
# Usage: scripts/run_regression.sh

set -euo pipefail

CONTAINER_ENGINE="${CONTAINER_ENGINE:-podman}"
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE=pyion_bpv7:4.2.1a1

"$CONTAINER_ENGINE" build -t "$IMAGE" -f "$REPO_DIR/docker/Dockerfile" "$REPO_DIR/docker"

"$CONTAINER_ENGINE" run --rm --shm-size=256m \
    -e PYTHONUNBUFFERED=1 \
    -v "$REPO_DIR":/home/ion-interface \
    "$IMAGE" bash -c '
        set -e
        cd /home/ion-interface
        rm -rf build
        python3 setup.py install >/tmp/build.log 2>&1 || { tail -30 /tmp/build.log; exit 1; }
        bash tests/regression/concurrency/run_c_concurrency_test.sh
    '
echo "regression passed"

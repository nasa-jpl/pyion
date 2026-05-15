#!/bin/bash
# ======================================================================
# Start a single ION node, run the C-layer concurrency regression test,
# then stop ION and clean up.
#
# Every step is wrapped in `timeout`: ION-in-container startup can
# occasionally hang, and a wedged test must surface as a bounded failure
# rather than hanging the run indefinitely.
#
# Intended to run inside the docker/ test image, where ION's admin tools
# are on PATH. pyion's _bp extension must be importable: either install
# pyion first, or set PYTHONPATH to a build_ext output directory.
#
# Usage
# -----
#   bash tests/regression/concurrency/run_c_concurrency_test.sh
#
# To run under a sanitizer, build pyion with PYION_SANITIZE and point
# PYION_ASAN_PRELOAD at the sanitizer runtime:
#   PYION_SANITIZE=address,undefined python3 setup.py build_ext
#   PYION_ASAN_PRELOAD=$(gcc -print-file-name=libasan.so) \
#       bash tests/regression/concurrency/run_c_concurrency_test.sh
# ======================================================================
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
NODE_DIR="$HERE/ion_node"

ION_START_TIMEOUT=45     # seconds for each ION admin command
TEST_TIMEOUT=180         # seconds for the test itself

cleanup_ion() {
    cd "$NODE_DIR" || return
    timeout 30 bpadmin  . >/dev/null 2>&1
    sleep 1
    timeout 30 ionadmin . >/dev/null 2>&1
    killm                 >/dev/null 2>&1
}

# Clear any stale ION shared memory from a previous run.
killm >/dev/null 2>&1

# --- Start ION -------------------------------------------------------------
cd "$NODE_DIR" || exit 1
echo "Starting ION node..."
for cmd in "ionadmin host.ionrc" "ionsecadmin host.ionsecrc" "bpadmin host.bprc"; do
    if ! timeout "$ION_START_TIMEOUT" $cmd; then
        echo "ION startup step failed or timed out: $cmd"
        cleanup_ion
        exit 1
    fi
done
sleep 2

# --- Run the test ----------------------------------------------------------
# When PYION_ASAN_PRELOAD is set, the test runs under the sanitizer runtime
# (pyion must have been built with PYION_SANITIZE). Preload is scoped to the
# Python process only: ION's admin tools above are not sanitizer-instrumented
# and must not inherit it.
cd "$HERE"
if [ -n "${PYION_ASAN_PRELOAD:-}" ]; then
    echo "Running test under sanitizer ($PYION_ASAN_PRELOAD)..."
    LD_PRELOAD="$PYION_ASAN_PRELOAD" \
    ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" \
    UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1}" \
        timeout "$TEST_TIMEOUT" python3 test_c_concurrency.py
else
    timeout "$TEST_TIMEOUT" python3 test_c_concurrency.py
fi
RC=$?
if [ "$RC" -eq 124 ]; then
    echo "OVERALL STATUS: FAILED (test timed out after ${TEST_TIMEOUT}s -- likely deadlock)"
fi

# --- Stop ION --------------------------------------------------------------
echo "Stopping ION node..."
cleanup_ion

exit $RC

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
cd "$HERE"
timeout "$TEST_TIMEOUT" python3 test_c_concurrency.py
RC=$?
if [ "$RC" -eq 124 ]; then
    echo "OVERALL STATUS: FAILED (test timed out after ${TEST_TIMEOUT}s -- likely deadlock)"
fi

# --- Stop ION --------------------------------------------------------------
echo "Stopping ION node..."
cleanup_ion

exit $RC

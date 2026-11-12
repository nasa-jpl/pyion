#!/usr/bin/env bash
# Two-node BPv7 delivery check over loopback TCP, modeled on the bench-tcp demo.
#
# Starts ION node 2 (sender) and node 3 (receiver) on 127.0.0.1, sends a fixed number
# of bundles from ipn:2.x to ipn:3.1 over TCP, and waits until node 3 reports each
# payload delivered. Stops both nodes on exit.
#
# Usage: tests/loopback/run_loopback.sh
# Requires ION admin tools (ionadmin, bpadmin, bpsink, killm) on PATH and pyion importable
# by $PYTHON (default python3).
# killm stops every ION daemon of the current user, not only this test's, so run this
# only where no other ION node is running (a container, or a host dedicated to tests).
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
BUNDLES=${BUNDLES:-3}
WAIT_SECS=${WAIT_SECS:-60}
SEND_FILE="$HERE/2/payload.txt"
RECV_FILE="$HERE/3/received.txt"

export ION_NODE_LIST_DIR="$HERE"
PY="${PYTHON:-python3}"

stop_nodes() {
    (cd "$HERE/3" && bpadmin . >/dev/null 2>&1; ionadmin . >/dev/null 2>&1)
    (cd "$HERE/2" && bpadmin . >/dev/null 2>&1; ionadmin . >/dev/null 2>&1)
    killm f >/dev/null 2>&1
    rm -f "$HERE/ion_nodes"
}

trap stop_nodes EXIT
killm f >/dev/null 2>&1
rm -f "$HERE/ion_nodes" "$RECV_FILE"
echo "This is a pyion loopback payload." > "$SEND_FILE"

for n in 2 3; do
    (cd "$HERE/$n" && ./ionstart) || { echo "ION start failed for $n" >&2; exit 1; }
done
sleep 3

# Receiver on node 3 (background; output goes to RECV_FILE).
(cd "$HERE/3" && bpsink ipn:3.1 > "$RECV_FILE") &
sleep 1

# Sender on node 2.
(cd "$HERE/2" && "$PY" "$HERE/pyion_send.py" "$BUNDLES") || { echo "pyion send failed" >&2; exit 1; }

elapsed=0
got=0
while [ "$elapsed" -lt "$WAIT_SECS" ]; do
    got=$(grep -c "^ION event: Payload delivered\." "$RECV_FILE" 2>/dev/null || true)
    [ -z "$got" ] && got=0
    [ "$got" -ge "$BUNDLES" ] && break
    sleep 2
    elapsed=$((elapsed + 2))
done

if [ "$got" -ge "$BUNDLES" ]; then
    echo "loopback ok: $got of $BUNDLES BPv7 bundles delivered ipn:2 -> ipn:3 over TCP"
    exit 0
fi
echo "loopback FAILED: only $got of $BUNDLES bundles delivered after ${WAIT_SECS}s" >&2
exit 1

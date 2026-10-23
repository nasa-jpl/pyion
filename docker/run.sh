#!/usr/bin/env bash
#
# Convenience runner for the portable pyion regression network.
#
# Usage:
#   docker/run.sh [bp|cfdp|ltp|mem]     # bring up the two-node network (default: bp)
#   docker/run.sh <proto> down          # tear it down
#   docker/run.sh <proto> <compose-args...>
#
# After 'up', drive the test from two more shells (receiver first):
#   docker exec -it pyion_node2 bash -lc "cd /work/${PROTO}_tests && python3 rx.py"
#   docker exec -it pyion_node1 bash -lc "cd /work/${PROTO}_tests && python3 tx.py"
set -euo pipefail

cd "$(dirname "$0")"

PROTO="${1:-bp}"
case "$PROTO" in
  bp|cfdp|ltp|mem) ;;
  *) echo "error: protocol must be one of: bp cfdp ltp mem" >&2; exit 2 ;;
esac
shift || true

export PYION_TEST="$PROTO"

if [ "$#" -eq 0 ]; then
  echo ">> Bringing up pyion '${PROTO}' test network (first run builds ION, ~10-15 min)."
  echo ">> When both nodes report 'up', drive the test in two more shells:"
  echo ">>   docker exec -it pyion_node2 bash -lc 'cd /work/${PROTO}_tests && python3 rx.py'"
  echo ">>   docker exec -it pyion_node1 bash -lc 'cd /work/${PROTO}_tests && python3 tx.py'"
  echo ">> Stop with: docker/run.sh ${PROTO} down"
  exec docker compose up --build
else
  exec docker compose "$@"
fi

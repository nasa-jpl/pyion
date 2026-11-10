"""Send BPv7 bundles from ipn:2.1 to ipn:3.1 through pyion on ION node 2.

Used by tests/loopback/run_loopback.sh in place of ION's bpsource, so that the
pyion extensions are on the send path. Run with the node 2 directory as the
working directory and ION_NODE_LIST_DIR set to the loopback directory.
"""

import sys

from pyion.proxies import BpProxy

SRC_EID = "ipn:2.1"
DST_EID = "ipn:3.1"


def main(count):
    payload = b"This is a pyion loopback payload.\n"
    proxy = BpProxy(2)
    proxy.bp_attach()
    try:
        ept = proxy.bp_open(SRC_EID)
        try:
            for _ in range(count):
                ept.bp_send(DST_EID, payload)
        finally:
            proxy.bp_close(ept)
    finally:
        proxy.bp_detach()
    return 0


if __name__ == "__main__":
    sys.exit(main(int(sys.argv[1]) if len(sys.argv) > 1 else 1))

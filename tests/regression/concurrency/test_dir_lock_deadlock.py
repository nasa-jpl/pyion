"""
Self-contained smoke test for the PART I ``in_ion_folder`` lock-scope fix.

Background
----------
``pyion.utils.in_ion_folder`` used to wrap the *entire* decorated call inside the
process-global ``ion_dir_lock`` -- even when ``node_dir is None`` and no
``os.chdir()`` was performed. Because the blocking receive path
(``Endpoint._bp_receive``) is decorated with ``in_ion_folder``, the worker thread
held ``ion_dir_lock`` while blocked inside the C ``bp_receive`` call. When the
receive timed out, ``Endpoint`` called ``proxy.bp_interrupt()`` -- which is *also*
decorated with ``in_ion_folder`` -- and that call blocked forever trying to
acquire the same lock. The receiver could only be woken by the interrupt, and the
interrupt could only run once the receiver released the lock: a permanent deadlock.

The fix skips ``ion_dir_lock`` entirely when ``node_dir is None`` (the common
single-node case), so the interrupt path is never gated on a lock held by the
blocked receiver.

These tests use mocked ION C extensions (no ION install required) and a watchdog
timer, so a regression manifests as a FAIL/timeout rather than an infinite hang.
"""
import os
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import MagicMock


# --- MOCK THE ION C EXTENSIONS BEFORE IMPORTING pyion ---------------------------
class UniqueMock(MagicMock):
    _counter = 1000

    def __getattr__(self, name):
        if name.startswith('_'):
            return super().__getattr__(name)
        if name[0].isupper():
            val = UniqueMock._counter
            UniqueMock._counter += 1
            setattr(self, name, val)
            return val
        return super().__getattr__(name)


mock_bp = UniqueMock()
sys.modules['_bp'] = mock_bp
sys.modules['_ltp'] = UniqueMock()
sys.modules['_cfdp'] = UniqueMock()
sys.modules['_mgmt'] = UniqueMock()
sys.modules['_mem'] = UniqueMock()
sys.modules['_utils'] = UniqueMock()

import pyion  # noqa: E402
from pyion import utils  # noqa: E402
from pyion.bp import Endpoint  # noqa: E402
from pyion.proxies import BpProxy  # noqa: E402
from pyion.constants import BpCustodyEnum  # noqa: E402


class FakeProxy:
    """Minimal stand-in for a pyion Proxy.

    ``bp_interrupt`` is decorated with the *real* ``in_ion_folder`` so it takes
    exactly the same lock path the deadlock depended on. ``node_dir`` is None,
    matching the common single-node deployment.
    """

    def __init__(self, wake_event):
        self.node_dir = None
        self._wake_event = wake_event

    @utils.in_ion_folder
    def bp_interrupt(self, ept):
        # ION's real bp_interrupt wakes the blocked receiver; emulate that by
        # releasing the event the mocked bp_receive is waiting on.
        self._wake_event.set()

    def bp_close(self, ept):
        # No-op: lets Endpoint.__del__ -> close() run cleanly at teardown.
        self._wake_event.set()
        ept._cleanup()


def _make_endpoint(proxy):
    no_custody = int(BpCustodyEnum.NO_CUSTODY_REQUESTED)
    return Endpoint(proxy, "ipn:1.1", 0x1234, 3600, 1, None,
                    no_custody, 0, 0, 0, False, None, None, False, False)


def run_with_watchdog(fn, timeout, on_timeout=None):
    """Run ``fn`` in a daemon thread; return True if it finished within timeout."""
    done = threading.Event()

    def target():
        try:
            fn()
        finally:
            done.set()

    t = threading.Thread(target=target, daemon=True)
    t.start()
    finished = done.wait(timeout)
    if not finished and on_timeout is not None:
        on_timeout()  # best-effort unblock so the process can exit cleanly
        done.wait(1.0)
    return finished


class TestDirLockDeadlock(unittest.TestCase):

    def setUp(self):
        print(f"\nRunning: {self._testMethodName}")
        # Each test configures the mocked C calls it needs; clear any side
        # effects a previous test installed so order cannot leak between them.
        for name in ("bp_receive", "bp_interrupt", "bp_close", "bp_send",
                     "bp_attach", "bp_detach"):
            getattr(mock_bp, name).side_effect = None

    def _proxy_with_node_dir(self, node_nbr):
        """A real BpProxy (node_dir set) with one registered endpoint.

        Drives the actual bp_close/bp_interrupt/bp_receive code (not a stand-in)
        against a throwaway directory os.chdir() can enter. Cleanup marks the
        endpoint closed and clears node_dir so neither the endpoint's nor the
        proxy's __del__ touches ION or chdir at an arbitrary GC time.

        :return: (proxy, endpoint)
        """
        proxy = BpProxy(node_nbr)          # __init__ does not attach
        proxy.attached = True
        proxy.node_dir = Path(tempfile.mkdtemp(prefix="pyion_node_"))
        ept = _make_endpoint(proxy)
        proxy._ept_map[ept.eid] = ept
        self.addCleanup(ept._cleanup)                      # __del__ -> no-op
        self.addCleanup(setattr, proxy, "node_dir", None)  # __del__ -> no chdir
        self.addCleanup(proxy._ept_map.clear)
        return proxy, ept

    def test_receive_timeout_interrupt_does_not_deadlock(self):
        """A timed-out bp_receive must be able to interrupt itself (no deadlock)."""
        wake = threading.Event()

        # Mocked C receive: block until interrupted (as ION's bp_receive does),
        # then return empty to signal an interrupted receive.
        def blocking_receive(sap_addr, return_headers):
            wake.wait(5.0)
            return b""

        mock_bp.bp_receive.side_effect = blocking_receive

        proxy = FakeProxy(wake)
        ept = _make_endpoint(proxy)

        # bp_receive(timeout=0.2): the worker blocks in blocking_receive; on
        # timeout Endpoint calls proxy.bp_interrupt(), which (pre-fix) would
        # deadlock on ion_dir_lock. Watchdog gives it 3s to complete.
        finished = run_with_watchdog(
            lambda: ept.bp_receive(timeout=0.2),
            timeout=3.0,
            on_timeout=wake.set,   # unblock the leaked worker if we deadlocked
        )

        if finished:
            print("  [PASS] receive timeout + interrupt completed without deadlock.")
        else:
            print("  [FAIL] receive timeout deadlocked on ion_dir_lock.")
        self.assertTrue(
            finished,
            "bp_receive timeout path deadlocked: bp_interrupt could not acquire "
            "ion_dir_lock held by the blocked receiver.",
        )

    def test_single_node_calls_are_not_globally_serialized(self):
        """With node_dir=None, a blocked receive must not stall other endpoints."""
        wake = threading.Event()
        send_completed = threading.Event()

        def blocking_receive(sap_addr, return_headers):
            wake.wait(5.0)
            return b""

        def quick_send(*args, **kwargs):
            send_completed.set()

        mock_bp.bp_receive.side_effect = blocking_receive
        mock_bp.bp_send.side_effect = quick_send

        proxy = FakeProxy(wake)
        ept_rx = _make_endpoint(proxy)
        ept_tx = _make_endpoint(proxy)

        # Endpoint A parks in a long blocking receive (no timeout interrupt).
        rx_thread = threading.Thread(
            target=lambda: ept_rx.bp_receive(timeout=4.0), daemon=True)
        rx_thread.start()

        # Give A time to enter the blocking receive and (pre-fix) grab the lock.
        import time
        time.sleep(0.2)

        # Endpoint B sends. Under the old global-lock-across-func behavior this
        # would block behind A's receive; under the fix it returns immediately.
        run_with_watchdog(lambda: ept_tx.bp_send("ipn:2.1", b"hi"), timeout=2.0)

        proceeded = send_completed.wait(0.5)

        # Cleanly release the parked receiver.
        wake.set()
        rx_thread.join(2.0)

        if proceeded:
            print("  [PASS] send on endpoint B proceeded while endpoint A blocked in receive.")
        else:
            print("  [FAIL] send on endpoint B was serialized behind endpoint A's blocking receive.")
        self.assertTrue(
            proceeded,
            "send was blocked by an unrelated endpoint's receive: ion_dir_lock "
            "is still held across the blocking call for node_dir=None.",
        )

    # ----------------------------------------------------------------------
    # node_dir SET -- the case the original fix and tests never covered.
    # Here in_ion_folder takes ion_dir_lock, so the deadlocks are live unless
    # close/interrupt/receive stay off that lock. (issue #22)
    # ----------------------------------------------------------------------

    def test_close_with_node_dir_does_not_deadlock(self):
        """bp_close with node_dir set must return (close -> interrupt recursion)."""
        proxy, ept = self._proxy_with_node_dir(91)

        # Pre-fix: bp_close holds the non-reentrant ion_dir_lock and calls
        # bp_interrupt, which re-acquires it on the same thread -> self-deadlock.
        finished = run_with_watchdog(lambda: proxy.bp_close(ept), timeout=3.0)

        if finished:
            print("  [PASS] bp_close with node_dir set returned (no self-deadlock).")
        else:
            print("  [FAIL] bp_close with node_dir set deadlocked on ion_dir_lock.")
        self.assertTrue(
            finished,
            "bp_close deadlocked: it held ion_dir_lock and bp_interrupt re-took "
            "it on the same thread.",
        )

    def test_blocked_receive_external_interrupt_with_node_dir(self):
        """An external bp_interrupt must wake a receive blocked with node_dir set."""
        wake = threading.Event()
        mock_bp.bp_receive.side_effect = lambda sap, hdr: (wake.wait(5.0), b"")[1]
        mock_bp.bp_interrupt.side_effect = lambda sap: wake.set()  # ION wakes receiver

        proxy, ept = self._proxy_with_node_dir(92)

        rx = threading.Thread(target=lambda: ept.bp_receive(timeout=4.0), daemon=True)
        rx.start()
        time.sleep(0.2)  # let the receiver enter the blocking call (holds lock pre-fix)

        # Pre-fix: the receiver holds ion_dir_lock across the blocking receive, so
        # this interrupt can never acquire it.
        finished = run_with_watchdog(
            lambda: proxy.bp_interrupt(ept), timeout=3.0, on_timeout=wake.set)
        rx.join(2.0)

        ok = finished and not rx.is_alive()
        print("  [%s] external interrupt of a blocked receive (node_dir set)."
              % ("PASS" if ok else "FAIL"))
        self.assertTrue(
            ok, "bp_interrupt could not wake a receive that held ion_dir_lock.")

    def test_receive_timeout_interrupt_with_node_dir(self):
        """A timed-out receive must interrupt itself with node_dir set."""
        wake = threading.Event()
        mock_bp.bp_receive.side_effect = lambda sap, hdr: (wake.wait(5.0), b"")[1]
        mock_bp.bp_interrupt.side_effect = lambda sap: wake.set()

        proxy, ept = self._proxy_with_node_dir(93)

        # bp_receive's own timeout path calls proxy.bp_interrupt; pre-fix that
        # blocks on the lock held by the blocked receiver.
        finished = run_with_watchdog(
            lambda: ept.bp_receive(timeout=0.2), timeout=3.0, on_timeout=wake.set)

        print("  [%s] receive timeout self-interrupt (node_dir set)."
              % ("PASS" if finished else "FAIL"))
        self.assertTrue(
            finished,
            "receive timeout deadlocked: bp_interrupt could not acquire "
            "ion_dir_lock held by the blocked receiver.",
        )

    def test_blocked_receive_close_with_node_dir(self):
        """bp_close must wake and close a receive blocked with node_dir set."""
        wake = threading.Event()
        mock_bp.bp_receive.side_effect = lambda sap, hdr: (wake.wait(5.0), b"")[1]
        mock_bp.bp_interrupt.side_effect = lambda sap: wake.set()

        proxy, ept = self._proxy_with_node_dir(94)

        rx = threading.Thread(target=lambda: ept.bp_receive(timeout=4.0), daemon=True)
        rx.start()
        time.sleep(0.2)

        finished = run_with_watchdog(
            lambda: proxy.bp_close(ept), timeout=3.0, on_timeout=wake.set)
        rx.join(2.0)

        ok = finished and not rx.is_alive()
        print("  [%s] close of a blocked receive (node_dir set)."
              % ("PASS" if ok else "FAIL"))
        self.assertTrue(
            ok, "bp_close could not wake+close a receive that held ion_dir_lock.")


if __name__ == '__main__':
    print("=" * 60)
    print("PYION in_ion_folder LOCK-SCOPE SMOKE TEST")
    print("=" * 60)
    suite = unittest.TestLoader().loadTestsFromTestCase(TestDirLockDeadlock)
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    if result.wasSuccessful():
        print("\nOVERALL STATUS: PASSED")
        sys.exit(0)
    else:
        print("\nOVERALL STATUS: FAILED")
        sys.exit(1)

"""
Real-ION C-layer concurrency regression test.

Exercises the Phase 1 thread-safety fixes in pyion's C extension by driving
the ``_bp`` extension directly (bypassing the Python-level wrappers in
``pyion/bp.py``):

  * Closing an endpoint while another thread is blocked in ``bp_receive``
    must wake the receiver and must not deadlock.
  * Interrupting an endpoint while a thread is blocked in ``bp_receive``
    must wake the receiver and must not deadlock.

Before Phase 1, a lock held across the blocking ``bp_receive`` made the
interrupt/close path deadlock -- and that deadlock occurs *inside* the
``bp_close``/``bp_interrupt`` call itself. Every potentially-blocking C call
is therefore run in its own daemon thread and watchdogged: if a call does not
return within a bounded time it is reported as a deadlock failure rather than
hanging the test run.

Requires a running ION node exposing endpoint ipn:1.1. The companion script
``run_c_concurrency_test.sh`` starts ION before invoking this test.
"""
import sys
import time
import threading

import _bp

# A C call that does not return within this many seconds is treated as a
# deadlock (the operation itself wedged) or as a receiver that was never woken.
OP_TIMEOUT = 10.0
# Time allowed for a receiver thread to actually enter the blocking call.
SETTLE = 1.0

EID = "ipn:1.1"


def call_in_thread(fn):
    """Run fn() in a daemon thread.

    Returns (done_event, box). box gets "value" on success or "exc" on
    failure once done_event is set. A deadlocked fn() simply never sets the
    event, which the caller detects with done_event.wait(timeout).
    """
    done = threading.Event()
    box = {}

    def runner():
        try:
            box["value"] = fn()
        except BaseException as exc:  # noqa: BLE001 - record whatever ION raised
            box["exc"] = exc
        finally:
            done.set()

    threading.Thread(target=runner, daemon=True).start()
    return done, box


def test_close_unblocks_blocked_receiver():
    """base_bp_close must wake a blocked receiver and must itself not deadlock."""
    sap = _bp.bp_open(EID, 0, 0)

    rx_done, rx_box = call_in_thread(lambda: _bp.bp_receive(sap, 0))
    time.sleep(SETTLE)
    if rx_done.is_set():
        raise AssertionError("receiver never blocked; cannot test close")

    # bp_close is itself run in a thread: the Phase 1 regression deadlocks
    # *inside* this call, so it must be watchdogged, not called inline.
    cl_done, cl_box = call_in_thread(lambda: _bp.bp_close(sap))
    if not cl_done.wait(OP_TIMEOUT):
        raise AssertionError("DEADLOCK: bp_close did not return")

    if not rx_done.wait(OP_TIMEOUT):
        raise AssertionError("DEADLOCK: receiver still blocked after close")

    outcome = rx_box.get("exc")
    print("    bp_close returned; receiver woke with %s"
          % (type(outcome).__name__ if outcome else "no exception"))


def test_interrupt_unblocks_blocked_receiver():
    """base_bp_interrupt must wake a blocked receiver and must itself not deadlock."""
    sap = _bp.bp_open(EID, 0, 0)

    rx_done, rx_box = call_in_thread(lambda: _bp.bp_receive(sap, 0))
    time.sleep(SETTLE)
    if rx_done.is_set():
        raise AssertionError("receiver never blocked; cannot test interrupt")

    int_done, _int_box = call_in_thread(lambda: _bp.bp_interrupt(sap))
    if not int_done.wait(OP_TIMEOUT):
        raise AssertionError("DEADLOCK: bp_interrupt did not return")

    if not rx_done.wait(OP_TIMEOUT):
        raise AssertionError("DEADLOCK: receiver still blocked after interrupt")

    outcome = rx_box.get("exc")
    print("    bp_interrupt returned; receiver woke with %s"
          % (type(outcome).__name__ if outcome else "no exception"))

    # The endpoint returns to idle after an interrupt; close it to release it.
    cl_done, _cl_box = call_in_thread(lambda: _bp.bp_close(sap))
    if not cl_done.wait(OP_TIMEOUT):
        raise AssertionError("DEADLOCK: cleanup bp_close did not return")


def test_repeated_close_while_receiving():
    """Repeat the close-while-receiving race to surface intermittent hangs."""
    iterations = 10
    for i in range(iterations):
        sap = _bp.bp_open(EID, 0, 0)
        rx_done, _rx_box = call_in_thread(lambda: _bp.bp_receive(sap, 0))
        time.sleep(0.2)

        cl_done, _cl_box = call_in_thread(lambda: _bp.bp_close(sap))
        if not cl_done.wait(OP_TIMEOUT):
            raise AssertionError("DEADLOCK: bp_close hung on iteration %d/%d"
                                 % (i + 1, iterations))
        if not rx_done.wait(OP_TIMEOUT):
            raise AssertionError("DEADLOCK: receiver hung on iteration %d/%d"
                                 % (i + 1, iterations))
    print("    %d close-while-receiving cycles completed" % iterations)


TESTS = [
    test_close_unblocks_blocked_receiver,
    test_interrupt_unblocks_blocked_receiver,
    test_repeated_close_while_receiving,
]


def main():
    print("=" * 60)
    print("PYION C-LAYER CONCURRENCY REGRESSION TEST")
    print("=" * 60)

    if _bp.bp_attach() is not True:
        print("OVERALL STATUS: FAILED (could not attach to BP)")
        return 1

    failures = 0
    for test in TESTS:
        print("\nRunning: %s" % test.__name__)
        try:
            test()
            print("  [PASS] %s" % test.__name__)
        except Exception as exc:  # noqa: BLE001
            failures += 1
            print("  [FAIL] %s: %s" % (test.__name__, exc))

    print()
    if failures:
        print("OVERALL STATUS: FAILED (%d/%d test(s) failed)" % (failures, len(TESTS)))
        return 1
    print("OVERALL STATUS: PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())

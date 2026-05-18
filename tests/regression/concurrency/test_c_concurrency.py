"""
Real-ION C-layer concurrency regression test.

Exercises the Phase 1 and Phase 2 thread-safety fixes in pyion's C extension
by driving the ``_bp`` and ``_mgmt`` extensions directly (bypassing the
Python-level wrappers in ``pyion/bp.py``).

Phase 1 -- endpoint lifecycle:
  * Closing an endpoint while another thread is blocked in ``bp_receive``
    must wake the receiver and must not deadlock.
  * Interrupting an endpoint while a thread is blocked in ``bp_receive``
    must wake the receiver and must not deadlock.

Phase 2 -- serialization locks:
  * Concurrent ``bp_send`` on one endpoint (per-endpoint send_lock).
  * Concurrent ``_mgmt`` calls (the global mgmt_lock).
  * Concurrent ``bp_attach`` (the global ion_global_lock).

The Phase 2 tests are contention stress tests: they hammer each locked path
from many threads and require every call to complete without a crash, a hang,
or a corruption-induced error. They do not by themselves prove serialization,
but a missing or broken lock would surface here as a crash or an error.

Every potentially-blocking C call is run in a watchdogged thread: a call that
does not return within a bounded time is reported as a deadlock failure rather
than hanging the run.

Requires a running ION node exposing endpoint ipn:1.1. The companion script
``run_c_concurrency_test.sh`` starts ION before invoking this test.
"""
import sys
import time
import threading

import _bp
import _mgmt

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


def run_threads(target, n, timeout):
    """Start n daemon threads running target(); join them against a deadline.

    Raises AssertionError if any worker is still alive when the deadline
    passes -- i.e. a locked path deadlocked.
    """
    threads = [threading.Thread(target=target, daemon=True) for _ in range(n)]
    for t in threads:
        t.start()
    deadline = time.time() + timeout
    for t in threads:
        t.join(max(0.0, deadline - time.time()))
    if any(t.is_alive() for t in threads):
        raise AssertionError("DEADLOCK: a worker thread did not finish in %.0fs"
                             % timeout)


# --------------------------------------------------------------------------
# Phase 1 -- endpoint lifecycle
# --------------------------------------------------------------------------

def test_close_unblocks_blocked_receiver():
    """base_bp_close must wake a blocked receiver and must itself not deadlock."""
    sap = _bp.bp_open(EID, 0, 0)

    rx_done, rx_box = call_in_thread(lambda: _bp.bp_receive(sap, 0))
    time.sleep(SETTLE)
    if rx_done.is_set():
        raise AssertionError("receiver never blocked; cannot test close")

    # bp_close is itself run in a thread: the Phase 1 regression deadlocks
    # *inside* this call, so it must be watchdogged, not called inline.
    cl_done, _cl_box = call_in_thread(lambda: _bp.bp_close(sap))
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


def test_stale_handle_rejected():
    """Using an endpoint handle after close is rejected, not a use-after-free.

    The first close frees the endpoint; the handle is now stale. A second
    close, or any other operation on that handle, must raise rather than
    dereference freed memory.
    """
    sap = _bp.bp_open(EID, 0, 0)

    cl_done, _cl_box = call_in_thread(lambda: _bp.bp_close(sap))
    if not cl_done.wait(OP_TIMEOUT):
        raise AssertionError("DEADLOCK: first bp_close did not return")

    # Each of these targets the now-stale handle and must raise.
    stale_ops = {
        "bp_close": lambda: _bp.bp_close(sap),
        "bp_interrupt": lambda: _bp.bp_interrupt(sap),
        "bp_send": lambda: _bp.bp_send(sap, "ipn:1.2", None, 3600, 1, 0, 0, 0, 0, b"x"),
        "bp_receive": lambda: _bp.bp_receive(sap, 0),
    }
    for name, op in stale_ops.items():
        try:
            op()
        except Exception:  # noqa: BLE001 - any raised error is acceptable
            continue
        raise AssertionError("%s on a stale handle did not raise" % name)
    print("    stale handle rejected by close/interrupt/send/receive")


# --------------------------------------------------------------------------
# Phase 2 -- serialization locks
# --------------------------------------------------------------------------

def test_concurrent_sends():
    """Concurrent bp_send on one endpoint (per-endpoint send_lock)."""
    threads = 6
    per_thread = 20
    sap = _bp.bp_open(EID, 0, 0)

    def sender():
        for _ in range(per_thread):
            try:
                # The bundle has nowhere to route on this minimal node; a
                # routing/limbo error is fine. We are testing that the C send
                # path does not crash or corrupt under concurrent callers.
                _bp.bp_send(sap, "ipn:1.2", None, 3600, 1, 0, 0, 0, 0, b"x")
            except Exception:  # noqa: BLE001
                pass

    run_threads(sender, threads, OP_TIMEOUT)

    cl_done, _cl_box = call_in_thread(lambda: _bp.bp_close(sap))
    if not cl_done.wait(OP_TIMEOUT):
        raise AssertionError("DEADLOCK: bp_close hung after concurrent sends")
    print("    %d threads x %d concurrent sends completed"
          % (threads, per_thread))


def test_concurrent_send_and_receive():
    """Concurrent bp_send and bp_receive on the same endpoint.

    Send and receive use different locks, so they are allowed to run at the
    same time on one endpoint; this checks that doing so does not crash or
    corrupt the SAP.
    """
    sap = _bp.bp_open(EID, 0, 0)

    rx_done, _rx_box = call_in_thread(lambda: _bp.bp_receive(sap, 0))
    time.sleep(SETTLE)
    if rx_done.is_set():
        raise AssertionError("receiver never blocked; cannot test send+receive")

    def sender():
        for _ in range(20):
            try:
                _bp.bp_send(sap, "ipn:1.2", None, 3600, 1, 0, 0, 0, 0, b"x")
            except Exception:  # noqa: BLE001
                pass

    run_threads(sender, 4, OP_TIMEOUT)

    cl_done, _cl_box = call_in_thread(lambda: _bp.bp_close(sap))
    if not cl_done.wait(OP_TIMEOUT):
        raise AssertionError("DEADLOCK: bp_close hung after send+receive")
    if not rx_done.wait(OP_TIMEOUT):
        raise AssertionError("DEADLOCK: receiver still blocked after close")
    print("    concurrent send + receive on one endpoint completed")


def test_concurrent_mgmt_calls():
    """Concurrent _mgmt read calls (the global mgmt_lock)."""
    threads = 8
    per_thread = 30
    errors = []

    def hammer():
        for _ in range(per_thread):
            try:
                _mgmt.list_contacts()
                _mgmt.list_ranges()
                _mgmt.bp_endpoint_exists(EID)
            except Exception as exc:  # noqa: BLE001
                errors.append(exc)

    run_threads(hammer, threads, OP_TIMEOUT)

    if errors:
        raise AssertionError("%d _mgmt call(s) failed under contention; first: %r"
                             % (len(errors), errors[0]))
    print("    %d threads x %d concurrent _mgmt call-sets completed"
          % (threads, per_thread))


def test_concurrent_attach():
    """Concurrent bp_attach (the global ion_global_lock)."""
    threads = 8
    per_thread = 20
    results = []

    def attacher():
        for _ in range(per_thread):
            results.append(_bp.bp_attach())

    run_threads(attacher, threads, OP_TIMEOUT)

    if not results or not all(r is True for r in results):
        raise AssertionError("bp_attach did not return True under contention")
    print("    %d threads x %d concurrent bp_attach calls completed"
          % (threads, per_thread))


# --------------------------------------------------------------------------
# Deferred -- waiting on an external dependency, not executed yet
# --------------------------------------------------------------------------

def test_concurrent_open_same_endpoint():
    """Concurrent bp_open of the same endpoint from several threads.

    DEFERRED to ION 4.2.0. ION 4.1.4-a.2's bp_open is not safe for a
    concurrent open of the same endpoint; an ION 4.2.0 update is required
    before this can pass. Until the test node moves to ION 4.2.0 this case
    is listed in DEFERRED_TESTS and is not executed.

    Expected behaviour once enabled: exactly one bp_open(EID) succeeds, the
    others raise cleanly (endpoint already in use) with no crash, and the
    winning handle can then be used and closed.
    """
    n = 6
    results = []
    results_lock = threading.Lock()

    def opener():
        try:
            outcome = ("opened", _bp.bp_open(EID, 0, 0))
        except Exception as exc:  # noqa: BLE001
            outcome = ("error", exc)
        with results_lock:
            results.append(outcome)

    run_threads(opener, n, OP_TIMEOUT)

    opened = [h for kind, h in results if kind == "opened"]
    if len(opened) != 1:
        raise AssertionError(
            "expected exactly 1 successful bp_open, got %d" % len(opened))

    # The endpoint that won must be usable and closable.
    cl_done, _cl_box = call_in_thread(lambda: _bp.bp_close(opened[0]))
    if not cl_done.wait(OP_TIMEOUT):
        raise AssertionError("DEADLOCK: bp_close hung after concurrent open")
    print("    one bp_open won; the rest were rejected cleanly")


# Tests deferred until an external dependency lands. They are kept here,
# version-controlled and visible, but are not executed -- main() reports them
# as skipped. Move an entry into TESTS once its dependency is satisfied.
DEFERRED_TESTS = [
    (test_concurrent_open_same_endpoint,
     "deferred to ION 4.2.0: concurrent bp_open of one endpoint is not safe "
     "until the ION 4.2.0 update is complete"),
]


TESTS = [
    test_close_unblocks_blocked_receiver,
    test_interrupt_unblocks_blocked_receiver,
    test_repeated_close_while_receiving,
    test_stale_handle_rejected,
    test_concurrent_sends,
    test_concurrent_send_and_receive,
    test_concurrent_mgmt_calls,
    test_concurrent_attach,
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

    # Deferred tests are reported but not run, and do not affect the result.
    for test, reason in DEFERRED_TESTS:
        print("\nDeferred: %s" % test.__name__)
        print("  [SKIP] %s" % reason)

    print()
    if failures:
        print("OVERALL STATUS: FAILED (%d/%d test(s) failed)" % (failures, len(TESTS)))
        return 1
    print("OVERALL STATUS: PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())

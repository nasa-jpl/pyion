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
import _ltp
import _cfdp

# A C call that does not return within this many seconds is treated as a
# deadlock (the operation itself wedged) or as a receiver that was never woken.
OP_TIMEOUT = 10.0
# Time allowed for a receiver thread to actually enter the blocking call.
SETTLE = 1.0

EID = "ipn:1.1"
LTP_CLIENT = 4  # an LTP client id not used by BP/SDA/CFDP
CFDP_ENTITY = 1  # the local node; CFDP ops may fail with no route, which the
                 # lifecycle tests tolerate -- they exercise the handle layer.


def cfdp_open_handle():
    """Open a CFDP entity and return its opaque handle.

    Args mirror _cfdp.cfdp_open(entityId, lifespan, classOfService, ordinal,
    srrFlags, criticality).
    """
    return _cfdp.cfdp_open(CFDP_ENTITY, 86400, 0, 0, 0, 0)


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
# LTP -- the _ltp extension, hardened with the same design as _bp
# --------------------------------------------------------------------------

def test_ltp_close_unblocks_receiver():
    """base_ltp_close must wake a thread blocked in ltp_receive."""
    sap = _ltp.ltp_open(LTP_CLIENT)

    rx_done, _rx_box = call_in_thread(lambda: _ltp.ltp_receive(sap))
    time.sleep(SETTLE)
    if rx_done.is_set():
        raise AssertionError("LTP receiver never blocked; cannot test close")

    cl_done, _cl_box = call_in_thread(lambda: _ltp.ltp_close(sap))
    if not cl_done.wait(OP_TIMEOUT):
        raise AssertionError("DEADLOCK: ltp_close did not return")
    if not rx_done.wait(OP_TIMEOUT):
        raise AssertionError("DEADLOCK: LTP receiver still blocked after close")
    print("    ltp_close returned; LTP receiver woke")


def test_ltp_interrupt_unblocks_receiver():
    """base_ltp_interrupt must wake a thread blocked in ltp_receive."""
    sap = _ltp.ltp_open(LTP_CLIENT)

    rx_done, _rx_box = call_in_thread(lambda: _ltp.ltp_receive(sap))
    time.sleep(SETTLE)
    if rx_done.is_set():
        raise AssertionError("LTP receiver never blocked; cannot test interrupt")

    int_done, _int_box = call_in_thread(lambda: _ltp.ltp_interrupt(sap))
    if not int_done.wait(OP_TIMEOUT):
        raise AssertionError("DEADLOCK: ltp_interrupt did not return")
    if not rx_done.wait(OP_TIMEOUT):
        raise AssertionError("DEADLOCK: LTP receiver still blocked after interrupt")

    cl_done, _cl_box = call_in_thread(lambda: _ltp.ltp_close(sap))
    if not cl_done.wait(OP_TIMEOUT):
        raise AssertionError("DEADLOCK: cleanup ltp_close did not return")
    print("    ltp_interrupt returned; LTP receiver woke")


def test_ltp_stale_handle_rejected():
    """Using an LTP handle after close is rejected, not a use-after-free."""
    sap = _ltp.ltp_open(LTP_CLIENT)

    cl_done, _cl_box = call_in_thread(lambda: _ltp.ltp_close(sap))
    if not cl_done.wait(OP_TIMEOUT):
        raise AssertionError("DEADLOCK: first ltp_close did not return")

    stale_ops = {
        "ltp_close": lambda: _ltp.ltp_close(sap),
        "ltp_interrupt": lambda: _ltp.ltp_interrupt(sap),
        "ltp_send": lambda: _ltp.ltp_send(sap, 1, b"x"),
        "ltp_receive": lambda: _ltp.ltp_receive(sap),
    }
    for name, op in stale_ops.items():
        try:
            op()
        except Exception:  # noqa: BLE001 - any raised error is acceptable
            continue
        raise AssertionError("%s on a stale LTP handle did not raise" % name)
    print("    stale LTP handle rejected by close/interrupt/send/receive")


# --------------------------------------------------------------------------
# CFDP -- entity (params) lifecycle and global event handling
# --------------------------------------------------------------------------
# _cfdp holds the request-params block through an opaque, never-reused handle
# resolved by a registry, with a refcount/deferred-free lifecycle and a
# per-handle op_lock. The process-global cfdp_get_event / cfdp_interrupt take
# no handle and are never lock-wrapped.

def test_cfdp_stale_handle_rejected():
    """Every _cfdp entry point rejects a closed handle instead of a UAF."""
    h = cfdp_open_handle()
    _cfdp.cfdp_close(h)

    # Each of these resolves the (now freed) handle through the registry and
    # must raise ValueError (PYION_INVALID_HANDLE_ERR), never touch freed memory.
    checks = [
        ("close",    lambda: _cfdp.cfdp_close(h)),
        ("cancel",   lambda: _cfdp.cfdp_cancel(h)),
        ("suspend",  lambda: _cfdp.cfdp_suspend(h)),
        ("resume",   lambda: _cfdp.cfdp_resume(h)),
        ("report",   lambda: _cfdp.cfdp_report(h)),
        ("add_usr_msg", lambda: _cfdp.cfdp_add_usr_msg(h, "x")),
        ("add_fs_req",  lambda: _cfdp.cfdp_add_filestore_request(h, 0, "a", "b")),
        ("send",     lambda: _cfdp.cfdp_send(h, "/tmp/x", "/tmp/y", 0, 0, 0)),
        ("request",  lambda: _cfdp.cfdp_request(h, "/tmp/x", "/tmp/y", 0, 0, 0)),
    ]
    for name, fn in checks:
        try:
            fn()
        except ValueError:
            continue
        except Exception as exc:  # noqa: BLE001
            raise AssertionError(
                "cfdp_%s on a stale handle raised %r, expected ValueError"
                % (name, exc))
        raise AssertionError("cfdp_%s on a stale handle did not raise" % name)
    print("    stale CFDP handle rejected by every entry point")


def test_cfdp_concurrent_add_usr_msg_same_handle():
    """Concurrent request-mutating ops on one handle are serialized (op_lock).

    Many threads add user messages to the same entity at once; without the
    per-handle op_lock the read-create-assign of params->msgsToUser and the
    MetadataList mutation would race. Every call must succeed.
    """
    h = cfdp_open_handle()
    errors = []
    errlock = threading.Lock()

    def worker():
        try:
            for _ in range(20):
                _cfdp.cfdp_add_usr_msg(h, "msg")
        except Exception as exc:  # noqa: BLE001
            with errlock:
                errors.append(exc)

    run_threads(worker, 6, OP_TIMEOUT)
    _cfdp.cfdp_close(h)
    if errors:
        raise AssertionError("concurrent add_usr_msg failed: %r" % errors[0])
    print("    6 threads x 20 concurrent add_usr_msg completed")


def test_cfdp_concurrent_open_close():
    """Concurrent open+close of distinct handles stresses the registry.

    Exercises registry register/unregister and the refcount deferred free from
    many threads; a broken registry would crash or corrupt rather than run.
    """
    errors = []
    errlock = threading.Lock()

    def worker():
        try:
            for _ in range(10):
                h = cfdp_open_handle()
                _cfdp.cfdp_close(h)
        except Exception as exc:  # noqa: BLE001
            with errlock:
                errors.append(exc)

    run_threads(worker, 6, OP_TIMEOUT)
    if errors:
        raise AssertionError("concurrent open/close failed: %r" % errors[0])
    print("    6 threads x 10 concurrent open/close completed")


def test_cfdp_close_while_ops_in_flight():
    """Closing a handle while ops run on it must defer the free, not UAF.

    Workers hammer a shared handle; the main thread closes it mid-flight. An
    in-flight op holds a refcount so the params block is freed only once it
    returns (deferred free); an op that starts after the close is rejected
    cleanly. Under AddressSanitizer a premature free would surface here.
    """
    h = cfdp_open_handle()
    stop = threading.Event()
    other = []
    errlock = threading.Lock()

    # Use cfdp_report as the in-flight op: it resolves the handle, takes the
    # op_lock and calls into ION on the (empty) transaction id, so it drives
    # the acquire/op_lock/release path without allocating SDR memory. On a live
    # handle ION reports "no such transaction" (RuntimeError), which is fine --
    # we are stressing the lifecycle, not the report itself; on a closed handle
    # the registry rejects it with ValueError.
    def worker():
        while not stop.is_set():
            try:
                _cfdp.cfdp_report(h)
            except ValueError:
                return  # handle was closed; expected, stop
            except RuntimeError:
                pass    # no transaction to report; keep exercising the path
            except Exception as exc:  # noqa: BLE001
                with errlock:
                    other.append(exc)
                return

    threads = [threading.Thread(target=worker, daemon=True) for _ in range(6)]
    for t in threads:
        t.start()
    time.sleep(0.3)  # let the workers get in flight

    _cfdp.cfdp_close(h)  # deferred free while ops are running
    stop.set()

    deadline = time.time() + OP_TIMEOUT
    for t in threads:
        t.join(max(0.0, deadline - time.time()))
    if any(t.is_alive() for t in threads):
        raise AssertionError("DEADLOCK: a CFDP worker did not finish")
    if other:
        raise AssertionError("unexpected error during close-in-flight: %r" % other[0])

    # After close the handle is gone: a fresh op must be rejected, not a UAF.
    try:
        _cfdp.cfdp_report(h)
    except ValueError:
        pass
    else:
        raise AssertionError("op on closed CFDP handle did not raise")
    print("    close during in-flight ops deferred the free; stale op rejected")


def test_cfdp_interrupt_unblocks_next_event():
    """cfdp_interrupt_events must wake a thread blocked in cfdp_next_event.

    The event path is process-global and lock-free; interrupt must not
    deadlock against it.
    """
    done, box = call_in_thread(lambda: _cfdp.cfdp_next_event())
    time.sleep(SETTLE)
    _cfdp.cfdp_interrupt_events()
    if not done.wait(OP_TIMEOUT):
        raise AssertionError("DEADLOCK: cfdp_next_event not woken by interrupt")
    if "exc" in box:
        raise AssertionError("cfdp_next_event raised after interrupt: %r" % box["exc"])
    print("    cfdp_interrupt_events woke a blocked next_event")


# --------------------------------------------------------------------------
# Concurrent open of the same endpoint (requires the ION 4.2.0+ bp_open fix)
# --------------------------------------------------------------------------

def test_concurrent_open_same_endpoint():
    """Concurrent bp_open of the same endpoint from several threads.

    This was deferred on ION 4.1.4-a.2, whose bp_open is not safe for a
    concurrent open of the same endpoint. ION 4.2.0 fixed that; verified
    passing against ION 4.2.1-a.1, so the case is now active.

    Exactly one bp_open(EID) succeeds, the others raise cleanly (endpoint
    already in use) with no crash, and the winning handle can then be used
    and closed.
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


# Tests deferred until an external dependency lands: kept version-controlled
# and visible but not executed (main() reports them as skipped). An entry
# moves into TESTS once its dependency is satisfied. Currently empty -- the
# concurrent-open case moved to TESTS once ION reached 4.2.0+.
DEFERRED_TESTS = []


TESTS = [
    test_close_unblocks_blocked_receiver,
    test_interrupt_unblocks_blocked_receiver,
    test_repeated_close_while_receiving,
    test_stale_handle_rejected,
    test_concurrent_sends,
    test_concurrent_send_and_receive,
    test_concurrent_mgmt_calls,
    test_concurrent_attach,
    test_ltp_close_unblocks_receiver,
    test_ltp_interrupt_unblocks_receiver,
    test_ltp_stale_handle_rejected,
    test_cfdp_stale_handle_rejected,
    test_cfdp_concurrent_add_usr_msg_same_handle,
    test_cfdp_concurrent_open_close,
    test_cfdp_close_while_ops_in_flight,
    test_cfdp_interrupt_unblocks_next_event,
    test_concurrent_open_same_endpoint,
]


def main():
    print("=" * 60)
    print("PYION C-LAYER CONCURRENCY REGRESSION TEST")
    print("=" * 60)

    if _bp.bp_attach() is not True:
        print("OVERALL STATUS: FAILED (could not attach to BP)")
        return 1
    try:
        _ltp.ltp_attach()
    except Exception as exc:  # noqa: BLE001
        print("OVERALL STATUS: FAILED (could not attach to LTP: %s)" % exc)
        return 1
    try:
        _cfdp.cfdp_attach()
    except Exception as exc:  # noqa: BLE001
        print("OVERALL STATUS: FAILED (could not attach to CFDP: %s)" % exc)
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

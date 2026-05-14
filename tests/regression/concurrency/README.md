# Pyion Concurrency Regression Test

## Purpose
This test verifies the thread-safety fixes implemented for Python 3.13+ compatibility. It specifically checks:
1. **Global Directory Locking**: Ensures that concurrent calls to ION nodes do not interfere with each other's working directory (`os.chdir`).
2. **Endpoint Serialization**: Ensures that concurrent operations on the same Bundle Protocol endpoint (SAP) are serialized to prevent race conditions in the underlying C library.

## How it Works
The test uses Python's `unittest` and `unittest.mock` to simulate the ION C extensions. This allows the logic of the locking mechanisms to be verified without requiring a full ION installation or Docker environment.

## How to Run
From the root of the repository, run:
```bash
python3 tests/regression/concurrency/test_concurrency.py
```

## Expected Results

### Passing
* **Output**: Should show `[PASS]` for both `test_directory_lock` and `test_endpoint_serialization`.
* **Exit Code**: 0.
* **Console Summary**: `OVERALL STATUS: PASSED`.

### Failing
* **Output**: Will show `[FAIL]` and a traceback.
* **Exit Code**: 1.
* **Console Summary**: `OVERALL STATUS: FAILED`.
* **Typical Failure Cause**: One or more of the `threading.Lock` implementations in `pyion/utils.py` or `pyion/bp.py` have been removed or are malfunctioning, allowing threads to interleave their operations.

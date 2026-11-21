"""Root pytest configuration.

Ensures ``pyion/_build_info.py`` exists before any test imports ``pyion``.
That file is generated (by ``setup.py`` or ``scripts/ionpin.py
--write-build-info``) and gitignored, so a fresh checkout does not have it.
Tests that spawn fresh ``python -c "import pyion"`` subprocesses need the
real file on disk; ``sys.modules`` seeding would not reach them.
"""

import os
import sys

_REPO_ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_REPO_ROOT, 'scripts'))

# Several files under tests/ are not pytest tests and must not be collected:
#
# * tests/regression/concurrency/test_concurrency.py and
#   test_dir_lock_deadlock.py are standalone scripts (run as
#   ``python3 <script>``, as CI does). They stub the ION C extension modules
#   in sys.modules at import time, which leaks into any pytest session that
#   imports them and breaks tests that import the real pyion.
# * tests/regression/concurrency/test_c_concurrency.py drives a live ION
#   node through the C extensions; it is exercised only via
#   run_c_concurrency_test.sh inside ION's container. Collected bare, it
#   aborts the interpreter on the first blocking call.
# * tests/cfdp_tests/ contains interactive CFDP sample/demo programs, not
#   automated tests (they require a running ION node and a display).
collect_ignore = [
    os.path.join('tests', 'regression', 'concurrency', 'test_concurrency.py'),
    os.path.join('tests', 'regression', 'concurrency', 'test_dir_lock_deadlock.py'),
    os.path.join('tests', 'regression', 'concurrency', 'test_c_concurrency.py'),
    os.path.join('tests', 'cfdp_tests'),
]

_BUILD_INFO = os.path.join(_REPO_ROOT, 'pyion', '_build_info.py')

if not os.path.exists(_BUILD_INFO):
    from ionpin import write_build_info
    write_build_info(_REPO_ROOT)

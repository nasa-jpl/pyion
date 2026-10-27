Introduction
------------

This repository contains the source code of **pyion**, a Python extension for the Interplanetary Overlay Network.
Pyion follows the release schedule of ION and places the relevant codebase in separate branches
(e.g., for ION-3.7.0, checkout branch v3.7.0).

> **Note: free-threaded Python compatibility**
>
> This concerns only the *free-threaded* build of Python 3.13+ (`python3.13t`,
> built with `--disable-gil`), a separate, opt-in build. Stock CPython keeps the
> GIL and is unaffected.
>
> As of this release all five C extensions (`_bp`, `_mgmt`, `_ltp`, `_cfdp`,
> `_mem`) and the Python layer (`pyion.bp` / `pyion.utils`) are hardened for
> thread-safety -- opaque handle registries with refcount/deferred-free
> lifecycles, per-handle and global locks -- and declare `Py_MOD_GIL_NOT_USED`.
> Importing the full `pyion` package on a free-threaded build therefore leaves
> the GIL disabled, and pyion's own C code is free of data races and
> use-after-free there (verified against a real ION node under
> AddressSanitizer/UBSan and, on a GIL-disabled build, ThreadSanitizer).
>
> Note that ION itself is the layer below pyion; pyion serializes its own use
> of ION, but ION's own threading guarantees are out of pyion's scope.

Pyion's documentation can be found at https://pyion.readthedocs.io/en/latest/.

ION's official repository is now available at https://github.com/nasa-jpl/ION-DTN.

ION's official documentation is available at https://ion-dtn.readthedocs.io/en/latest/.

Regression Testing
------------------

Pyion includes a suite of regression tests to verify thread-safety and concurrency handling. These tests are located in ``tests/regression/concurrency``.

**Python-level Concurrency Test** (``test_concurrency.py``):
Verifies that the Python-level locks (global directory lock and endpoint-level serialization) correctly prevent race conditions.

To run:
```bash
export PYTHONPATH=$PYTHONPATH:.
export PYION_BP_VERSION=BPv7
python3 tests/regression/concurrency/test_concurrency.py
```

License Terms
-------------

Copyright (c) 2019, California Institute of Technology ("Caltech").  
U.S. Government sponsorship acknowledged.

All rights reserved.

Redistribution and use in source and binary forms, with or without modification, 
are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, 
  this list of conditions and the following disclaimer.
* Redistributions must reproduce the above copyright notice, this list 
  of conditions and the following disclaimer in the documentation and/or other 
  materials provided with the distribution.
* Neither the name of Caltech nor its operating division, the Jet Propulsion Laboratory, 
  nor the names of its contributors may be used to endorse or promote products 
  derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND 
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED 
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. 
IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, 
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) 
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY 
OF SUCH DAMAGE.

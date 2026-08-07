Introduction
------------

This repository contains the source code of **pyion**, a Python extension for the Interplanetary Overlay Network.
Pyion follows the release schedule of ION and places the relevant codebase in separate branches
(e.g., for ION-3.7.0, checkout branch v3.7.0).

> **Note: free-threaded Python compatibility**
>
> This affects only the *free-threaded* build of Python 3.13+ (`python3.13t`,
> built with `--disable-gil`), which is a separate, opt-in build. Stock CPython
> 3.13+ keeps the GIL and is unaffected.
>
> **PYION is currently NOT fully thread-safe** without the GIL. If you are running
> a free-threaded build, set the environment variable `PYTHON_GIL=1` (or pass
> `-X gil=1`) to re-enable the GIL and avoid race conditions and potential crashes.

Pyion's documentation can be found at https://pyion.readthedocs.io/en/latest/.

ION's official repository is now available at https://github.com/nasa-jpl/ION-DTN.

ION's official documentation is available at https://nasa-jpl.github.io/ION-DTN/.

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

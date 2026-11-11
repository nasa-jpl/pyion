# pyion test images

This directory holds two independent Docker harnesses, both targeting
**ION 4.2.1-a.1** and both building ION from source so no local ION checkout
is needed:

| Harness | Files | Drives |
| --- | --- | --- |
| **Concurrency / thread-safety** | `Dockerfile`, `Dockerfile.freethreaded` | the single-node C-layer suite in `tests/regression/concurrency/` (also run by `.github/workflows/regression.yml`) |
| **Portable functional** | `Dockerfile.functional`, `compose.yaml`, `run.sh` | the two-node protocol tests in `tests/<proto>_tests/` |

In every image pyion is **not** baked in: the containers bind-mount this
repository and run `python3 setup.py install` at start, so you can edit pyion
and re-run without rebuilding the image.

## Concurrency image

`Dockerfile` builds an image that bundles ION-DTN and the build toolchain.
It is the image the test compose files (`tests/*/test_network.yaml`) expect
under the name `pyion_bpv7:4.2.1a1`.

The image is **self-contained**: ION-DTN is downloaded from the official
`nasa-jpl/ION-DTN` repository and built during the image build. No local ION
source tree is required.

### Build

```bash
# from the repository root
docker build -t pyion_bpv7:4.2.1a1 -f docker/Dockerfile docker/
```

The build downloads ION-DTN, compiles and installs its public API into
`/usr/local`, and leaves the ION source tree at
`/home/ion-open-source-4.2.1a1`. `setup.py` needs both: `/usr/local` for the
public headers/libraries and `ION_HOME` (the source tree) for ION's private
headers.

To build against a different ION version, override the build args. `ION_TAG`
is the git tag in the ION-DTN repository; `ION_VERSION` names the install
directory and must match the `ION_HOME` used by the test compose files:

```bash
docker build \
    --build-arg ION_TAG=ion-open-source-<tag> \
    --build-arg ION_VERSION=<version> \
    -t pyion_bpv7:<version> -f docker/Dockerfile docker/
```

### Run the concurrency tests

The compose files reference Windows-style host paths from the original author
(`C:/Users/mnet/...`). Update the `volumes:` entries to point at your local
checkout of this repository before running, e.g.:

```yaml
volumes:
  - /abs/path/to/pyion:/home/ion-interface
  - /abs/path/to/pyion/tests/bp_tests/nodes/1:/home/ion-interface/tests/bp_tests/nodes/1
```

Then:

```bash
docker compose -f tests/bp_tests/test_network.yaml up
```

### Free-threaded image

`Dockerfile.freethreaded` builds a second image that bundles ION-DTN with a
free-threaded (GIL-disabled, PEP 703) CPython 3.13 compiled from source. It is
used to validate pyion under free-threading and to run ThreadSanitizer (its
Ubuntu 24.04 toolchain ships a libtsan that works with modern kernel ASLR).

```bash
docker build -t pyion_ft:4.2.1a1 -f docker/Dockerfile.freethreaded docker/
```

Run the concurrency suite under free-threading with `PYTHON_GIL=0`:

```bash
docker run --rm --shm-size=256m -e PYTHON_GIL=0 \
    -v "$PWD":/home/ion-interface pyion_ft:4.2.1a1 bash -c '
        cd /home/ion-interface && python3 setup.py install &&
        bash tests/regression/concurrency/run_c_concurrency_test.sh'
```

ThreadSanitizer additionally needs reduced mmap randomization on the host
(`sudo sysctl -w vm.mmap_rnd_bits=28`); see the `free-threaded` job in
`.github/workflows/regression.yml`.

### Notes

- The default image is based on Ubuntu 22.04 (Python 3.10) and tests pyion
  under the GIL; the free-threaded image above covers GIL-disabled CPython.
- `valgrind` and `gdb` are installed, and `build-essential` provides the
  AddressSanitizer/ThreadSanitizer runtimes, for the C-layer concurrency
  stress tests.

## Portable functional tests

`Dockerfile.functional` + `compose.yaml` bring up a **two-node ION network**
and run the existing protocol tests under `tests/<proto>_tests/`. Unlike the
per-directory `tests/*/test_network.yaml` files (which carry the original
author's Windows host paths), this harness is host-independent: it mounts the
repo from one directory up and picks the protocol with `PYION_TEST`.

Quick start, from the repository root:

```bash
docker/run.sh bp          # bp | cfdp | ltp | mem  (first run builds ION)
```

or equivalently:

```bash
PYION_TEST=bp docker compose -f docker/compose.yaml up --build
```

When both nodes report that they are up, drive the test from two more shells
(receiver first):

```bash
docker exec -it pyion_node2 bash -lc 'cd /work/bp_tests && python3 rx.py'
docker exec -it pyion_node1 bash -lc 'cd /work/bp_tests && python3 tx.py'
```

Tear down with `docker/run.sh bp down` (or `docker compose -f docker/compose.yaml down`).

Each node runs the test from a container-internal copy of the test directory
(`/work/<proto>_tests`), not the mounted repo, so ION's runtime writes and the
scripts' `chmod` never dirty your working tree. pyion is still built from the
mounted working copy. To build against a different ION version, override the
build arg: `docker build --build-arg ION_REF=ion-open-source-<tag> ...`.

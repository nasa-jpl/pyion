# pyion test image

`Dockerfile` builds an image that bundles ION-DTN and the build toolchain.
It is the image the test compose files (`tests/*/test_network.yaml`) expect
under the name `pyion_bpv7:4.1.4a2`.

The image is **self-contained**: ION-DTN is downloaded from the official
`nasa-jpl/ION-DTN` repository and built during the image build. No local ION
source tree is required.

pyion itself is **not** compiled into the image. The compose files bind-mount
this repository into the container and run `python3 setup.py install` at
startup, so you can edit and rebuild pyion without rebuilding the image.

## Build

```bash
# from the repository root
docker build -t pyion_bpv7:4.1.4a2 -f docker/Dockerfile docker/
```

The build downloads ION-DTN, compiles and installs its public API into
`/usr/local`, and leaves the ION source tree at
`/home/ion-open-source-4.1.4a2`. `setup.py` needs both: `/usr/local` for the
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

## Run the tests

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

## Notes

- The image is based on Ubuntu 22.04 (Python 3.10) and is suitable for
  testing pyion under the GIL. Validating free-threading (`python3.13t`)
  needs a base image with a free-threaded Python build; that is a later
  step in the C-layer thread-safety work.
- `valgrind` and `gdb` are installed, and `build-essential` provides the
  AddressSanitizer runtime, for the C-layer concurrency stress tests.

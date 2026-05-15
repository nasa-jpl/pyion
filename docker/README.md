# pyion test image

`Dockerfile` builds an image that bundles ION-DTN and the build toolchain.
It is the image the test compose files (`tests/*/test_network.yaml`) expect
under the name `pyion_bpv7:4.1.4a2`.

pyion itself is **not** compiled into the image. The compose files bind-mount
this repository into the container and run `python3 setup.py install` at
startup, so you can edit and rebuild pyion without rebuilding the image.

## What you must provide

The Dockerfile needs the ION-DTN source tree in its build context. Place it in
this `docker/` directory as a directory named:

```
docker/ion-open-source-4.1.4a2/
```

(For a different version, use `ion-open-source-<version>/` and pass
`--build-arg ION_VERSION=<version>`.)

The ION source is not committed to this repository: it is large and carries
its own license. Obtain the ION-DTN 4.1.4-a.2 release from the official
project — see the ION links in the top-level `README.md` — and unpack it here.

## Build

```bash
# from the repository root
docker build -t pyion_bpv7:4.1.4a2 -f docker/Dockerfile docker/
```

The build compiles and installs ION's public API into `/usr/local` and leaves
the ION source tree at `/home/ion-open-source-4.1.4a2`. `setup.py` needs both:
`/usr/local` for the public headers/libraries and `ION_HOME` (the source tree)
for ION's private headers.

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

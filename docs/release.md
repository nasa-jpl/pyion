# Releasing pyion wheels

## Tag scheme

The release tag is `pyion-v<public PYION_VERSION>`, for example `pyion-v4.2.1a1` (the part of
`PYION_VERSION` before `+`). The wheel version is `4.2.1a1+ion4.2.1`. The BP version is not part of the version
string because ION 4.2.x supports BPv7 only.

Earlier pre-releases used the hyphenated ION form (`pyion-v4.2.1-a.1-rc0`, `-rc1`). They are kept as published
and are not stable releases.

## Workflow-driven release

`.github/workflows/build-wheels.yml` runs on pull requests and pushes to `master` that touch packaging, and on
manual runs:

- `pin`: reads `ion-pin.env` and derives the release tag.
- `build`: builds wheels for CPython 3.9-3.13 in the pinned `manylinux_2_28` image (`scripts/build_wheels.sh`).
- `smoke`: installs and tests each wheel (`scripts/smoke_test.sh`).
- `prerelease`: runs for same-repository pull requests and manual runs. It publishes a pre-release tagged
  `<release tag>-pr<number>-run<run>` or `<release tag>-rc<run>`, then checks the assets.
- `release`: runs on a push to `master` after `smoke` passes. It publishes the stable release `<release tag>`
  unless that release already exists, then checks the assets. A merge that does not change the pyion version
  therefore publishes nothing.

To release a new version, change `ion-pin.env` in a pull request. Its checks run before merge. Merging to
`master` publishes the release.

Check the Release page after the first run of a new version: it should show five wheels and `SHA256SUMS`.

## Manual runs

Run the workflow from the Actions tab with `workflow_dispatch`. It builds, smoke-tests, and publishes a
pre-release tagged `<release tag>-rc<run>`. It never publishes the stable release.

## Manual fallback

Use this only if the workflow cannot run. It needs your own GitHub token, and no token is stored in the repository.

```sh
. ./ion-pin.env
RELEASE_TAG="pyion-v${PYION_VERSION%%+*}"
CONTAINER_ENGINE=podman scripts/build_wheels.sh "$ION_TAG" dist
for whl in dist/*.whl; do CONTAINER_ENGINE=podman scripts/smoke_test.sh "$whl" "$ION_TAG"; done
gh release create "$RELEASE_TAG" dist/*.whl dist/SHA256SUMS --title "pyion $RELEASE_TAG"
scripts/check_release.sh "$RELEASE_TAG"
```

## Moving to a new ION release

Each ION release gets its own branch and its own pyion release line (for example `v4.2.1-a.1` for ION 4.2.1-a.1).

1. Download the ION tag archive `https://github.com/nasa-jpl/ION-DTN/archive/refs/tags/<tag>.tar.gz` and record its `sha256sum`.
2. Edit `ion-pin.env`: `ION_TAG`, `ION_BUILD_VERSION`, `ION_SHA256`, and `PYION_VERSION` (`<pyion-version>+ion<ion-version>`). The build, smoke, and workflow scripts read this file.
3. Update the matching `ARG` defaults in `docker/Dockerfile` (`ION_TAG`, `ION_SHA256`), `docker/Dockerfile.freethreaded` (`ION_TAG`, `ION_SHA256`), and `docker/Dockerfile.functional` (`ION_REF`).
4. Update the version in the places that carry it for humans: the README install section and example wheel filename, `docs/index.rst`, `docs/conf.py`, `docker/README.md`, the image tags in `scripts/run_regression.sh`, `docker/compose.yaml`, and `tests/*/test_network.yaml`, the error text in `pyion/_preflight.py`, and the version in the examples in this document.
5. Run `tests/test_release_pipeline.py`. Its consistency tests fail if the Dockerfiles or workflow drift from `ion-pin.env`. Then run the wheel build, smoke test, and regression run for the new release.
6. Tag as described above, with a throwaway `-rc` tag first.

## Known deviations

- Wheel filenames carry compound manylinux tags (for example `manylinux2014_x86_64.manylinux_2_17_x86_64.manylinux_2_28_x86_64`) added by `auditwheel`. The `manylinux_2_28_x86_64` tag is present.
- The loopback test uses two ION nodes (2 and 3) over TCP on 127.0.0.1, from the bench-tcp demo configs, instead of one node.
- The `dry_run` workflow input was removed. Manual runs build and smoke test only.
- `smoke_test.sh` builds ION inside the manylinux image, not in a separate compiler-free runtime container. It takes the interpreter from the wheel's CPython tag.
- `tests/container/` (the no-ION container gate) is not implemented. The missing-library case is covered by the smoke negative check and by `test_missing_ion_raises_importerror`.

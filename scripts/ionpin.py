"""Shared reader/writer for the ION pin file (``ion-pin.env``).

The pin file is a flat ``KEY=value`` file with ``#`` comments and blank
lines. It pins the ION release pyion is built and tested against. Example:

    # The ION release this branch builds against.
    ION_TAG=ion-open-source-4.2.1-a.1
    ION_BUILD_VERSION=4.2.1-a.1
    ION_SHA256=...
    PYION_VERSION=4.2.1a1+ion4.2.1

Lines that are not blank and not comments must contain ``=``; anything
else raises ``ValueError`` with the offending line number. Keys and values
are stripped of surrounding whitespace.

This module is importable both as ``scripts/ionpin.py`` (via ``sys.path``
insertion) and as a plain script:

    python3 scripts/ionpin.py --write-build-info [--out PATH]

``--write-build-info`` reads the pin file next to the repository root (the
parent of this script's directory) and writes ``pyion/_build_info.py``
containing the ``ION_VERSION`` and ``BP_VERSION`` constants that
``pyion/__init__.py`` imports at runtime. ``--out`` overrides the target
path (used by tests). ``BP_VERSION`` comes from the ``PYION_BP_VERSION``
environment variable, defaulting to ``BPv7``.
"""

import argparse
import os
import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parent.parent


def read_pin(path):
    """Parse an ion-pin.env-style file into a dict.

    Raises ValueError (with the line number) on any non-comment,
    non-blank line that does not contain '='.
    """
    pins = {}
    with open(path) as f:
        for lineno, raw in enumerate(f, start=1):
            line = raw.strip()
            if not line or line.startswith('#'):
                continue
            if '=' not in line:
                raise ValueError(
                    '{}:{}: malformed line (expected KEY=value): {!r}'.format(
                        path, lineno, raw.rstrip('\n')))
            key, _, value = line.partition('=')
            key = key.strip()
            if not key:
                raise ValueError(
                    '{}:{}: empty key in line: {!r}'.format(
                        path, lineno, raw.rstrip('\n')))
            pins[key] = value.strip()
    return pins


def build_info_text(ion_version, bp_version):
    """Return the exact contents of a generated ``_build_info.py``."""
    return (
        "# Generated from ion-pin.env; do not edit.\n"
        "ION_VERSION = '{}'\n".format(ion_version)
        + "BP_VERSION = '{}'\n".format(bp_version)
    )


def write_build_info(repo_root, bp_version=None, out_path=None):
    """Write ``pyion/_build_info.py`` from the pin file under ``repo_root``.

    ``bp_version`` defaults to the ``PYION_BP_VERSION`` environment variable,
    falling back to ``'BPv7'``. Returns the path written.
    """
    repo_root = Path(repo_root)
    pin = read_pin(repo_root / 'ion-pin.env')
    if 'ION_BUILD_VERSION' not in pin:
        raise ValueError(
            '{} is missing ION_BUILD_VERSION'.format(repo_root / 'ion-pin.env'))
    if bp_version is None:
        bp_version = os.environ.get('PYION_BP_VERSION', 'BPv7')
    if out_path is None:
        out_path = repo_root / 'pyion' / '_build_info.py'
    else:
        out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(build_info_text(pin['ION_BUILD_VERSION'], bp_version))
    return out_path


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--write-build-info', action='store_true',
                        help='write pyion/_build_info.py from ion-pin.env')
    parser.add_argument('--out', default=None,
                        help='override the output path (default: <repo>/pyion/_build_info.py)')
    args = parser.parse_args(argv)

    if not args.write_build_info:
        parser.error('nothing to do; pass --write-build-info')

    try:
        out = write_build_info(_REPO_ROOT, out_path=args.out)
    except (OSError, ValueError) as exc:
        print('ionpin: error: {}'.format(exc), file=sys.stderr)
        return 1
    print('wrote {}'.format(out))
    return 0


if __name__ == '__main__':
    sys.exit(main())

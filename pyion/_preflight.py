"""Runtime check that the ION shared libraries are loadable and match the build.

Pure Python, no environment variables. Called from ``pyion/__init__.py``
before the C extensions are imported.
"""

import ctypes
import os
import re
import shutil
import subprocess
import warnings

_REQUIRED_LIBS = ('libici.so', 'libbp.so')
_INSTALL_HINT = (
    "Put the ION library directory (for example /usr/local/lib) on the dynamic "
    "linker path, either with /etc/ld.so.conf.d/ plus ldconfig, or with "
    "LD_LIBRARY_PATH. See the 'Install from wheel' section of the pyion README."
)


def _load_libs():
    for name in _REQUIRED_LIBS:
        try:
            ctypes.CDLL(name)
        except OSError as err:
            raise ImportError(
                "pyion cannot load ION library '{}' ({}). Install ION 4.2.1-a.1 "
                "and make its library directory visible to the dynamic linker. {}"
                .format(name, err, _INSTALL_HINT)
            ) from err


def _libbp_dir():
    """Directory of the libbp.so actually loaded into this process, or None."""
    try:
        with open('/proc/self/maps') as maps:
            for line in maps:
                path = line.split()[-1] if line.split() else ''
                if os.path.basename(path).startswith('libbp.so'):
                    return os.path.dirname(os.path.realpath(path))
    except OSError:
        pass
    return None


def _read_version(pc_path):
    with open(pc_path) as f:
        for line in f:
            m = re.match(r'^Version:\s*(\S+)', line)
            if m:
                return m.group(1)
    return None


def _find_ion_pc():
    libdir = _libbp_dir()
    if libdir:
        candidate = os.path.join(libdir, 'pkgconfig', 'ion.pc')
        if os.path.isfile(candidate):
            return candidate
    if shutil.which('pkg-config'):
        try:
            out = subprocess.run(['pkg-config', '--variable=pcfiledir', 'ion'],
                                 capture_output=True, text=True, timeout=5)
            if out.returncode == 0 and out.stdout.strip():
                candidate = os.path.join(out.stdout.strip(), 'ion.pc')
                if os.path.isfile(candidate):
                    return candidate
        except (OSError, subprocess.SubprocessError):
            pass
    return None


def check_ion_installation(build_ion_version):
    """Raise ImportError if ION is missing or mismatched; warn if ion.pc is absent."""
    _load_libs()
    pc = _find_ion_pc()
    if pc is None:
        warnings.warn(
            "Could not find ion.pc next to the loaded ION libraries; skipping the "
            "ION version check (built against ION {}).".format(build_ion_version),
            RuntimeWarning, stacklevel=2)
        return
    runtime_version = _read_version(pc)
    if runtime_version != build_ion_version:
        raise ImportError(
            "ION version mismatch: this pyion wheel was built for ION {} but the "
            "installed ION ({}) reports {}. Install the matching ION version or the "
            "matching pyion wheel. {}".format(
                build_ion_version, pc, runtime_version, _INSTALL_HINT))

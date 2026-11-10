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

# The four ION shared libraries the built extensions link against (verified via
# DT_NEEDED: _bp, _ltp, _mem and _mgmt all need ltp and cfdp in addition to ici/bp).
_REQUIRED_LIBS = ('libici.so', 'libbp.so', 'libltp.so', 'libcfdp.so')
_INSTALL_HINT = (
    "Put the ION library directory (for example /usr/local/lib) on the dynamic "
    "linker path, either with /etc/ld.so.conf.d/ plus ldconfig, or with "
    "LD_LIBRARY_PATH. See the 'Install from wheel' section of the pyion README."
)


def _load_libs(build_ion_version):
    for name in _REQUIRED_LIBS:
        try:
            ctypes.CDLL(name)
        except OSError as err:
            raise ImportError(
                "pyion cannot load ION library '{}' ({}). Install ION {} "
                "and make its library directory visible to the dynamic linker. {}"
                .format(name, err, build_ion_version, _INSTALL_HINT)
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


def _normalize_version(v):
    """Normalize a version string for comparison.

    Strips everything except ASCII alphanumerics and lowercases, so that
    e.g. '4.2.1-a.1' and '4.2.1a1' compare equal. Comparison is deliberately
    loose about punctuation but strict about the alphanumeric content.
    """
    return re.sub(r'[^0-9a-z]', '', v.lower())


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
    """Raise ImportError if ION is missing or mismatched; warn if ion.pc is absent.

    Version matching is normalized (punctuation and case are ignored), so
    '4.2.1-a.1' and '4.2.1a1' compare equal; strict string equality is not
    required.
    """
    _load_libs(build_ion_version)
    pc = _find_ion_pc()
    if pc is None:
        warnings.warn(
            "Could not find ion.pc next to the loaded ION libraries; skipping the "
            "ION version check (built against ION {}).".format(build_ion_version),
            RuntimeWarning, stacklevel=2)
        return
    runtime_version = _read_version(pc)
    if runtime_version is None or _normalize_version(runtime_version) != _normalize_version(build_ion_version):
        raise ImportError(
            "ION version mismatch: this pyion wheel was built for ION {} but the "
            "installed ION ({}) reports {}. Install the matching ION version or the "
            "matching pyion wheel. {}".format(
                build_ion_version, pc, runtime_version, _INSTALL_HINT))

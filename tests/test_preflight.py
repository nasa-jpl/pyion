"""Unit tests for the runtime ION preflight (pyion/_preflight.py) and build info.

Run from the built source tree (after setup.py build_ext --inplace) with ION libraries
on LD_LIBRARY_PATH.
"""

import os
import subprocess
import sys
import textwrap
import warnings

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_REPO_ROOT, 'scripts'))

from ionpin import read_pin  # noqa: E402

_PIN_FILE = os.path.join(_REPO_ROOT, 'ion-pin.env')


def _pin(key):
    return read_pin(_PIN_FILE)[key]

import pytest

from pyion import _preflight


def test_import_without_env_vars():
    env = {k: v for k, v in os.environ.items() if k not in ('PYION_BP_VERSION', 'ION_HOME')}
    out = subprocess.run([sys.executable, '-c', 'import pyion; print(pyion.BP_VERSION)'],
                         env=env, capture_output=True, text=True)
    assert out.returncode == 0, out.stderr
    assert out.stdout.strip() == 'BPv7'


def test_build_info_attributes():
    import pyion
    from pyion import _build_info
    assert pyion.ION_VERSION == _build_info.ION_VERSION == _pin('ION_BUILD_VERSION')
    assert pyion.BP_VERSION == _build_info.BP_VERSION == 'BPv7'


def test_missing_ion_raises_importerror():
    env = {k: v for k, v in os.environ.items() if k != 'LD_LIBRARY_PATH'}
    out = subprocess.run([sys.executable, '-c', 'import pyion'],
                         env=env, capture_output=True, text=True)
    assert out.returncode != 0
    assert 'cannot load ION library' in out.stderr
    assert 'ldconfig' in out.stderr or '/etc/ld.so.conf.d' in out.stderr
    assert 'LD_LIBRARY_PATH' in out.stderr


def test_required_libs_cover_all_linked_ion_libraries():
    # DT_NEEDED of the built extensions requires all four.
    assert _preflight._REQUIRED_LIBS == ('libici.so', 'libbp.so', 'libltp.so', 'libcfdp.so')


def test_version_mismatch_raises(tmp_path, monkeypatch):
    pc_dir = tmp_path / 'pkgconfig'
    pc_dir.mkdir()
    (pc_dir / 'ion.pc').write_text('Name: ion\nVersion: 9.9.9\n')
    monkeypatch.setattr(_preflight, '_load_libs', lambda *args: None)
    monkeypatch.setattr(_preflight, '_find_ion_pc', lambda: str(pc_dir / 'ion.pc'))
    with pytest.raises(ImportError, match='ION version mismatch'):
        _preflight.check_ion_installation('4.2.1-a.1')


def test_version_match_passes(tmp_path, monkeypatch):
    pc = tmp_path / 'ion.pc'
    pc.write_text('Name: ion\nVersion: 4.2.1-a.1\n')
    monkeypatch.setattr(_preflight, '_load_libs', lambda *args: None)
    monkeypatch.setattr(_preflight, '_find_ion_pc', lambda: str(pc))
    _preflight.check_ion_installation('4.2.1-a.1')


def test_version_match_normalizes_punctuation(tmp_path, monkeypatch):
    pc = tmp_path / 'ion.pc'
    pc.write_text('Name: ion\nVersion: 4.2.1a1\n')
    monkeypatch.setattr(_preflight, '_load_libs', lambda *args: None)
    monkeypatch.setattr(_preflight, '_find_ion_pc', lambda: str(pc))
    # 4.2.1a1 and 4.2.1-a.1 differ only in punctuation and must compare equal.
    _preflight.check_ion_installation('4.2.1-a.1')


def test_load_libs_error_message_names_build_version(monkeypatch):
    def fake_cdll(name):
        raise OSError('cannot open shared object file')
    monkeypatch.setattr(_preflight.ctypes, 'CDLL', fake_cdll)
    with pytest.raises(ImportError) as excinfo:
        _preflight._load_libs('9.9.9')
    msg = str(excinfo.value)
    assert 'cannot load ION library' in msg
    assert '9.9.9' in msg
    assert '4.2.1-a.1' not in msg


def test_missing_ion_pc_warns(monkeypatch):
    monkeypatch.setattr(_preflight, '_load_libs', lambda *args: None)
    monkeypatch.setattr(_preflight, '_find_ion_pc', lambda: None)
    with pytest.warns(RuntimeWarning, match='skipping the ION version check'):
        _preflight.check_ion_installation('4.2.1-a.1')

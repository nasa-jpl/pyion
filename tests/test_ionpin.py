"""Unit tests for scripts/ionpin.py (the shared ion-pin.env reader/writer)."""

import os
import shutil
import subprocess
import sys

import pytest

REPO = os.path.join(os.path.dirname(__file__), '..')
sys.path.insert(0, os.path.join(REPO, 'scripts'))

import ionpin  # noqa: E402


def test_read_pin_parses_real_pin_file():
    pins = ionpin.read_pin(os.path.join(REPO, 'ion-pin.env'))
    assert pins['ION_BUILD_VERSION']
    assert pins['ION_TAG'].startswith('ion-open-source-')
    assert pins['ION_SHA256']
    assert pins['PYION_VERSION']


def test_read_pin_tolerates_comments_blanks_and_spaces(tmp_path):
    pin = tmp_path / 'pin.env'
    pin.write_text(
        '# comment\n'
        '\n'
        '   \n'
        'KEY = value with spaces  \n'
        'OTHER=thing\n'
        '#KEY=ignored\n'
    )
    assert ionpin.read_pin(pin) == {'KEY': 'value with spaces', 'OTHER': 'thing'}


def test_read_pin_rejects_malformed_line(tmp_path):
    pin = tmp_path / 'pin.env'
    pin.write_text('ION_BUILD_VERSION=4.2.1-a.1\nnot a key value line\n')
    with pytest.raises(ValueError) as excinfo:
        ionpin.read_pin(pin)
    assert ':2:' in str(excinfo.value)


def test_read_pin_rejects_empty_key(tmp_path):
    pin = tmp_path / 'pin.env'
    pin.write_text('=value\n')
    with pytest.raises(ValueError):
        ionpin.read_pin(pin)


def test_build_info_text_round_trips():
    text = ionpin.build_info_text('4.2.1-a.1', 'BPv7')
    namespace = {}
    exec(text, namespace)
    assert namespace['ION_VERSION'] == '4.2.1-a.1'
    assert namespace['BP_VERSION'] == 'BPv7'


def test_write_build_info_defaults_to_bpv7(tmp_path):
    (tmp_path / 'ion-pin.env').write_text('ION_BUILD_VERSION=4.2.1-a.1\n')
    out = ionpin.write_build_info(tmp_path)
    assert out == tmp_path / 'pyion' / '_build_info.py'
    namespace = {}
    exec(out.read_text(), namespace)
    assert namespace['ION_VERSION'] == '4.2.1-a.1'
    assert namespace['BP_VERSION'] == 'BPv7'


def test_write_build_info_honors_env_override(tmp_path, monkeypatch):
    (tmp_path / 'ion-pin.env').write_text('ION_BUILD_VERSION=4.2.1-a.1\n')
    monkeypatch.setenv('PYION_BP_VERSION', 'BPv6')
    out = ionpin.write_build_info(tmp_path)
    namespace = {}
    exec(out.read_text(), namespace)
    assert namespace['BP_VERSION'] == 'BPv6'


def test_write_build_info_missing_key_raises(tmp_path):
    (tmp_path / 'ion-pin.env').write_text('ION_TAG=ion-open-source-4.2.1-a.1\n')
    with pytest.raises(ValueError):
        ionpin.write_build_info(tmp_path)


def test_cli_write_build_info(tmp_path):
    out = tmp_path / '_build_info.py'
    env = dict(os.environ, PYION_BP_VERSION='BPv7')
    result = subprocess.run(
        [sys.executable, os.path.join(REPO, 'scripts', 'ionpin.py'),
         '--write-build-info', '--out', str(out)],
        capture_output=True, text=True, env=env)
    assert result.returncode == 0, result.stderr
    expected_ion = ionpin.read_pin(os.path.join(REPO, 'ion-pin.env'))['ION_BUILD_VERSION']
    namespace = {}
    exec(out.read_text(), namespace)
    assert namespace['ION_VERSION'] == expected_ion
    assert namespace['BP_VERSION'] == 'BPv7'


def test_cli_fails_cleanly_on_malformed_pin(tmp_path):
    bad = tmp_path / 'ion-pin.env'
    bad.write_text('nonsense line\n')
    out = tmp_path / '_build_info.py'
    # Point the CLI at a repo root whose pin file is malformed by copying the
    # script layout: scripts/ionpin.py + ion-pin.env under tmp_path.
    scripts_dir = tmp_path / 'scripts'
    scripts_dir.mkdir()
    shutil.copy(os.path.join(REPO, 'scripts', 'ionpin.py'), scripts_dir / 'ionpin.py')
    result = subprocess.run(
        [sys.executable, str(scripts_dir / 'ionpin.py'),
         '--write-build-info', '--out', str(out)],
        capture_output=True, text=True)
    assert result.returncode != 0
    assert result.stderr.strip() != ''
    assert not out.exists()


def test_sdist_contains_ionpin(tmp_path):
    pytest.importorskip('setuptools')
    # Copy the minimal tree needed for an sdist into tmp_path so the real
    # working tree is untouched.
    work = tmp_path / 'sdist-src'
    work.mkdir()
    for name in ('setup.py', 'pyproject.toml', 'MANIFEST.in', 'README.md', 'ion-pin.env'):
        src = os.path.join(REPO, name)
        if os.path.exists(src):
            shutil.copy2(src, work / name)
    shutil.copytree(os.path.join(REPO, 'pyion'), work / 'pyion',
                    ignore=shutil.ignore_patterns('_build_info.py', '__pycache__', '*.so'))
    scripts_dst = work / 'scripts'
    scripts_dst.mkdir()
    shutil.copy2(os.path.join(REPO, 'scripts', 'ionpin.py'), scripts_dst / 'ionpin.py')

    env = dict(os.environ)
    env.setdefault('PYION_BP_VERSION', 'BPv7')
    # setup.py requires ION_HOME to point at an ION source tree; the sdist
    # command does not compile anything, but setup.py validates the variable
    # at import time, so point it at anything that exists.
    env.setdefault('ION_HOME', '/usr/local')
    dist = tmp_path / 'dist'
    result = subprocess.run(
        [sys.executable, 'setup.py', 'sdist', '--dist-dir', str(dist)],
        cwd=str(work), capture_output=True, text=True, env=env)
    assert result.returncode == 0, result.stderr
    tarballs = list(dist.glob('*.tar.gz'))
    assert len(tarballs) == 1
    listing = subprocess.run(['tar', '-tzf', str(tarballs[0])],
                             capture_output=True, text=True).stdout
    assert any(name.endswith('scripts/ionpin.py') for name in listing.splitlines())
    # setup.py reads ion-pin.env at import time, so the sdist is unusable without it.
    assert any(name.endswith('ion-pin.env') for name in listing.splitlines())
    # The C sources are included, so the headers they #include must be too;
    # without them the sdist cannot compile.
    names = listing.splitlines()
    for header in ('base_bp.h', 'base_cfdp.h', 'base_ltp.h', 'base_mem.h',
                   'macros.h', 'return_codes.h'):
        assert any(n.endswith('pyion/' + header) for n in names), header

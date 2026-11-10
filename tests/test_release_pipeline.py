"""Structure and behaviour tests for the wheel build, release check, and docs.

Not run in the ION container; they check files, scripts with fake engines and fixtures,
and the workflow YAML.
"""

import os
import shutil
import subprocess
import sys

import pytest
import yaml

REPO = os.path.join(os.path.dirname(__file__), '..')
WORKFLOW = os.path.join(REPO, '.github', 'workflows', 'build-wheels.yml')
SCRIPTS = os.path.join(REPO, 'scripts')


def _load_workflow():
    with open(WORKFLOW) as f:
        return yaml.safe_load(f)


def _pins():
    pins = {}
    with open(os.path.join(REPO, 'ion-pin.env')) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith('#'):
                key, _, value = line.partition('=')
                pins[key] = value
    return pins


def _dockerfile_arg(path, name):
    with open(os.path.join(REPO, 'docker', path)) as f:
        for line in f:
            if line.startswith('ARG {}='.format(name)):
                return line.strip().partition('=')[2]
    return None


def test_dockerfiles_match_ion_pin():
    pins = _pins()
    for path in ('Dockerfile', 'Dockerfile.freethreaded'):
        assert _dockerfile_arg(path, 'ION_TAG') == pins['ION_TAG'], path
        assert _dockerfile_arg(path, 'ION_SHA256') == pins['ION_SHA256'], path
    assert _dockerfile_arg('Dockerfile.functional', 'ION_REF') == pins['ION_TAG']


def test_scripts_and_workflow_take_ion_tag_from_pin():
    with open(os.path.join(SCRIPTS, 'build_wheels.sh')) as f:
        build = f.read()
    with open(os.path.join(SCRIPTS, 'smoke_test.sh')) as f:
        smoke = f.read()
    with open(WORKFLOW) as f:
        workflow = f.read()
    for text in (build, smoke, workflow):
        assert 'ion-pin.env' in text or 'GITHUB_ENV' in text
        assert 'ion-open-source-4' not in text


def test_release_job_gated():
    wf = _load_workflow()
    jobs = wf['jobs']
    assert jobs['release']['needs'] == ['pin', 'smoke']
    assert jobs['release']['if'] == "github.event_name == 'push' && github.ref == 'refs/heads/master'"
    assert jobs['release']['permissions'] == {'contents': 'write'}
    assert wf['permissions'] == {'contents': 'read'}
    for name in ('pin', 'build', 'smoke'):
        assert 'permissions' not in jobs[name], name


def test_prerelease_job_gated():
    wf = _load_workflow()
    job = wf['jobs']['prerelease']
    assert job['needs'] == ['pin', 'smoke']
    assert job['permissions'] == {'contents': 'write'}
    assert 'workflow_dispatch' in job['if']
    assert 'head.repo.full_name == github.repository' in job['if']


def test_workflow_triggers():
    wf = _load_workflow()
    # PyYAML parses the bare key `on` as boolean True.
    triggers = wf.get('on', wf.get(True))
    assert triggers['push']['branches'] == ['master']
    assert triggers['pull_request']['branches'] == ['master']
    assert 'tags' not in triggers['push']
    assert 'workflow_dispatch' in triggers


ACTIONLINT = shutil.which('actionlint') or os.path.join(os.path.dirname(sys.executable), 'actionlint')


@pytest.mark.skipif(not os.path.exists(ACTIONLINT), reason='actionlint not installed')
def test_workflow_lint():
    out = subprocess.run([ACTIONLINT, WORKFLOW], capture_output=True, text=True)
    assert out.returncode == 0, out.stdout + out.stderr


def test_release_check_expected_assets():
    ok = subprocess.run(['bash', os.path.join(SCRIPTS, 'check_release.sh'), 'pyion-v4.2.1-a.1',
                         os.path.join(REPO, 'tests', 'release', 'assets_complete.json')],
                        capture_output=True, text=True)
    assert ok.returncode == 0, ok.stderr
    bad = subprocess.run(['bash', os.path.join(SCRIPTS, 'check_release.sh'), 'pyion-v4.2.1-a.1',
                          os.path.join(REPO, 'tests', 'release', 'assets_missing_sums.json')],
                         capture_output=True, text=True)
    assert bad.returncode != 0
    assert 'SHA256SUMS' in bad.stderr


def _fake_engine(tmp_path, pull_ok):
    engine = tmp_path / 'fake-engine'
    if pull_ok:
        engine.write_text('#!/bin/sh\nexit 0\n')
    else:
        engine.write_text('#!/bin/sh\nexit 1\n')
    engine.chmod(0o755)
    return str(engine)


def test_build_script_pins(tmp_path):
    env = dict(os.environ, CONTAINER_ENGINE=_fake_engine(tmp_path, True),
               MANYLINUX_IMAGE='quay.io/pypa/manylinux_2_28_x86_64:latest')
    out = subprocess.run(['bash', os.path.join(SCRIPTS, 'build_wheels.sh'),
                          _pins()['ION_TAG'], str(tmp_path / 'd')],
                         capture_output=True, text=True, env=env)
    assert out.returncode != 0
    assert '@sha256:' in out.stderr


def test_build_script_pull_failure(tmp_path):
    env = dict(os.environ, CONTAINER_ENGINE=_fake_engine(tmp_path, False))
    out = subprocess.run(['bash', os.path.join(SCRIPTS, 'build_wheels.sh'), 'ion-open-source-4.2.1-a.1', str(tmp_path / 'd')],
                         capture_output=True, text=True, env=env)
    assert out.returncode != 0
    assert 'could not pull build image' in out.stderr
    assert 'manylinux_2_28_x86_64' in out.stderr


def test_source_build_env_var_path():
    # A source-built package still imports and reports the BP version when PYION_BP_VERSION is set.
    env = dict(os.environ, PYION_BP_VERSION='BPv7')
    out = subprocess.run([sys.executable, '-c', 'import pyion; print(pyion.BP_VERSION)'],
                         capture_output=True, text=True, env=env, cwd=REPO)
    assert out.returncode == 0, out.stderr
    assert out.stdout.strip() == 'BPv7'

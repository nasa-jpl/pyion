import os
import subprocess

import pytest

REPO = os.path.join(os.path.dirname(__file__), '..')


def test_branch_readme_install_section_and_master_pointer():
    with open(os.path.join(REPO, 'README.md')) as f:
        assert 'Install from wheel' in f.read()
    master = subprocess.run(['git', '-C', REPO, 'show', 'master:README.md'],
                            capture_output=True, text=True)
    if master.returncode != 0:
        pytest.skip('master branch not available')
    assert 'GitHub Releases' in master.stdout


def test_release_doc_sections():
    with open(os.path.join(REPO, 'docs', 'release.md')) as f:
        text = f.read()
    assert '## Workflow-driven release' in text
    assert '## Manual fallback' in text
    assert '## Manual runs' in text

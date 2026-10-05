"""Every config file in the repo must be valid and read correctly by the firmware.

Files checked: configs/**/*.json (real device configs, one per device), a root
smart_rtu_config*.json, and tests/fixtures/configs/*.json (known-good test configs).
"""

import json

import pytest
from jsonschema import Draft202012Validator

from conftest import REPO
from config_checks import SCHEMA_PATH, check_file

pytestmark = pytest.mark.validate

CONFIG_FILES = sorted(
    {*REPO.glob("configs/**/*.json"), *REPO.glob("smart_rtu_config*.json"),
     *REPO.glob("tests/fixtures/configs/*.json")})


def test_schema_is_valid_json_schema():
    Draft202012Validator.check_schema(json.loads(SCHEMA_PATH.read_text()))


def test_known_good_fixture_exists():
    assert REPO / "tests/fixtures/configs/full_config.json" in CONFIG_FILES


@pytest.mark.parametrize("path", CONFIG_FILES, ids=lambda p: str(p.relative_to(REPO)))
def test_config_file_valid(path):
    problems = check_file(path)
    assert not problems, f"{path.relative_to(REPO)} has {len(problems)} problem(s):\n  " + "\n  ".join(problems)

#!/usr/bin/env python3
"""Check one or more smart_rtu_config.json files, like the validate-configs CI job.

    python3 tests/validate/validate_config.py configs/plant_a.json [more.json ...]

Exit code 0 when every file is valid, 1 otherwise. Needs jsonschema and a C
compiler (the real firmware parser is compiled and run on each file).
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from config_checks import check_file  # noqa: E402


def main(paths):
    if not paths:
        sys.exit(__doc__)
    bad = 0
    for path in paths:
        problems = check_file(path)
        if problems:
            bad += 1
            print(f"{path} has {len(problems)} problem(s):")
            for p in problems:
                print(f"  {p}")
        else:
            print(f"{path}: OK")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

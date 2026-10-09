"""Shared setup for the validate tests: import paths and the config file lists."""

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(Path(__file__).parent), str(REPO / "tools" / "smart_rtu_tool")]

"""Runs last: the gateway never crashed during the session."""

import re

import pytest

pytestmark = pytest.mark.hardware

CRASH = re.compile(r"Segmentation fault|\(core dumped\)|double free or corruption|stack smashing detected|"
                   r"Main process exited, code=(killed|dumped)")


def test_no_automatic_restart(board):
    """systemd restarts the service only after a crash. NRestarts counts those automatic
    restarts since the last manual (re)start by the tests; the journal scan below covers
    the whole session."""
    assert int(board.prop("NRestarts") or 0) == 0


def test_no_crash_in_journal(board):
    """This session only, after the installer stopped the previously installed build
    (whose own shutdown is not what this pipeline tests)."""
    text = board.since(board.session_mark)
    installed = text.find("[MAIN] gateway ")      # first start of the build under test
    hits = [l for l in text[max(installed, 0):].splitlines() if CRASH.search(l)]
    assert not hits, "\n".join(hits[-10:])

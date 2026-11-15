"""Lancia le prove PowerShell di installa.ps1 (solo su Windows)."""

import shutil
import subprocess
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).parent

pytestmark = pytest.mark.skipif(
    sys.platform != "win32" or shutil.which("powershell.exe") is None,
    reason="installa.ps1 gira solo su Windows",
)


def test_installa_ps1_functions():
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
         "-File", str(HERE / "installa_tests.ps1")],
        capture_output=True, text=True, timeout=120,
    )
    output = result.stdout + result.stderr
    assert result.returncode == 0, output
    assert "PASS" in output and "FAIL" not in output, output


def test_installa_ps1_has_utf8_bom():
    # PowerShell 5.1 legge i .ps1 senza BOM come ANSI: le lettere accentate si rovinano.
    assert (HERE.parent / "installa.ps1").read_bytes().startswith(b"\xef\xbb\xbf")


def test_installa_bat_is_ascii():
    (HERE.parent / "installa.bat").read_bytes().decode("ascii")

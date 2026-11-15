"""Avvio automatico: prove PowerShell di avvio-automatico.ps1 (solo su Windows) e
coerenza con launcher.py."""

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

import launcher

HERE = Path(__file__).parent
ROOT = HERE.parent

windows_only = pytest.mark.skipif(
    sys.platform != "win32" or shutil.which("powershell.exe") is None,
    reason="avvio-automatico.ps1 gira solo su Windows",
)


@windows_only
def test_avvio_automatico_ps1_functions():
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
         "-File", str(HERE / "avvio_automatico_tests.ps1")],
        capture_output=True, text=True, errors="replace", timeout=180,
        env={**os.environ, "XEG_TEST_PYTHON": sys.executable},
    )  # fmt: skip
    output = result.stdout + result.stderr
    assert result.returncode == 0, output
    assert "PASS" in output and "FAIL" not in output, output


def test_ps1_has_utf8_bom():
    # PowerShell 5.1 legge i .ps1 senza BOM come ANSI: le lettere accentate si rovinano.
    assert (ROOT / "avvio-automatico.ps1").read_bytes().startswith(b"\xef\xbb\xbf")


@pytest.mark.parametrize("bat, action", [
    ("avvio-automatico-attiva.bat", "Attiva"),
    ("avvio-automatico-disattiva.bat", "Disattiva"),
    ("avvio-automatico-stato.bat", "Stato"),
    ("avvia.bat", "Avvia"),
    ("ferma.bat", "Ferma"),
])  # fmt: skip
def test_bat_files(bat, action):
    text = (ROOT / bat).read_bytes().decode("ascii")
    assert f'avvio-automatico.ps1" -{action} %*' in text
    assert "\r\n" in text and "\n" not in text.replace("\r\n", "")


def test_avvia_bat_closes_on_success_and_waits_on_error():
    text = (ROOT / "avvia.bat").read_bytes().decode("ascii")
    assert "if errorlevel 1" in text and "pause" in text and "timeout /t" in text


def test_windowed_launcher_bat():
    text = (ROOT / "avvia-con-finestra.bat").read_bytes().decode("ascii")
    assert "launcher.py %*" in text and "--auto" not in text


def test_stop_event_name_matches_launcher():
    ps1 = (ROOT / "avvio-automatico.ps1").read_text(encoding="utf-8-sig")
    name = re.search(r"\$StopEventName = '([^']+)'", ps1).group(1)
    assert name == launcher.STOP_EVENT


def test_installa_uses_avvio_automatico():
    # Un solo posto per creare l'avvio automatico: installa.ps1 chiama avvio-automatico.ps1.
    ps1 = (ROOT / "installa.ps1").read_text(encoding="utf-8-sig")
    assert "avvio-automatico.ps1" in ps1
    assert "Register-ScheduledTask" not in ps1

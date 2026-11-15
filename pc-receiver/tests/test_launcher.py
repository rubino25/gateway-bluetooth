"""Ricerca di usbip.exe / viiper.exe e PATH passato a VIIPER (senza avviare nulla)."""

import os

import launcher
from config import Config


def make_exe(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"")
    return path


def isolate(monkeypatch, tmp_path):
    """Ambiente finto: niente PATH reale, cartelle di sistema dentro tmp_path."""
    monkeypatch.setattr(launcher.shutil, "which", lambda name: None)
    for name in ("ProgramFiles", "ProgramW6432", "ProgramFiles(x86)"):
        monkeypatch.delenv(name, raising=False)
    monkeypatch.setenv("ProgramFiles", str(tmp_path / "pf"))
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path / "local"))
    monkeypatch.setenv("USERPROFILE", str(tmp_path / "home"))


def test_usbip_found_in_program_files(monkeypatch, tmp_path):
    isolate(monkeypatch, tmp_path)
    exe = make_exe(tmp_path / "pf" / "USBip" / "usbip.exe")
    assert launcher.find_usbip(Config()) == exe


def test_usbip_config_path_wins(monkeypatch, tmp_path):
    isolate(monkeypatch, tmp_path)
    make_exe(tmp_path / "pf" / "USBip" / "usbip.exe")
    custom = make_exe(tmp_path / "altrove" / "usbip.exe")
    assert launcher.find_usbip(Config(usbip_path=str(custom))) == custom


def test_usbip_missing(monkeypatch, tmp_path):
    isolate(monkeypatch, tmp_path)
    assert launcher.find_usbip(Config()) is None
    # Un percorso in config che non esiste non conta.
    assert launcher.find_usbip(Config(usbip_path=str(tmp_path / "no.exe"))) is None


def test_viiper_prefers_installed_over_downloads(monkeypatch, tmp_path):
    isolate(monkeypatch, tmp_path)
    make_exe(tmp_path / "home" / "Downloads" / "viiper-windows-amd64.exe")
    installed = make_exe(tmp_path / "local" / "VIIPER" / "viiper.exe")
    assert launcher.find_viiper(Config()) == installed


def test_viiper_found_in_downloads(monkeypatch, tmp_path):
    isolate(monkeypatch, tmp_path)
    exe = make_exe(tmp_path / "home" / "Downloads" / "viiper-windows-amd64.exe")
    assert launcher.find_viiper(Config()) == exe


def test_viiper_missing(monkeypatch, tmp_path):
    isolate(monkeypatch, tmp_path)
    assert launcher.find_viiper(Config()) is None


def test_env_with_dir_prepends_once(tmp_path):
    usbip_dir = tmp_path / "USBip"
    env = launcher.env_with_dir(usbip_dir, {"Path": r"C:\Windows;C:\Windows\System32"})
    assert env["Path"].split(os.pathsep) == [str(usbip_dir), r"C:\Windows", r"C:\Windows\System32"]
    again = launcher.env_with_dir(usbip_dir, env)
    assert again["Path"] == env["Path"]  # già presente: non duplica
    assert "PATH" not in env  # mantiene il nome della variabile esistente


def test_env_with_dir_without_path(tmp_path):
    env = launcher.env_with_dir(tmp_path, {})
    assert env["PATH"] == str(tmp_path)


def test_running_viiper_pids_parses_tasklist(monkeypatch):
    sample = (
        '"System Idle Process","0","Services","0","8 K"\n'
        '"viiper.exe","12828","Console","1","40.000 K"\n'
        '"viiper-windows-amd64.exe","4242","Console","1","38.000 K"\n'
        '"python.exe","999","Console","1","10.000 K"\n'
    )

    class Result:
        stdout = sample

    monkeypatch.setattr(launcher.subprocess, "run", lambda *a, **k: Result())
    assert launcher.running_viiper_pids() == [12828, 4242]

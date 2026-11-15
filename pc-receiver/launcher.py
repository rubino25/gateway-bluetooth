"""Avvio unico: VIIPER server (con usbip-win2 nel PATH) + ricevitore.

Uso con finestra: `avvia-con-finestra.bat`, oppure `python launcher.py`. Chiudendo la
finestra o premendo Ctrl+C si ferma tutto: ricevitore, pad virtuale e il VIIPER
avviato da qui.

Cosa fa:
1. trova `usbip.exe` (installato da usbip-win2, di solito in `C:\\Program Files\\USBip`,
   cartella che l'installer non aggiunge al PATH);
2. trova `viiper.exe`;
3. chiude un VIIPER già in esecuzione (per esempio quello dell'avvio automatico, che
   non vede `usbip.exe`) e lo riavvia con la cartella di usbip-win2 nel PATH;
4. avvia il ricevitore, e riavvia VIIPER se si chiude da solo.

In background (`pythonw launcher.py --auto`: lo avviano `avvia.bat` e l'avvio
all'accesso a Windows, tramite avvio-automatico.ps1): nessuna finestra, messaggi in
`avvio-automatico.log`, dopo un errore riparte da solo. Si ferma in modo pulito con
`ferma.bat` (avvio-automatico.ps1 -Ferma o -Disattiva), che imposta l'evento STOP_EVENT.

Un solo launcher per volta (mutex INSTANCE_MUTEX): se ne gira già uno, in background o
con la finestra, il secondo lo segnala ed esce.
"""

from __future__ import annotations

import argparse
import csv
import functools
import io
import logging
import logging.handlers
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Callable

import receiver
from config import Config, load_config
from viiper_client import ViiperClient, ViiperError

HERE = Path(__file__).resolve().parent
VIIPER_LOG = HERE / "viiper.log"
AUTO_LOG = HERE / "avvio-automatico.log"
# Nomi condivisi con avvio-automatico.ps1: vanno cambiati insieme.
INSTANCE_MUTEX = "Local\\xbox-eth-gateway-launcher"
STOP_EVENT = "Local\\xbox-eth-gateway-stop"

VIIPER_RESTART_S = 5.0  # intervallo minimo fra due riavvii di VIIPER
AUTO_BACKOFF_S = (5.0, 60.0)  # attesa prima di ripartire dopo un errore: da 5 s, raddoppia fino a 60
AUTO_STABLE_S = 120.0  # dopo tanto tempo senza errori l'attesa torna a 5 s

log = logging.getLogger("launcher")
_auto = False  # True in modalità automatica: messaggi nel log invece che sulla console


def _say(text: str) -> None:
    if _auto:
        log.info(text)
    else:
        print(text)


def _fail(message: str) -> int:
    if _auto:
        log.error(message.replace("\n", " "))
    else:
        print()
        print("ERRORE: " + message)
    return 1


def _env_dir(name: str) -> Path | None:
    value = os.environ.get(name)
    return Path(value) if value else None


def find_usbip(cfg: Config) -> Path | None:
    """Percorso di usbip.exe: da config, dal PATH o dalla cartella di installazione."""
    candidates: list[Path] = []
    if cfg.usbip_path:
        candidates.append(Path(cfg.usbip_path))
    found = shutil.which("usbip")
    if found:
        candidates.append(Path(found))
    for base in ("ProgramFiles", "ProgramW6432", "ProgramFiles(x86)"):
        root = _env_dir(base)
        if root:
            candidates.append(root / "USBip" / "usbip.exe")
    return next((p for p in candidates if p.is_file()), None)


def find_viiper(cfg: Config) -> Path | None:
    """Percorso di viiper.exe: da config, dal PATH, dall'installazione o dai Download."""
    candidates: list[Path] = []
    if cfg.viiper_path:
        candidates.append(Path(cfg.viiper_path))
    found = shutil.which("viiper")
    if found:
        candidates.append(Path(found))
    local = _env_dir("LOCALAPPDATA")
    if local:
        candidates.append(local / "VIIPER" / "viiper.exe")
    home = _env_dir("USERPROFILE")
    if home:
        candidates.extend(sorted((home / "Downloads").glob("viiper-windows-*.exe"), reverse=True))
    return next((p for p in candidates if p.is_file()), None)


def env_with_dir(directory: Path, base: dict[str, str] | None = None) -> dict[str, str]:
    """Copia dell'ambiente con `directory` in testa al PATH."""
    env = dict(os.environ if base is None else base)
    key = next((k for k in env if k.upper() == "PATH"), "PATH")
    parts = [p for p in env.get(key, "").split(os.pathsep) if p]
    if not any(os.path.normcase(p.rstrip("\\/")) == os.path.normcase(str(directory)) for p in parts):
        parts.insert(0, str(directory))
    env[key] = os.pathsep.join(parts)
    return env


def running_viiper_pids() -> list[int]:
    """PID dei processi il cui eseguibile si chiama viiper*.exe."""
    try:
        out = subprocess.run(
            ["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True, check=True,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        ).stdout  # fmt: skip
    except (OSError, subprocess.CalledProcessError):
        return []
    pids = []
    for row in csv.reader(io.StringIO(out)):
        if len(row) >= 2 and row[0].lower().startswith("viiper") and row[0].lower().endswith(".exe"):
            try:
                pids.append(int(row[1]))
            except ValueError:
                pass
    return pids


def stop_pids(pids: list[int]) -> None:
    for pid in pids:
        subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"], capture_output=True,
                       creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))  # fmt: skip


def wait_for_viiper(client: ViiperClient, seconds: float, proc: subprocess.Popen | None = None) -> dict | None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if proc is not None and proc.poll() is not None:
            return None  # VIIPER è uscito subito
        try:
            return client.ping()
        except ViiperError:
            time.sleep(0.3)
    return None


def wait_until_down(client: ViiperClient, seconds: float) -> bool:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            client.ping()
        except ViiperError:
            return True
        time.sleep(0.2)
    return False


# ---------------------------------------------------------------- oggetti di Windows


@functools.lru_cache(maxsize=None)
def _kernel32():
    import ctypes
    from ctypes import wintypes

    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.CreateMutexW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR]
    k.CreateMutexW.restype = wintypes.HANDLE
    k.CreateEventW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR]
    k.CreateEventW.restype = wintypes.HANDLE
    k.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    k.WaitForSingleObject.restype = wintypes.DWORD
    for name in ("ReleaseMutex", "ResetEvent", "SetEvent", "CloseHandle"):
        getattr(k, name).argtypes = [wintypes.HANDLE]
        getattr(k, name).restype = wintypes.BOOL
    return k


_WAIT_OBJECT_0 = 0
_WAIT_ABANDONED = 0x80


class SingleInstance:
    """Mutex con nome: lo possiede un solo launcher per volta nella sessione di Windows.

    Conta chi lo possiede, non chi lo ha aperto: così un controllo dello stato da
    PowerShell (che apre il mutex per un attimo) non blocca l'avvio.
    """

    def __init__(self, name: str = INSTANCE_MUTEX):
        self.name = name
        self._handle = None

    def acquire(self) -> bool:
        if sys.platform != "win32":
            return True
        k = _kernel32()
        handle = k.CreateMutexW(None, False, self.name)
        if not handle:
            return True  # non si riesce a controllare: meglio partire che restare fermi
        if k.WaitForSingleObject(handle, 0) in (_WAIT_OBJECT_0, _WAIT_ABANDONED):
            self._handle = handle
            return True
        k.CloseHandle(handle)
        return False

    def release(self) -> None:
        if self._handle:
            k = _kernel32()
            k.ReleaseMutex(self._handle)
            k.CloseHandle(self._handle)
            self._handle = None


class StopSignal:
    """Evento con nome che avvio-automatico.ps1 imposta per chiedere un arresto pulito."""

    def __init__(self, name: str = STOP_EVENT):
        self._handle = None
        if sys.platform == "win32":
            k = _kernel32()
            handle = k.CreateEventW(None, True, False, name)  # a reset manuale
            if handle:
                k.ResetEvent(handle)  # una richiesta rimasta da prima non vale per noi
                self._handle = handle

    def is_set(self) -> bool:
        return bool(self._handle) and _kernel32().WaitForSingleObject(self._handle, 0) == _WAIT_OBJECT_0

    def wait(self, seconds: float) -> bool:
        """Attende al più `seconds`; True se nel frattempo è arrivata la richiesta di arresto."""
        if not self._handle:
            time.sleep(seconds)
            return False
        ms = max(0, int(seconds * 1000))
        return _kernel32().WaitForSingleObject(self._handle, ms) == _WAIT_OBJECT_0

    def close(self) -> None:
        if self._handle:
            _kernel32().CloseHandle(self._handle)
            self._handle = None


# ---------------------------------------------------------------- VIIPER e sorveglianza


class ViiperProcess:
    """Il `viiper server` avviato da qui, con usbip-win2 nel PATH e l'output in un file."""

    def __init__(self, exe: Path, usbip_dir: Path, log_path: Path = VIIPER_LOG, hidden: bool = False):
        self.exe = exe
        self.usbip_dir = usbip_dir
        self.log_path = log_path
        self.hidden = hidden
        self.proc: subprocess.Popen | None = None
        self._log = None

    def start(self, append: bool = False) -> None:
        self._close_log()
        self._log = self.log_path.open("a" if append else "w", encoding="utf-8", errors="replace")
        # Gruppo a parte: Ctrl+C ferma prima il ricevitore, che toglie il pad virtuale,
        # e solo dopo VIIPER viene chiuso da qui. Senza console (modalità automatica)
        # VIIPER non deve aprire una finestra sua.
        flags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
        if self.hidden:
            flags |= getattr(subprocess, "CREATE_NO_WINDOW", 0)
        try:
            self.proc = subprocess.Popen(
                [str(self.exe), "server"],
                env=env_with_dir(self.usbip_dir),
                stdout=self._log,
                stderr=subprocess.STDOUT,
                stdin=subprocess.DEVNULL,
                cwd=str(self.exe.parent),
                creationflags=flags,
            )
        except OSError:
            self._close_log()
            raise

    def alive(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def stop(self) -> None:
        if self.alive():
            stop_pids([self.proc.pid])
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
        self._close_log()

    def _close_log(self) -> None:
        if self._log is not None:
            self._log.close()
            self._log = None


class Watchdog:
    """Chiamato dal ciclo del ricevitore: arresto richiesto, VIIPER chiuso da solo."""

    CHECK_S = 0.25

    def __init__(self, viiper: ViiperProcess, stop: StopSignal, clock: Callable[[], float] = time.monotonic):
        self.viiper = viiper
        self.stop = stop
        self._clock = clock
        self._next_check = 0.0
        self._restart_at = 0.0

    def __call__(self, rx: receiver.Receiver) -> None:
        now = self._clock()
        if now < self._next_check:
            return
        self._next_check = now + self.CHECK_S
        if self.stop.is_set():
            _say("Arresto richiesto (avvio-automatico.ps1): fermo tutto.")
            rx.stop()
            return
        if self.viiper.alive() or now < self._restart_at:
            return
        self._restart_at = now + VIIPER_RESTART_S
        code = self.viiper.proc.returncode if self.viiper.proc is not None else None
        _say(f"VIIPER si è chiuso (codice {code}): lo riavvio. Log in {self.viiper.log_path.name}")
        try:
            # Senza attendere: il ricevitore si ricollega da solo appena VIIPER risponde.
            self.viiper.start(append=True)
        except OSError as exc:
            _say(f"riavvio di VIIPER fallito: {exc}")


# ---------------------------------------------------------------- avvio


def run_once(receiver_argv: list[str], stop: StopSignal, hidden: bool = False) -> int:
    """Trova usbip-win2 e VIIPER, avvia VIIPER e il ricevitore; ritorna quando si fermano."""
    try:
        cfg = load_config()
    except (OSError, ValueError) as exc:
        return _fail(f"configurazione non valida: {exc}")
    if _auto:
        logging.getLogger().setLevel(cfg.log_level.upper())
    if cfg.viiper_host not in ("127.0.0.1", "localhost", "::1"):
        return _fail("[viiper] host non è questo PC: l'avvio unico gestisce solo VIIPER locale.\n"
                     "Avviare VIIPER sull'altro PC e poi `python receiver.py`.")  # fmt: skip

    usbip = find_usbip(cfg)
    if usbip is None:
        return _fail(
            "usbip-win2 non trovato (usbip.exe).\n"
            "Installarlo con installa.bat, oppure da https://github.com/vadimgrn/usbip-win2/releases,\n"
            "e riavviare il PC. Se è installato in una cartella diversa, indicare [launcher] usbip_path\n"
            "in config.toml."
        )  # fmt: skip
    viiper = find_viiper(cfg)
    if viiper is None:
        return _fail(
            "VIIPER non trovato (viiper.exe).\n"
            "Installarlo con installa.bat, oppure da https://github.com/Alia5/VIIPER/releases,\n"
            "o indicare [launcher] viiper_path in config.toml."
        )  # fmt: skip
    _say(f"usbip-win2: {usbip}")
    _say(f"VIIPER:     {viiper}")

    client = ViiperClient(cfg.viiper_host, cfg.viiper_port, cfg.viiper_timeout_s)
    old = running_viiper_pids()
    if old:
        # Quello già attivo (es. avvio automatico di VIIPER) di solito non ha usbip nel PATH.
        _say(f"Chiudo il VIIPER già in esecuzione (PID {', '.join(map(str, old))}) per riavviarlo con usbip.")
        stop_pids(old)
        wait_until_down(client, 5.0)
    else:
        try:
            client.ping()
            return _fail(f"la porta {cfg.viiper_port} è occupata da un altro programma che risponde come VIIPER\n"
                         "ma non è viiper*.exe: chiuderlo e rilanciare.")  # fmt: skip
        except ViiperError:
            pass

    proc = ViiperProcess(viiper, usbip.parent, VIIPER_LOG, hidden)
    try:
        proc.start()
    except OSError as exc:
        return _fail(f"avvio di VIIPER fallito: {exc}")

    try:
        info = wait_for_viiper(client, 15.0, proc.proc)
        if info is None:
            return _fail(f"VIIPER non risponde sulla porta {cfg.viiper_port}. Log in {VIIPER_LOG}")
        _say(f"VIIPER {info.get('version', '?')} avviato (PID {proc.proc.pid}), log in {VIIPER_LOG.name}")
        if not _auto:
            print("Avvio il ricevitore. Per fermare tutto: Ctrl+C o chiudere la finestra.")
            print()
        return receiver.main(receiver_argv, on_idle=Watchdog(proc, stop))
    finally:
        proc.stop()
        _say("VIIPER fermato.")


def run_auto(run: Callable[[], int], stop: StopSignal, clock: Callable[[], float] = time.monotonic) -> int:
    """Ripete `run` finché non arriva la richiesta di arresto, con attese crescenti dopo gli errori."""
    backoff = AUTO_BACKOFF_S[0]
    while True:
        started = clock()
        try:
            code = run()
        except Exception:  # noqa: BLE001 - in background un errore non deve fermare tutto
            log.exception("errore inatteso")
            code = 1
        if stop.is_set():
            log.info("fermato su richiesta")
            return 0
        if clock() - started >= AUTO_STABLE_S:
            backoff = AUTO_BACKOFF_S[0]
        log.warning("ricevitore terminato (codice %s): riparto fra %.0f s", code, backoff)
        if stop.wait(backoff):
            log.info("fermato su richiesta")
            return 0
        backoff = min(backoff * 2, AUTO_BACKOFF_S[1])


class _LogStream(io.TextIOBase):
    """Al posto di stdout/stderr con pythonw (che non li ha): ogni riga va nel log."""

    def __init__(self, level: int):
        self.level = level
        self._busy = False

    def writable(self) -> bool:
        return True

    def write(self, text: str) -> int:
        if self._busy:
            return len(text)  # errore dentro il log stesso (es. disco pieno): niente ricorsione
        self._busy = True
        try:
            for line in text.splitlines():
                if line.strip():
                    log.log(self.level, line.rstrip())
        finally:
            self._busy = False
        return len(text)


def _setup_auto_logging() -> None:
    handler = logging.handlers.RotatingFileHandler(AUTO_LOG, maxBytes=1_000_000, backupCount=2, encoding="utf-8")
    handler.setFormatter(logging.Formatter("%(asctime)s %(levelname)-7s %(message)s", "%Y-%m-%d %H:%M:%S"))
    root = logging.getLogger()
    root.addHandler(handler)
    root.setLevel(logging.INFO)
    # receiver.main() chiama basicConfig: con un gestore già presente non fa nulla, e i
    # suoi messaggi finiscono qui.
    sys.stdout = _LogStream(logging.INFO)
    sys.stderr = _LogStream(logging.ERROR)


def parse_args(argv: list[str] | None) -> tuple[argparse.Namespace, list[str]]:
    """Opzioni del launcher; le altre (es. -v) passano al ricevitore."""
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--auto", action="store_true", help="modalità automatica, senza finestra")
    parser.add_argument("--delay", type=float, default=0.0, help="secondi di attesa prima di partire")
    return parser.parse_known_args(sys.argv[1:] if argv is None else argv)


def main(argv: list[str] | None = None) -> int:
    global _auto
    args, receiver_argv = parse_args(argv)
    _auto = args.auto
    if _auto:
        _setup_auto_logging()

    instance = SingleInstance()
    if not instance.acquire():
        if _auto:
            log.info("il ricevitore è già in esecuzione (in background o in un'altra finestra): esco")
            return 0
        return _fail("il ricevitore è già in esecuzione, in background (avvia.bat o avvio automatico)\n"
                     "o in un'altra finestra. Per fermare quello in background: ferma.bat.")  # fmt: skip
    stop = StopSignal()
    try:
        if not _auto:
            return run_once(receiver_argv, stop)
        log.info("avvio automatico (PID %d)", os.getpid())
        if args.delay > 0 and stop.wait(args.delay):
            return 0
        return run_auto(lambda: run_once(receiver_argv, stop, hidden=True), stop)
    finally:
        stop.close()
        instance.release()


if __name__ == "__main__":
    code = main()
    if code and not _auto and sys.stdin is not None and sys.stdin.isatty():
        try:
            input("Premere Invio per chiudere...")
        except EOFError:
            pass  # stdin è NUL (che per Windows è un terminale): niente da aspettare
    sys.exit(code)

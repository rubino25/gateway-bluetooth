"""Modalità automatica di launcher.py: una sola istanza, arresto su richiesta,
sorveglianza di VIIPER, ripartenza dopo gli errori (senza avviare VIIPER)."""

import logging
import subprocess
import sys
import textwrap
import uuid
from pathlib import Path

import pytest

import launcher
import receiver
from config import Config

ROOT = Path(__file__).parent.parent
windows_only = pytest.mark.skipif(sys.platform != "win32", reason="oggetti con nome di Windows")


def unique(prefix):
    return f"Local\\xeg-test-{prefix}-{uuid.uuid4().hex}"


def test_parse_args_splits_launcher_and_receiver_options():
    args, rest = launcher.parse_args(["--auto", "--delay", "30", "-v"])
    assert args.auto and args.delay == 30 and rest == ["-v"]
    args, rest = launcher.parse_args([])
    assert not args.auto and args.delay == 0 and rest == []


@windows_only
def test_single_instance_across_processes():
    name = unique("mutex")
    child = textwrap.dedent(f"""
        import sys
        sys.path.insert(0, {str(ROOT)!r})
        import launcher
        print(launcher.SingleInstance({name!r}).acquire())
    """)
    first = launcher.SingleInstance(name)
    assert first.acquire()
    try:
        out = subprocess.run([sys.executable, "-c", child], capture_output=True, text=True, timeout=30)
        assert out.stdout.strip() == "False", out.stderr
    finally:
        first.release()
    out = subprocess.run([sys.executable, "-c", child], capture_output=True, text=True, timeout=30)
    assert out.stdout.strip() == "True", out.stderr


@windows_only
def test_stop_signal():
    sig = launcher.StopSignal(unique("evt"))
    try:
        assert not sig.is_set()
        assert sig.wait(0.05) is False
        launcher._kernel32().SetEvent(sig._handle)  # come fa avvio-automatico.ps1
        assert sig.is_set() and sig.wait(5) is True
    finally:
        sig.close()


@windows_only
def test_stop_signal_ignores_old_request():
    name = unique("evt")
    old = launcher.StopSignal(name)
    launcher._kernel32().SetEvent(old._handle)
    new = launcher.StopSignal(name)  # riparte: la richiesta di prima non vale
    try:
        assert not new.is_set()
    finally:
        old.close()
        new.close()


class FakeStop:
    def __init__(self):
        self.set = False
        self.waits = []

    def is_set(self):
        return self.set

    def wait(self, seconds):
        self.waits.append(seconds)
        return self.set


class FakeViiper:
    log_path = Path("viiper.log")

    def __init__(self):
        self.running = True
        self.starts = []
        self.proc = type("P", (), {"returncode": None})()

    def alive(self):
        return self.running

    def start(self, append=False):
        self.starts.append(append)
        self.running = True


class FakeRx:
    stopped = False

    def stop(self):
        self.stopped = True


def test_watchdog_restarts_viiper_with_pause():
    now = [0.0]
    viiper, stop, rx = FakeViiper(), FakeStop(), FakeRx()
    dog = launcher.Watchdog(viiper, stop, clock=lambda: now[0])
    dog(rx)
    assert viiper.starts == []
    viiper.running = False
    viiper.proc.returncode = 1
    now[0] = 1.0
    dog(rx)
    assert viiper.starts == [True]  # in coda al log di prima
    viiper.running = False  # si richiude subito: non si riprova prima di VIIPER_RESTART_S
    now[0] = 2.0
    dog(rx)
    assert viiper.starts == [True]
    now[0] = 1.0 + launcher.VIIPER_RESTART_S
    dog(rx)
    assert viiper.starts == [True, True]
    assert not rx.stopped


def test_watchdog_stops_receiver_on_request():
    now = [0.0]
    stop, rx = FakeStop(), FakeRx()
    dog = launcher.Watchdog(FakeViiper(), stop, clock=lambda: now[0])
    stop.set = True
    dog(rx)
    assert rx.stopped


def test_watchdog_checks_at_most_every_check_interval():
    now = [10.0]
    stop, rx = FakeStop(), FakeRx()
    dog = launcher.Watchdog(FakeViiper(), stop, clock=lambda: now[0])
    dog(rx)
    stop.set = True
    now[0] += launcher.Watchdog.CHECK_S / 2
    dog(rx)
    assert not rx.stopped
    now[0] += launcher.Watchdog.CHECK_S
    dog(rx)
    assert rx.stopped


def test_run_auto_backoff_and_stop(caplog):
    stop = FakeStop()
    codes = iter([1, 2, 1])
    runs = []

    def run():
        runs.append(1)
        code = next(codes, None)
        if code is None:
            stop.set = True  # arresto chiesto mentre gira
            return 0
        return code

    with caplog.at_level(logging.INFO, logger="launcher"):
        assert launcher.run_auto(run, stop, clock=lambda: 0.0) == 0
    assert len(runs) == 4
    assert stop.waits == [5.0, 10.0, 20.0]
    assert "fermato su richiesta" in caplog.text


def test_run_auto_survives_exceptions_and_resets_backoff():
    stop = FakeStop()
    t = [0.0]
    calls = []

    def run():
        calls.append(1)
        if len(calls) == 1:
            raise RuntimeError("boom")
        if len(calls) == 2:
            t[0] += launcher.AUTO_STABLE_S  # ha girato a lungo prima di chiudersi
            return 1
        stop.set = True
        return 0

    assert launcher.run_auto(run, stop, clock=lambda: t[0]) == 0
    assert stop.waits == [5.0, 5.0]


def test_run_auto_stops_during_wait():
    stop = FakeStop()

    def wait(seconds):
        stop.waits.append(seconds)
        return True  # richiesta arrivata durante l'attesa

    stop.wait = wait
    assert launcher.run_auto(lambda: 1, stop) == 0
    assert stop.waits == [5.0]


def test_backoff_caps_at_maximum():
    stop = FakeStop()
    n = [0]

    def run():
        n[0] += 1
        if n[0] > 6:
            stop.set = True
        return 1

    launcher.run_auto(run, stop, clock=lambda: 0.0)
    assert stop.waits == [5.0, 10.0, 20.0, 40.0, 60.0, 60.0]


def test_log_stream_writes_lines(caplog):
    stream = launcher._LogStream(logging.ERROR)
    with caplog.at_level(logging.INFO, logger="launcher"):
        print("prima riga\nseconda", file=stream)
        stream.write("\n")
    assert [r.getMessage() for r in caplog.records] == ["prima riga", "seconda"]
    assert all(r.levelno == logging.ERROR for r in caplog.records)


def test_receiver_run_calls_on_idle_until_stopped():
    rx = receiver.Receiver(Config(bind_address="127.0.0.1", port=0), pad_factory=lambda *a: None)
    calls = []

    def on_idle(r):
        calls.append(r)
        if len(calls) == 3:
            r.stop()

    try:
        rx.step = lambda timeout=None: None  # niente attese reali
        rx.run(on_idle)
    finally:
        rx.close()
    assert calls == [rx, rx, rx]

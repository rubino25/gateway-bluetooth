"""Lettura di config.toml con valori di default."""

from __future__ import annotations

import tomllib
from dataclasses import dataclass, field
from pathlib import Path

import protocol

DEFAULT_PATH = Path(__file__).with_name("config.toml")


@dataclass
class Config:
    # [udp]
    bind_address: str = "0.0.0.0"
    port: int = protocol.DEFAULT_PORT
    allowed_sources: list[str] = field(default_factory=list)  # vuoto = qualsiasi
    # [viiper]
    viiper_host: str = "127.0.0.1"
    viiper_port: int = 3242
    viiper_bus_id: int = 0  # 0 = automatico
    viiper_timeout_s: float = 1.0
    viiper_retry_s: float = 3.0
    # [mapping]
    invert_y: bool = True  # solo per il pad Xbox 360
    dualsense_device: str = "dualsense"  # "dualsense" o "xbox360": come emulare il DualSense
    # [timeouts]
    release_s: float = 1.0
    remove_s: float = 5.0
    # [rumble]
    rumble_duration_ms: int = 500
    rumble_refresh_ms: int = 250
    heartbeat_ms: int = 1000  # 0 = disattivato
    # [launcher] percorsi facoltativi, vuoto = ricerca automatica
    usbip_path: str = ""
    viiper_path: str = ""
    # [simulate]
    simulate_target: str = "127.0.0.1"
    # [logging]
    log_level: str = "INFO"


# sezione -> {chiave del file: attributo di Config}
_KEYS = {
    "udp": {"bind_address": "bind_address", "port": "port", "allowed_sources": "allowed_sources"},
    "viiper": {
        "host": "viiper_host",
        "port": "viiper_port",
        "bus_id": "viiper_bus_id",
        "timeout_s": "viiper_timeout_s",
        "retry_s": "viiper_retry_s",
    },
    "mapping": {"invert_y": "invert_y", "dualsense": "dualsense_device"},
    "timeouts": {"release_s": "release_s", "remove_s": "remove_s"},
    "rumble": {
        "duration_ms": "rumble_duration_ms",
        "refresh_ms": "rumble_refresh_ms",
        "heartbeat_ms": "heartbeat_ms",
    },
    "launcher": {"usbip_path": "usbip_path", "viiper_path": "viiper_path"},
    "simulate": {"target": "simulate_target"},
    "logging": {"level": "log_level"},
}


def load_config(path: str | Path | None = None) -> Config:
    """Carica il file indicato (default: config.toml accanto agli script).

    Se il file di default manca si usano i valori predefiniti; chiavi o sezioni
    sconosciute sono un errore, per non ignorare in silenzio un refuso.
    """
    cfg = Config()
    file = Path(path) if path is not None else DEFAULT_PATH
    if path is None and not file.exists():
        return cfg
    # utf-8-sig: accetta anche i file salvati con il BOM (alcuni editor di Windows).
    data = tomllib.loads(file.read_text(encoding="utf-8-sig"))
    for section, values in data.items():
        if section not in _KEYS or not isinstance(values, dict):
            raise ValueError(f"{file.name}: sezione sconosciuta [{section}]")
        for key, value in values.items():
            attr = _KEYS[section].get(key)
            if attr is None:
                raise ValueError(f"{file.name}: chiave sconosciuta {section}.{key}")
            expected = type(getattr(cfg, attr))
            if expected is float and isinstance(value, int) and not isinstance(value, bool):
                value = float(value)
            if type(value) is not expected:
                raise ValueError(
                    f"{file.name}: {section}.{key} deve essere di tipo {expected.__name__}"
                )
            setattr(cfg, attr, value)
    if not 0 < cfg.port < 65536 or not 0 < cfg.viiper_port < 65536:
        raise ValueError(f"{file.name}: porta fuori range")
    if cfg.dualsense_device not in ("dualsense", "xbox360"):
        raise ValueError(f"{file.name}: mapping.dualsense deve essere \"dualsense\" o \"xbox360\"")
    if cfg.remove_s < cfg.release_s:
        raise ValueError(f"{file.name}: timeouts.remove_s deve essere >= release_s")
    return cfg

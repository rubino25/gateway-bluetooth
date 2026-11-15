#!/usr/bin/env python3
"""Server finto per provare la pagina web del gateway senza hardware.

Serve i file di firmware/main/www e risponde alle stesse API JSON di web_ui.c
con dati di esempio tenuti in memoria. Solo libreria standard, Python 3.11+.

Uso:
    python firmware/tools/mock_server.py                 # primo avvio: chiede di creare la password
    python firmware/tools/mock_server.py --password X    # parte con la password già impostata
    python firmware/tools/mock_server.py --port 8080
    python firmware/tools/mock_server.py --controller dualsense   # controller finto: xbox (default) o dualsense

A ogni abbinamento simulato il controller finto si alterna tra Xbox e DualSense.

Poi aprire http://localhost:8080/ nel browser.
"""

from __future__ import annotations

import argparse
import hmac
import json
import secrets
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

WWW_DIR = Path(__file__).resolve().parent.parent / "main" / "www"

STATIC_FILES = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/index.html": ("index.html", "text/html; charset=utf-8"),
    "/app.css": ("app.css", "text/css; charset=utf-8"),
    "/app.js": ("app.js", "application/javascript; charset=utf-8"),
    "/dongle.png": ("dongle.png", "image/png"),
}

COOKIE_NAME = "gw_session"
PASSWORD_MIN = 8
PASSWORD_MAX = 64
SESSION_IDLE_S = 30 * 60
LOGIN_FREE_ATTEMPTS = 5
LOGIN_LOCK_BASE_S = 30
LOGIN_LOCK_MAX_S = 15 * 60
MAX_JSON_BODY = 1024
OTA_PARTITION_SIZE = 0x300000  # dimensione finta della partizione OTA
VALID_BAUDS = (115200, 460800, 921600)
SECURITY_HEADERS = {
    "X-Content-Type-Options": "nosniff",
    "X-Frame-Options": "DENY",
    "Referrer-Policy": "no-referrer",
    "Content-Security-Policy": (
        "default-src 'self'; img-src 'self' data:; style-src 'self' 'unsafe-inline'; "
        "frame-ancestors 'none'; base-uri 'none'; form-action 'self'"
    ),
}


class ApiError(Exception):
    def __init__(self, status: int, message: str, extra: dict | None = None):
        super().__init__(message)
        self.status = status
        self.message = message
        self.extra = extra or {}


# Controller finti: come li riporta /api/status ("type" e "transport" come web_ui.c).
CONTROLLERS = {
    "xbox": {"name": "Xbox Wireless Controller", "addr": "02:00:00:CC:DD:02", "transport": "le"},
    "dualsense": {"name": "DualSense Wireless Controller", "addr": "02:00:00:CC:DD:03", "transport": "classic"},
}


def default_settings() -> dict:
    return {
        "device_name": "Xbox Gateway",
        "hostname": "dongle-m",
        "eth_dhcp": True,
        "eth_ip": "",
        "eth_netmask": "",
        "eth_gateway": "",
        "eth_dns": "",
        "pc_host": "",
        "pc_port": 47900,
        "mg24_baud": 115200,
        "mg24_tcp_port": 6638,
        "led_enabled": True,
        "bt_idle_timeout_min": 0,  # 0 = mai
    }


def valid_ipv4(text: str) -> bool:
    parts = text.split(".")
    if len(parts) != 4:
        return False
    return all(p.isdigit() and len(p) <= 3 and int(p) <= 255 for p in parts)


def valid_hostname(text: str) -> bool:
    if not 1 <= len(text) <= 31:
        return False
    if text[0] == "-" or text[-1] == "-":
        return False
    return all(c.isascii() and (c.isalnum() or c == "-") for c in text)


class Device:
    """Stato del dongle finto. Tutti gli accessi passano dal lock."""

    def __init__(self, password: str | None):
        self.lock = threading.RLock()
        self.password = password
        self.sessions: dict[str, float] = {}
        self.fail_count = 0
        self.lock_until = 0.0
        self.settings = default_settings()
        self.boot_time = time.monotonic()
        self.offline_until = 0.0  # simula il riavvio: il server non risponde
        self.version = "0.1.0"

        self.pairing = False
        self.pairing_since = 0.0
        self.bt_connected = True
        self.bt_kind = "xbox"  # chiave di CONTROLLERS
        self.battery = 80
        self.bonded = 1
        self.zb_bootloader = False
        self.zb_client_since = time.monotonic() - 20 * 60
        self.log = ""
        self.log_total = 0
        self._log_boot()

    # --- log -----------------------------------------------------------------

    def add_log(self, tag: str, text: str, level: str = "I") -> None:
        with self.lock:
            ms = int((time.monotonic() - self.boot_time) * 1000)
            line = f"{level} ({ms}) {tag}: {text}\n"
            self.log += line
            self.log_total += len(line.encode())
            self.log = self.log[-8192:]

    def _log_boot(self) -> None:
        self.add_log("main", "xbox-eth-gateway (mock)")
        self.add_log("eth", "link up, 100 Mbps full duplex")
        self.add_log("eth", "indirizzo ottenuto via DHCP")
        self.add_log("mg24", "ponte TCP in ascolto sulla porta 6638")
        self.add_log("web_ui", "server HTTP avviato sulla porta 80")
        self.add_log("gw_bt", "Bluepad32 pronto")

    # --- simulazione ----------------------------------------------------------

    def tick(self) -> None:
        """Fa evolvere lo stato finto: dopo 6 s di abbinamento compare un controller."""
        with self.lock:
            now = time.monotonic()
            if self.pairing and not self.bt_connected and now - self.pairing_since > 6:
                # Un abbinamento sì e uno no arriva l'altro tipo di controller.
                self.bt_kind = "dualsense" if self.bt_kind == "xbox" else "xbox"
                self.bt_connected = True
                self.pairing = False
                self.bonded = max(self.bonded, 1)
                pad = CONTROLLERS[self.bt_kind]
                self.add_log("gw_bt", f"Controller pronto: \"{pad['name']}\" "
                             f"({'BR/EDR' if pad['transport'] == 'classic' else 'BLE'})")

    def reboot(self, factory: bool = False) -> None:
        with self.lock:
            self.sessions.clear()
            self.offline_until = time.monotonic() + 4
            self.boot_time = time.monotonic() + 4
            self.zb_bootloader = False
            self.zb_client_since = time.monotonic() + 9
            self.pairing = False
            if factory:
                self.password = None
                self.settings = default_settings()
                self.bonded = 0
                self.bt_connected = False
            self.log = ""
            self._log_boot()

    def status(self) -> dict:
        with self.lock:
            now = time.monotonic()
            uptime = max(0, int(now - self.boot_time))
            s = self.settings
            dhcp = s["eth_dhcp"]
            zb_connected = not self.zb_bootloader and now >= self.zb_client_since
            pc_seen = self.bt_connected
            pad = CONTROLLERS[self.bt_kind]
            return {
                "device": {
                    "name": s["device_name"],
                    "hostname": s["hostname"],
                    "version": self.version,
                    "build_date": "Nov 14 2026 10:30:00",
                    "idf": "v5.5.1",
                    "chip": "ESP32",
                    "revision": "3.1",
                    "cores": 2,
                    "flash": "16MB",
                    "partition": "ota_0",
                    "uptime_s": uptime,
                    "free_heap": 148_000 - (uptime % 7) * 512,
                    "reset_reason": "Power on",
                },
                "eth": {
                    "link_up": True,
                    "has_ip": True,
                    "dhcp": dhcp,
                    "ip": "192.168.1.50" if dhcp else s["eth_ip"],
                    "netmask": "255.255.255.0" if dhcp else s["eth_netmask"],
                    "gateway": "192.168.1.1" if dhcp else s["eth_gateway"],
                    "dns": "192.168.1.1" if dhcp else s["eth_dns"],
                    "mac": "02:00:00:AA:BB:01",
                },
                "bt": {
                    "connected": self.bt_connected,
                    "pairing": self.pairing,
                    "name": pad["name"] if self.bt_connected else "",
                    "addr": pad["addr"] if self.bt_connected else "",
                    "battery": self.battery if self.bt_connected else -1,
                    "bonded": self.bonded,
                    "type": self.bt_kind if self.bt_connected else "",
                    "transport": pad["transport"] if self.bt_connected else "",
                    "classic_enabled": True,
                },
                "zigbee": {
                    "client_connected": zb_connected,
                    "client_ip": "192.168.1.10" if zb_connected else "",
                    "connected_s": int(now - self.zb_client_since) if zb_connected else 0,
                    "baud": 115200 if self.zb_bootloader else s["mg24_baud"],
                    "bootloader": self.zb_bootloader,
                    "bytes_to_radio": uptime * 37 if zb_connected else 0,
                    "bytes_from_radio": uptime * 212 if zb_connected else 0,
                    "tcp_port": s["mg24_tcp_port"],
                },
                "pc": {
                    "seen": pc_seen,
                    "ip": "192.168.1.20" if pc_seen else "",
                    "tx_packets": uptime * 125 if self.bt_connected else uptime * 10,
                    "rx_packets": uptime // 3 if pc_seen else 0,
                    "last_rx_age_ms": 340 if pc_seen else None,
                    "host": s["pc_host"],
                    "port": s["pc_port"],
                },
            }


class Handler(BaseHTTPRequestHandler):
    server_version = "gw-mock/1"
    protocol_version = "HTTP/1.1"
    dev: Device  # impostato in main()

    def log_message(self, fmt, *args):  # meno rumore sul terminale
        if "/api/status" in str(args[0]) or "/api/logs" in str(args[0]):
            return
        super().log_message(fmt, *args)

    # --- utilità ---------------------------------------------------------------

    def _send(self, status: int, body: bytes, ctype: str, headers: dict | None = None) -> None:
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store" if self.path.startswith("/api/") else "no-cache")
        for k, v in SECURITY_HEADERS.items():
            self.send_header(k, v)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _json(self, status: int, obj: dict, headers: dict | None = None) -> None:
        self._send(status, json.dumps(obj).encode(), "application/json", headers)

    def _session_token(self) -> str | None:
        """Ritorna il token se il cookie corrisponde a una sessione valida."""
        raw = self.headers.get("Cookie", "")
        token = None
        for part in raw.split(";"):
            name, _, value = part.strip().partition("=")
            if name == COOKIE_NAME:
                token = value
        if not token:
            return None
        now = time.monotonic()
        with self.dev.lock:
            found = None
            for known, last in list(self.dev.sessions.items()):
                if now - last > SESSION_IDLE_S:
                    del self.dev.sessions[known]
                elif hmac.compare_digest(known, token):
                    found = known
            if found:
                self.dev.sessions[found] = now
            return found

    def _new_session_cookie(self) -> dict:
        token = secrets.token_hex(16)
        with self.dev.lock:
            self.dev.sessions[token] = time.monotonic()
        return {"Set-Cookie": f"{COOKIE_NAME}={token}; Path=/; HttpOnly; SameSite=Strict"}

    def _origin_ok(self) -> bool:
        origin = self.headers.get("Origin", "")
        host = self.headers.get("Host", "")
        if not origin.startswith("http://") or not host:
            return False
        return origin[len("http://"):].lower() == host.lower()

    def _read_json(self) -> dict:
        length = int(self.headers.get("Content-Length", "0") or 0)
        if length == 0:
            return {}
        if length > MAX_JSON_BODY:
            raise ApiError(413, "Request too large")
        try:
            obj = json.loads(self.rfile.read(length))
        except (ValueError, UnicodeDecodeError):
            raise ApiError(400, "Invalid JSON") from None
        if not isinstance(obj, dict):
            raise ApiError(400, "Invalid JSON")
        return obj

    def _offline(self) -> bool:
        if time.monotonic() < self.dev.offline_until:
            # Durante il "riavvio" la connessione cade senza risposta.
            self.close_connection = True
            return True
        return False

    def _check_lockout(self) -> None:
        with self.dev.lock:
            left = self.dev.lock_until - time.monotonic()
        if left > 0:
            raise ApiError(429, "Too many attempts. Try again later.", {"retry_after": int(left) + 1})

    def _password_attempt(self, plain) -> bool:
        """Controlla la password applicando il limite ai tentativi."""
        self._check_lockout()
        dev = self.dev
        with dev.lock:
            ok = isinstance(plain, str) and dev.password is not None and hmac.compare_digest(
                plain.encode(), dev.password.encode()
            )
            if ok:
                dev.fail_count = 0
                return True
            dev.fail_count += 1
            if dev.fail_count >= LOGIN_FREE_ATTEMPTS:
                shift = min(dev.fail_count - LOGIN_FREE_ATTEMPTS, 5)
                dev.lock_until = time.monotonic() + min(LOGIN_LOCK_BASE_S << shift, LOGIN_LOCK_MAX_S)
            return False

    # --- GET -------------------------------------------------------------------

    def do_GET(self):  # noqa: N802
        if self._offline():
            return
        self.dev.tick()
        url = urlsplit(self.path)
        if url.path in STATIC_FILES:
            name, ctype = STATIC_FILES[url.path]
            try:
                body = (WWW_DIR / name).read_bytes()
            except OSError:
                self._send(404, b"not found", "text/plain")
                return
            self._send(200, body, ctype)
            return
        if url.path == "/favicon.ico":
            self._send(204, b"", "image/x-icon")
            return
        if not url.path.startswith("/api/"):
            self._send(404, b"not found", "text/plain")
            return
        try:
            self._api_get(url.path, parse_qs(url.query))
        except ApiError as e:
            self._json(e.status, {"error": e.message, **e.extra})

    def _api_get(self, path: str, query: dict) -> None:
        dev = self.dev
        if path == "/api/session":
            self._json(200, {
                "setup_required": dev.password is None,
                "authenticated": self._session_token() is not None,
            })
            return
        if self._session_token() is None:
            raise ApiError(401, "Not authenticated")
        if path == "/api/status":
            self._json(200, dev.status())
        elif path == "/api/settings":
            with dev.lock:
                self._json(200, dict(dev.settings))
        elif path == "/api/logs":
            try:
                since = int(query.get("since", ["0"])[0])
            except ValueError:
                since = 0
            with dev.lock:
                data = dev.log.encode()
                total = dev.log_total
            start = total - len(data)
            if since < start or since > total:
                since = start
            self._send(200, data[since - start:], "text/plain; charset=utf-8",
                       {"X-Log-Next": str(total)})
        else:
            raise ApiError(404, "Not found")

    # --- POST ------------------------------------------------------------------

    def do_POST(self):  # noqa: N802
        if self._offline():
            return
        self.dev.tick()
        path = urlsplit(self.path).path
        try:
            if not path.startswith("/api/"):
                raise ApiError(404, "Not found")
            if not self._origin_ok():
                raise ApiError(403, "Origin check failed")
            self._api_post(path)
        except ApiError as e:
            # Il corpo non letto va scartato, altrimenti sporca la connessione.
            self.close_connection = True
            self._json(e.status, {"error": e.message, **e.extra})

    def _api_post(self, path: str) -> None:
        dev = self.dev

        # Senza sessione: impostazione iniziale e login.
        if path == "/api/setup":
            body = self._read_json()
            if dev.password is not None:
                raise ApiError(403, "Password already set")
            self._check_new_password(body.get("password"))
            dev.password = body["password"]
            dev.add_log("web_ui", "password impostata")
            self._json(200, {"ok": True}, self._new_session_cookie())
            return
        if path == "/api/login":
            body = self._read_json()
            if dev.password is None:
                raise ApiError(403, "Setup required")
            if not self._password_attempt(body.get("password")):
                self._check_lockout()
                raise ApiError(401, "Wrong password")
            self._json(200, {"ok": True}, self._new_session_cookie())
            return

        token = self._session_token()
        if token is None:
            raise ApiError(401, "Not authenticated")

        if path == "/api/ota":
            self._ota()
            return

        body = self._read_json()
        ok = {"ok": True}

        if path == "/api/logout":
            with dev.lock:
                dev.sessions.pop(token, None)
            self._json(200, ok, {"Set-Cookie": f"{COOKIE_NAME}=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict"})
        elif path == "/api/password":
            if not self._password_attempt(body.get("current")):
                self._check_lockout()
                raise ApiError(403, "Current password is wrong")
            self._check_new_password(body.get("new"))
            with dev.lock:
                dev.password = body["new"]
                dev.sessions.clear()
            self._json(200, ok, self._new_session_cookie())
        elif path == "/api/settings":
            self._json(200, self._update_settings(body))
        elif path == "/api/controller/pairing":
            enable = self._bool(body, "enable")
            with dev.lock:
                dev.pairing = enable
                dev.pairing_since = time.monotonic()
            dev.add_log("gw_bt", f"abbinamento {'attivo' if enable else 'fermo'}")
            self._json(200, ok)
        elif path == "/api/controller/disconnect":
            with dev.lock:
                dev.bt_connected = False
            dev.add_log("gw_bt", "controller scollegato")
            self._json(200, ok)
        elif path == "/api/controller/forget":
            with dev.lock:
                dev.bt_connected = False
                dev.bonded = 0
            dev.add_log("gw_bt", "controller dimenticati")
            self._json(200, ok)
        elif path == "/api/controller/rumble":
            weak = self._int(body, "weak", 0, 255)
            strong = self._int(body, "strong", 0, 255)
            duration = self._int(body, "duration_ms", 1, 2000)
            if not dev.bt_connected:
                raise ApiError(409, "No controller connected")
            dev.add_log("gw_bt", f"rumble weak={weak} strong={strong} {duration} ms")
            self._json(200, ok)
        elif path == "/api/zigbee/reset":
            with dev.lock:
                dev.zb_bootloader = False
                dev.zb_client_since = time.monotonic() + 5
            dev.add_log("mg24", "reset")
            self._json(200, ok)
        elif path == "/api/zigbee/bootloader":
            enter = self._bool(body, "enter")
            with dev.lock:
                dev.zb_bootloader = enter
                dev.zb_client_since = time.monotonic() + 5
            dev.add_log("mg24", "bootloader" if enter else "applicazione")
            self._json(200, ok)
        elif path == "/api/zigbee/baud":
            baud = self._int(body, "baud", 0, 10_000_000)
            if baud not in VALID_BAUDS:
                raise ApiError(400, "Unsupported baud rate")
            with dev.lock:
                dev.settings["mg24_baud"] = baud
            dev.add_log("mg24", f"baud rate {baud}")
            self._json(200, ok)
        elif path == "/api/find":
            dev.add_log("led", "find device")
            self._json(200, ok)
        elif path == "/api/reboot":
            self._json(200, ok)
            dev.reboot()
        elif path == "/api/factory-reset":
            if body.get("confirm") is not True:
                raise ApiError(400, "Confirmation required")
            self._json(200, ok)
            dev.reboot(factory=True)
        else:
            raise ApiError(404, "Not found")

    # --- validazione -----------------------------------------------------------

    @staticmethod
    def _bool(body: dict, key: str) -> bool:
        value = body.get(key)
        if not isinstance(value, bool):
            raise ApiError(400, f"Invalid '{key}'")
        return value

    @staticmethod
    def _int(body: dict, key: str, lo: int, hi: int) -> int:
        value = body.get(key)
        if isinstance(value, bool) or not isinstance(value, int) or not lo <= value <= hi:
            raise ApiError(400, f"Invalid '{key}'")
        return value

    @staticmethod
    def _check_new_password(plain) -> None:
        if not isinstance(plain, str) or not PASSWORD_MIN <= len(plain.encode()) <= PASSWORD_MAX:
            raise ApiError(400, f"Password must be {PASSWORD_MIN} to {PASSWORD_MAX} characters")

    def _update_settings(self, body: dict) -> dict:
        dev = self.dev
        limits = {"device_name": 31, "hostname": 31, "eth_ip": 15, "eth_netmask": 15,
                  "eth_gateway": 15, "eth_dns": 15, "pc_host": 63}
        with dev.lock:
            new = dict(dev.settings)
            for key, value in body.items():
                if key in limits:
                    if not isinstance(value, str) or len(value.encode()) > limits[key]:
                        raise ApiError(400, f"Invalid '{key}'")
                    new[key] = value
                elif key in ("eth_dhcp", "led_enabled"):
                    new[key] = self._bool(body, key)
                elif key in ("pc_port", "mg24_tcp_port"):
                    new[key] = self._int(body, key, 1, 65535)
                elif key == "bt_idle_timeout_min":
                    new[key] = self._int(body, key, 0, 240)
                else:
                    raise ApiError(400, f"Unknown setting '{key}'")

            # Stesse regole attese da settings_set().
            name = new["device_name"]
            if not name or name != name.strip() or not all(" " <= c <= "~" for c in name):
                raise ApiError(400, "Invalid settings")
            if new["mg24_tcp_port"] == 80:
                raise ApiError(400, "Invalid settings")
            if not valid_hostname(new["hostname"]):
                raise ApiError(400, "Invalid settings")
            if not new["eth_dhcp"]:
                if not all(valid_ipv4(new[k]) for k in ("eth_ip", "eth_netmask")):
                    raise ApiError(400, "Invalid settings")
                if any(new[k] and not valid_ipv4(new[k]) for k in ("eth_gateway", "eth_dns")):
                    raise ApiError(400, "Invalid settings")
            if new["pc_host"] and not valid_ipv4(new["pc_host"]):
                raise ApiError(400, "Invalid settings")

            # pc_host viene riletto dal firmware senza riavvio; il resto no.
            restart_keys = ("hostname", "eth_dhcp", "eth_ip", "eth_netmask", "eth_gateway",
                            "eth_dns", "pc_port", "mg24_tcp_port")
            restart = any(new[k] != dev.settings[k] for k in restart_keys)
            dev.settings = new
        dev.add_log("settings", "impostazioni salvate")
        return {"ok": True, "restart_required": restart}

    # --- OTA -------------------------------------------------------------------

    def _ota(self) -> None:
        dev = self.dev
        total = int(self.headers.get("Content-Length", "0") or 0)
        if total < 288:
            raise ApiError(400, "Image too small")
        if total > OTA_PARTITION_SIZE:
            raise ApiError(413, "Image larger than the OTA partition")
        first = b""
        received = 0
        while received < total:
            chunk = self.rfile.read(min(4096, total - received))
            if not chunk:
                self.close_connection = True
                return
            if not first:
                first = chunk
            received += len(chunk)
            time.sleep(0.004)  # simula la scrittura in flash
        if first[0] != 0xE9:
            raise ApiError(400, "Not an ESP32 application image")
        dev.add_log("web_ui", f"OTA: ricevuti {received} byte, riavvio")
        self._json(200, {"ok": True, "version": dev.version})
        dev.reboot()


def main() -> None:
    parser = argparse.ArgumentParser(description="Server finto per la pagina web del gateway")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--bind", default="127.0.0.1", help="indirizzo di ascolto (default solo locale)")
    parser.add_argument("--password", default=None,
                        help="parte con la password già impostata (altrimenti la pagina chiede di crearla)")
    parser.add_argument("--battery", type=int, default=80, choices=range(-1, 101), metavar="{-1..100}",
                        help="batteria del controller finto; -1 = non riportata dal controller")
    parser.add_argument("--controller", default="xbox", choices=sorted(CONTROLLERS),
                        help="controller finto collegato all'avvio")
    parser.add_argument("--idle-timeout", type=int, default=0, choices=range(0, 241), metavar="{0..240}",
                        help="minuti di inattività prima dello scollegamento (0 = mai); "
                             "un valore fuori dalle voci del menu compare come voce in più")
    args = parser.parse_args()

    Handler.dev = Device(args.password)
    Handler.dev.battery = args.battery
    Handler.dev.bt_kind = args.controller
    Handler.dev.settings["bt_idle_timeout_min"] = args.idle_timeout
    httpd = ThreadingHTTPServer((args.bind, args.port), Handler)
    httpd.daemon_threads = True
    print(f"Mock del gateway su http://{args.bind}:{args.port}/  (Ctrl+C per fermare)")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

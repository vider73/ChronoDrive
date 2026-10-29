"""Paths, config, the device secret, the log and small helpers.

  ~/.config/chronodrive/config.json   server, account, device, the drives mounted here (plain)
  ~/.config/chronodrive/secret        the device secret (0600)
  ~/.local/state/chronodrive/         index-<drive>.json, status-<drive>.json, lock-<drive>, chronodrive.log, servers.json
XDG_CONFIG_HOME / XDG_STATE_HOME move them (two instances on one PC, tests)."""
import datetime
import json
import os
import re
import socket
import threading
import time

APP_ID = "chronodrive"
DEFAULT_SERVER = "https://cs-miniserver.chronoscan.org"
DEFAULT_STANDBY = "https://cs-miniserver-r.chronoscan.org"
PRIMARY_NAMES = ("cs-miniserver.chronoscan.org", "cs-miniserver.swedencentral.cloudapp.azure.com")
SMALL_MAX = 8 << 20      # whole-file PUT up to here; chunked uploads past it
CHUNK = 8 << 20          # bytes per upload chunk


# ------------------------------------------------------------------ paths
def config_dir():
    return os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "chronodrive")


def state_dir():
    d = os.path.join(os.environ.get("XDG_STATE_HOME") or os.path.expanduser("~/.local/state"), "chronodrive")
    os.makedirs(d, exist_ok=True)
    return d


def data_home():
    return os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")


def config_path():
    return os.path.join(config_dir(), "config.json")


def secret_path():
    return os.path.join(config_dir(), "secret")


def index_path(drive_id):
    return os.path.join(state_dir(), f"index-{drive_id}.json")


def status_path(drive_id):
    return os.path.join(state_dir(), f"status-{drive_id}.json")


def lock_path(drive_id):
    return os.path.join(state_dir(), f"lock-{drive_id}")


def kick_path():
    return os.path.join(state_dir(), "kick")


def log_path():
    return os.path.join(state_dir(), "chronodrive.log")


def write_atomic(path, data, mode=None):
    tmp = f"{path}.tmp{os.getpid()}"
    flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC
    fd = os.open(tmp, flags, mode if mode is not None else 0o644)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data.encode() if isinstance(data, str) else data)
            f.flush()
            os.fsync(f.fileno())
        if mode is not None:
            os.chmod(tmp, mode)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise


def read_json(path, default=None):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


# ------------------------------------------------------------------ log (chronodrive.log, rotated at 2 MB; the service also prints to the journal)
_log_lock = threading.Lock()
LOG_TO_STDOUT = False


def log(line):
    stamp = time.strftime("%Y-%m-%d %H:%M:%S")
    with _log_lock:
        p = log_path()
        try:
            if os.path.getsize(p) > 2 << 20:
                os.replace(p, os.path.join(state_dir(), "chronodrive.1.log"))
        except OSError:
            pass
        try:
            with open(p, "a", encoding="utf-8") as f:
                f.write(f"{stamp} {line}\n")
        except OSError:
            pass
        if LOG_TO_STDOUT:
            print(line, flush=True)


def log_tail(n):
    try:
        with open(log_path(), encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()[-n:]
    except OSError:
        return []


# ------------------------------------------------------------------ config and secret
class Config:
    def __init__(self):
        self.url = DEFAULT_SERVER
        self.standby = ""
        self.email = ""
        self.device_id = ""
        self.device_name = ""
        self.drives = []          # [{id, name, node, node_kind, folder, paused}]

    @classmethod
    def load(cls):
        c = cls()
        j = read_json(config_path(), {}) or {}
        c.url = j.get("url") or c.url
        c.standby = j.get("standby", "")
        c.email = j.get("email", "")
        c.device_id = j.get("device_id", "")
        c.device_name = j.get("device_name", "")
        c.drives = [d for d in j.get("drives", []) if d.get("id") and d.get("folder")]
        env = os.environ.get("MINISERVER_URL", "")
        if env:                                   # the environment wins (the lab rule)
            c.url = env
        c.url = c.url.rstrip("/")
        return c

    def save(self):
        os.makedirs(config_dir(), mode=0o700, exist_ok=True)
        j = {"url": self.url, "standby": self.standby, "email": self.email, "device_id": self.device_id,
             "device_name": self.device_name, "drives": self.drives}
        write_atomic(config_path(), json.dumps(j, indent=2, ensure_ascii=False) + "\n")

    def find(self, drive_id):
        for d in self.drives:
            if d["id"] == drive_id:
                return d
        return None


def load_secret():
    j = read_json(secret_path(), {}) or {}
    return j.get("device_secret", "")


def save_secret(value):
    os.makedirs(config_dir(), mode=0o700, exist_ok=True)
    if not value:
        try:
            os.remove(secret_path())
        except OSError:
            pass
        return
    write_atomic(secret_path(), json.dumps({"device_secret": value}), mode=0o600)


# ------------------------------------------------------------------ names and small helpers
def host_name():
    return socket.gethostname().split(".")[0] or "linux"


def device_tag(name):
    """The PC's name as the grant takes it: up to 64 letters, digits, '.', '_' or '-'."""
    t = re.sub(r"[^A-Za-z0-9._-]+", "-", name).strip("-")
    return (t or "linux")[:64]


def human(b):
    b = int(b or 0)
    if b >= 1 << 30:
        return f"{b / (1 << 30):.2f} GB"
    if b >= 1 << 20:
        return f"{b / (1 << 20):.1f} MB"
    if b >= 1024:
        return f"{b // 1024} KB"
    return f"{b} B"


def stamp_local(t=None):
    return time.strftime("%Y-%m-%d %H-%M", time.localtime(t or time.time()))


def time_local(t):
    return time.strftime("%H:%M:%S", time.localtime(t)) if t else ""


def ago(t):
    if not t:
        return "never"
    s = int(time.time() - t)
    if s < 60:
        return f"{s} s ago"
    if s < 3600:
        return f"{s // 60} min ago"
    if s < 86400:
        return f"{s // 3600} h ago"
    return datetime.datetime.fromtimestamp(t).strftime("%Y-%m-%d %H:%M")


def safe_folder_name(name):
    o = "".join("_" if c in "/\0" or ord(c) < 32 else c for c in name).strip().rstrip(".")
    return o or "Drive"


def short_home(path):
    h = os.path.expanduser("~")
    return "~" + path[len(h):] if path == h or path.startswith(h + "/") else path


def pid_alive(pid):
    try:
        os.kill(int(pid), 0)
        return True
    except (OSError, ValueError, TypeError):
        return False

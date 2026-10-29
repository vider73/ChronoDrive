"""chronodrive: the console interface (and the background service the systemd user unit runs).

  chronodrive                       a menu (in a terminal)
  chronodrive pair [--url U] [--name N] [--no-qr]   pair this PC with a Miniserver account (code + QR, waits)
  chronodrive drives                the account's drives and those shared with it: size, used, node, reachable, mounted where
  chronodrive mount <drive> [folder]   keep a drive in sync in a folder (default ~/ChronoDrive/<name>)
  chronodrive unmount <drive>       stop syncing it here (the files stay)
  chronodrive share <drive> <email> [--read]   share your drive with another account (can edit, or read only)
  chronodrive unshare <drive> <email>          stop sharing it with that account
  chronodrive leave <drive>         leave a drive somebody shared with you (unmounted here; the files stay)
  chronodrive status                account, service, every mounted drive (state, last sync, what is not synced)
  chronodrive sync [--once] [--seconds N] [drive]   sync in the foreground (until Ctrl+C, N seconds, or one pass)
  chronodrive unlink                forget the account on this PC (the files stay)
  chronodrive update                git pull in the checkout it was installed from, and reinstall
  chronodrive log [n]               the last n lines of the log
  chronodrive service               what chronodrive.service runs: every mounted drive, in the background"""
import concurrent.futures
import os
import shutil
import signal
import subprocess
import sys
import threading
import time

from . import __version__, common, net, qr
from .common import Config, human, log, short_home
from .net import HttpError, Miniserver, Revoked
from .sync import Busy, Engine

APP_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def out(s=""):
    print(s, flush=True)


def opt(args, name, default=None):
    if name in args:
        i = args.index(name)
        if i + 1 < len(args):
            v = args[i + 1]
            del args[i:i + 2]
            return v
        del args[i]
    return default


def flag(args, name):
    if name in args:
        args.remove(name)
        return True
    return False


def table(rows, header):
    w = [max(len(str(r[i])) for r in [header] + rows) for i in range(len(header))]
    for r in [header] + rows:
        out("  ".join(str(c).ljust(w[i]) for i, c in enumerate(r)).rstrip())


class Fail(Exception):
    pass


def linked_ms(cfg):
    secret = common.load_secret()
    if not (cfg.device_id and secret):
        raise Fail("this PC is not paired: chronodrive pair")
    return Miniserver(cfg, secret)


def forget_device(cfg):
    common.save_secret("")
    cfg.device_id = ""
    cfg.email = ""
    cfg.save()


def guard(fn):
    """Runs a command; readable errors, and a revoked device forgets its credentials (API.md section 2)."""
    def run(cfg, args):
        try:
            return fn(cfg, args)
        except Revoked:
            forget_device(cfg)
            out("This PC was unlinked on the Miniserver. Pair it again: chronodrive pair")
            return 1
        except HttpError as e:
            out(net.describe_error(e))
            return 1
        except Fail as e:
            out(str(e))
            return 1
    return run


# ------------------------------------------------------------------ service helpers
def systemctl(*a):
    if not shutil.which("systemctl"):
        return None
    try:
        r = subprocess.run(["systemctl", "--user", *a], capture_output=True, text=True, timeout=15)
        return r.stdout.strip() or r.stderr.strip()
    except (OSError, subprocess.SubprocessError):
        return None


def service_state():
    s = systemctl("is-active", "chronodrive")
    if s is None:
        return "no systemd user session"
    if s == "active":
        return "running"
    en = systemctl("is-enabled", "chronodrive") or ""
    if "not-found" in en or "No such file" in en or not en:
        return "not installed"
    return f"stopped ({s})"


def kick_service():
    p = common.kick_path()
    with open(p, "a"):
        pass
    os.utime(p)


def find_drive(drives, key):
    """By id, else by name (case-insensitive), else by a unique prefix of either."""
    k = key.lower()
    for d in drives:
        if d["id"] == key:
            return d
    hits = [d for d in drives if d.get("name", "").lower() == k]
    if len(hits) == 1:
        return hits[0]
    hits = [d for d in drives if d["id"].startswith(key) or d.get("name", "").lower().startswith(k)]
    if len(hits) == 1:
        return hits[0]
    raise Fail(f"no drive '{key}'" if not hits else f"'{key}' matches {len(hits)} drives: " + ", ".join(d.get("name", d["id"]) for d in hits))


# ------------------------------------------------------------------ commands
@guard
def cmd_pair(cfg, args):
    url = (opt(args, "--url") or cfg.url).rstrip("/")
    name = opt(args, "--name") or cfg.device_name or common.host_name()
    no_qr = flag(args, "--no-qr")
    if cfg.device_id and common.load_secret():
        out(f"Already linked to {cfg.email or cfg.device_id}: pairing again replaces it.")
    r = Miniserver.link_start(url, name)
    if not r.ok:
        out(f"Cannot start pairing at {url}: {r.describe()}")
        return 1
    j = r.json() or {}
    code, link = j.get("code", ""), j.get("link_url", "")
    out(f"Pairing code {code}  (approve it at {link})")
    if not no_qr and sys.stdout.isatty():
        out()
        for line in qr.render(j.get("qr_payload") or link):
            out("  " + line)
        out()
        out("Scan it with your phone, or open the link in a signed-in browser, and press Approve.")
    out("Waiting for approval... (Ctrl+C to cancel)")
    every = max(1, int(j.get("poll_interval_seconds") or 2))
    end = time.time() + 600
    try:
        while time.time() < end:
            time.sleep(every)
            p = Miniserver.link_poll(url, code, j.get("secret", ""))
            if p.status == 202:
                continue
            if p.status == 200:
                a = p.json() or {}
                cfg.url, cfg.device_id, cfg.device_name = url, a.get("device_id", ""), name
                cfg.email = (a.get("record") or {}).get("email", "")
                common.save_secret(a.get("device_secret", ""))
                if not cfg.email:   # PocketBase hides the email in the pairing answer: ask for our own record
                    uid = (a.get("record") or {}).get("id", "")
                    rr = net.request_json("GET", f"{url}/api/collections/users/records/{uid}", headers={"Authorization": a.get("token", "")})
                    cfg.email = (rr.json() or {}).get("email", "") if rr.ok else ""
                cfg.save()
                net.refresh_servers(url, force=True)
                out(f"Linked to {cfg.email or 'the account'} (device {cfg.device_id}).")
                out("Next: chronodrive drives, then chronodrive mount <drive>")
                kick_service()
                return 0
            out(f"Pairing {'expired' if p.status == 410 else 'failed'}: {p.describe()}")
            return 1
    except KeyboardInterrupt:
        out("\ncancelled")
        return 1
    out("Pairing timed out.")
    return 1


@guard
def cmd_unlink(cfg, args):
    for d in cfg.drives:
        forget_drive_state(d["id"])
    n = len(cfg.drives)
    cfg.drives = []
    forget_device(cfg)
    kick_service()
    out("Unlinked." + (f" {n} drive(s) no longer synced here; the files stay in their folders." if n else ""))
    return 0


def forget_drive_state(drive_id):
    for p in (common.index_path(drive_id), common.status_path(drive_id)):
        try:
            os.remove(p)
        except OSError:
            pass


def reachable(ms, cfg, d):
    try:
        _, cands = net.grant_for(ms, d["id"], common.device_tag(cfg.device_name or common.host_name()))
        dc, _ = net.reach(cands, timeout=4)
        if dc:
            return "yes"
    except HttpError as e:
        return f"no ({e.status})"
    return "no (home drive: its network only)" if d.get("node_kind") == "home" else "no"


@guard
def cmd_drives(cfg, args):
    ms = linked_ms(cfg)
    drives = ms.json("GET", "/api/ms/drive/drives").get("drives", [])
    if not drives:
        out(f"No drives in {cfg.email or 'this account'} yet. Create one on the web: {cfg.url}/chronodrive/")
        return 0
    with concurrent.futures.ThreadPoolExecutor(8) as ex:
        reach = list(ex.map(lambda d: reachable(ms, cfg, d), drives))
    rows = []
    for d, rch in zip(drives, reach):
        m = cfg.find(d["id"])
        state = "deleting" if d.get("deleting") else ("disabled" if d.get("enabled") is False else "")
        rows.append([d.get("name", ""), f"{d.get('size_gb', 0)} GB", human(d.get("used", 0)), f"{d.get('node', '')} ({d.get('node_kind', '')})",
                     state or rch, short_home(m["folder"]) if m else "-", sharing(d), d["id"]])
    table(rows, ["NAME", "SIZE", "USED", "NODE", "REACHABLE", "MOUNTED AT", "SHARING", "ID"])
    return 0


def sharing(d):
    """'shared by x (read only)' for a drive shared with this account, 'with a, b (read)' for an own shared one."""
    if d.get("owner") is False:
        return f"shared by {d.get('owner_email', '?')}" + (" (read only)" if d.get("role") == "read" else " (can edit)")
    ms = d.get("members") or []
    return ("with " + ", ".join(m.get("email", "?") + (" (read)" if m.get("role") == "read" else "") for m in ms)) if ms else "-"


def owned_drive(ms, key):
    d = find_drive(ms.json("GET", "/api/ms/drive/drives").get("drives", []), key)
    if d.get("owner") is False:
        raise Fail(f"{d['name']} is {d.get('owner_email', 'somebody else')}'s: only the owner shares it")
    return d


@guard
def cmd_share(cfg, args):
    read = flag(args, "--read")
    if len(args) < 2:
        raise Fail("usage: chronodrive share <drive> <email> [--read]")
    ms = linked_ms(cfg)
    d = owned_drive(ms, args[0])
    m = ms.json("POST", f"/api/ms/drive/drives/{d['id']}/members", {"email": args[1], "role": "read" if read else "write"})
    out(f"{d['name']} shared with {m.get('email', args[1])}" + (" (read only)." if m.get("role") == "read" else " (can edit)."))
    return 0


@guard
def cmd_unshare(cfg, args):
    if len(args) < 2:
        raise Fail("usage: chronodrive unshare <drive> <email>")
    ms = linked_ms(cfg)
    d = owned_drive(ms, args[0])
    members = ms.json("GET", f"/api/ms/drive/drives/{d['id']}/members").get("members", [])
    who = [m for m in members if m.get("email", "").lower() == args[1].lower() or m.get("user") == args[1]]
    if not who:
        raise Fail(f"{d['name']} is not shared with {args[1]}")
    ms.json("DELETE", f"/api/ms/drive/drives/{d['id']}/members/{who[0]['user']}")
    out(f"{d['name']} is no longer shared with {who[0].get('email', args[1])}.")
    return 0


@guard
def cmd_leave(cfg, args):
    if not args:
        raise Fail("usage: chronodrive leave <drive>")
    ms = linked_ms(cfg)
    d = find_drive(ms.json("GET", "/api/ms/drive/drives").get("drives", []), args[0])
    if d.get("owner") is not False:
        raise Fail(f"{d['name']} is yours: delete it on the web, or unmount it here")
    ms.json("DELETE", f"/api/ms/drive/drives/{d['id']}/members/me")
    m = cfg.find(d["id"])
    if m:
        cfg.drives = [x for x in cfg.drives if x["id"] != d["id"]]
        cfg.save()
        forget_drive_state(d["id"])
        kick_service()
    out(f"You left {d['name']} ({d.get('owner_email', '')})." + (f" It is no longer synced here; the files stay in {short_home(m['folder'])}." if m else ""))
    return 0


@guard
def cmd_mount(cfg, args):
    if not args:
        raise Fail("usage: chronodrive mount <drive> [folder]")
    ms = linked_ms(cfg)
    d = find_drive(ms.json("GET", "/api/ms/drive/drives").get("drives", []), args[0])
    if d.get("deleting"):
        raise Fail(f"{d['name']} is being deleted")
    folder = os.path.abspath(os.path.expanduser(args[1])) if len(args) > 1 else \
        os.path.join(os.path.expanduser("~/ChronoDrive"), common.safe_folder_name(d.get("name", d["id"])))
    old = cfg.find(d["id"])
    if old:
        if old["folder"] == folder:
            out(f"{d['name']} is already mounted at {short_home(folder)}.")
            return 0
        raise Fail(f"{d['name']} is mounted at {short_home(old['folder'])}: chronodrive unmount {d['name']} first")
    for o in cfg.drives:
        a, b = o["folder"].rstrip("/") + "/", folder.rstrip("/") + "/"
        if a.startswith(b) or b.startswith(a):
            raise Fail(f"{short_home(folder)} overlaps {o['name']} at {short_home(o['folder'])}")
    for p in (common.config_dir(), common.state_dir()):
        if (p + "/").startswith(folder.rstrip("/") + "/") or folder.startswith(p + "/"):
            raise Fail("pick a folder outside ChronoDrive's own config and state")
    if os.path.exists(folder) and not os.path.isdir(folder):
        raise Fail(f"{short_home(folder)} is a file")
    busy = os.path.isdir(folder) and any(True for _ in os.scandir(folder))
    os.makedirs(folder, exist_ok=True)
    entry = {"id": d["id"], "name": d.get("name", ""), "node": d.get("node", ""), "node_kind": d.get("node_kind", ""), "folder": folder}
    if d.get("owner") is False:
        entry.update(owner_email=d.get("owner_email", ""), role=d.get("role", "write"))
    cfg.drives.append(entry)
    cfg.save()
    out(f"Mounted {d['name']} at {short_home(folder)}.")
    if d.get("owner") is False and d.get("role") == "read":
        out(f"It is shared with you to read only ({d.get('owner_email', '')}): changes made here stay on this PC and are not uploaded.")
    if busy:
        out("The folder is not empty: its files are merged with the drive (a different file with the same name is kept as a conflict copy).")
    if service_state() == "running":
        kick_service()
        out("The background service syncs it now (chronodrive status).")
    else:
        out("The background service is not running: chronodrive sync (or ./linux/install.sh to install it).")
    return 0


@guard
def cmd_unmount(cfg, args):
    if not args:
        raise Fail("usage: chronodrive unmount <drive>")
    d = find_drive(cfg.drives, args[0])
    cfg.drives = [x for x in cfg.drives if x["id"] != d["id"]]
    cfg.save()
    forget_drive_state(d["id"])
    kick_service()
    out(f"{d['name']} is no longer synced here; the files stay in {short_home(d['folder'])}.")
    return 0


def drive_status_lines(d, verbose=True):
    st = common.read_json(common.status_path(d["id"]), {}) or {}
    idx = common.read_json(common.index_path(d["id"]), {}) or {}
    files = [e for e in (idx.get("files") or {}).values() if not e.get("dir")]
    live = common.pid_alive(st.get("pid")) and st.get("pid") != os.getpid()
    state = st.get("state", "")
    if not st:
        what = "not synced yet"
    elif live:
        what = st.get("detail") or state
        if st.get("pending"):
            what += f" ({st['pending']} to do" + (f", {human(st.get('done'))} of {human(st.get('total'))}" if st.get("total") else "") + ")"
    elif state == "idle":
        what = "not running (last sync up to date)"
    else:
        what = f"not running (last: {st.get('detail') or state})"
    tags = ([f"shared by {d['owner_email']}"] if d.get("owner_email") else []) + (["read only"] if st.get("read_only", d.get("role") == "read") else [])
    lines = [f"{d['name']}  ->  {short_home(d['folder'])}" + (f"  ({', '.join(tags)})" if tags else ""),
             f"  {what}  |  last sync {common.ago(st.get('last_sync'))}  |  {len(files)} files, {human(sum(e.get('size', 0) for e in files))}, rev {idx.get('rev', 0)}"]
    if st.get("node_url") and verbose:
        lines.append(f"  node {st['node_url']}")
    sk = st.get("skipped") or []
    if sk:
        lines.append(f"  not synced ({st.get('skipped_count', len(sk))}):")
        lines += [f"    {s['path']}  ({s['why']})" for s in sk[:10]]
    if verbose and st.get("recent"):
        lines.append("  recent:")
        for r in st["recent"][:5]:
            lines.append(f"    {common.time_local(r['at'])}  {r['what']}  {r['path']}" + (f"  ({r['by']})" if r.get("by") else ""))
    return lines


def cmd_status(cfg, args):
    out(f"server    {cfg.url}")
    out("account   " + (f"{cfg.email or '?'}  (device {cfg.device_name or common.host_name()}, {cfg.device_id})" if cfg.device_id and common.load_secret() else "not paired: chronodrive pair"))
    out(f"service   {service_state()}")
    out(f"version   {__version__}")
    if not cfg.drives:
        out("drives    none mounted here: chronodrive drives, chronodrive mount <drive>")
    for d in cfg.drives:
        out()
        for line in drive_status_lines(d):
            out(line)
    return 0


@guard
def cmd_sync(cfg, args):
    once = flag(args, "--once")
    secs = int(opt(args, "--seconds", "0") or 0)
    secret = linked_ms(cfg).secret
    drives = [find_drive(cfg.drives, args[0])] if args else list(cfg.drives)
    if not drives:
        raise Fail("no drive mounted here: chronodrive mount <drive> [folder]")
    if once:
        rc = 0
        for d in drives:
            e = Engine(cfg, secret, d)
            try:
                err = e.run_once()
            except Busy:
                kick_service()
                out(f"{d['name']}: synced by the background service (asked it to sync now; chronodrive status)")
                continue
            for r in reversed(e.recent):
                out(f"  {common.time_local(r['at'])}  {r['what']}  {r['path']}" + (f"  ({r['by']})" if r.get("by") else ""))
            for p, why in e.skipped:
                out(f"  not synced  {p}  ({why})")
            out(f"{d['name']}: " + (e.idle_text() if not err else err))
            rc = rc or (1 if err else 0)
        return rc
    engines = []
    for d in drives:
        e = Engine(cfg, secret, d)
        try:
            e.start()
            engines.append(e)
        except Busy:
            kick_service()
            out(f"{d['name']}: synced by the background service (asked it to sync now; chronodrive status)")
    if not engines:
        return 0
    stop = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    t0, last, err = time.time(), {}, False
    try:
        while not stop.is_set() and (not secs or time.time() - t0 < secs):
            stop.wait(2)
            for e in engines:
                st = e.st
                line = f"{st['state']}  {st['detail']}" + (f"  pending {st['pending']}" if st["pending"] else "") + \
                    (f"  {human(st['done'])}/{human(st['total'])}" if st["total"] else "") + (f"  @ {st['node_url']}" if st["node_url"] else "")
                if line != last.get(e.id):
                    out(f"{common.time_local(time.time())}  " + (f"[{e.name}] " if len(engines) > 1 else "") + line)
                    last[e.id] = line
                if st["state"] == "error":
                    err = True
            if err:
                break
    except KeyboardInterrupt:
        pass
    for e in engines:
        e.stop()
        for r in reversed(e.recent):
            out(f"  {common.time_local(r['at'])}  " + (f"[{e.name}] " if len(engines) > 1 else "") + f"{r['what']}  {r['path']}" + (f"  ({r['by']})" if r.get("by") else ""))
        for p, why in e.skipped:
            out(f"  not synced  {p}  ({why})")
    return 1 if err else 0


def cmd_log(cfg, args):
    for line in common.log_tail(int(args[0]) if args else 40):
        out(line)
    return 0


def checkout_dir():
    """The git checkout this copy was installed from (install.sh writes SOURCE), or the one it runs from."""
    try:
        with open(os.path.join(APP_DIR, "SOURCE"), encoding="utf-8") as f:
            src = f.read().strip()
            if src:
                return src
    except OSError:
        pass
    parent = os.path.dirname(APP_DIR)
    return parent if os.path.isdir(os.path.join(parent, ".git")) else ""


def cmd_update(cfg, args):
    src = checkout_dir()
    if not src or not os.path.isdir(os.path.join(src, ".git")):
        out("No git checkout to update from" + (f" ({src} is gone)" if src else "") + ": clone the repository again and run ./linux/install.sh")
        return 1
    before = __version__
    head = subprocess.run(["git", "-C", src, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    out(f"git pull in {short_home(src)}")
    r = subprocess.run(["git", "-C", src, "pull", "--ff-only"], capture_output=True, text=True)
    out((r.stdout + r.stderr).strip())
    if r.returncode:
        out(f"Update failed: git pull did not work in {short_home(src)} (see above).")
        return 1
    now = subprocess.run(["git", "-C", src, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    if now == head and not flag(args, "--force"):
        out(f"Already up to date ({before}).")
        return 0
    r = subprocess.run(["bash", os.path.join(src, "linux", "install.sh"), "--update"])
    if r.returncode:
        out("Reinstall failed (see above).")
        return 1
    after = subprocess.run([sys.executable, os.path.join(APP_DIR, "chronodrive"), "--version"], capture_output=True, text=True).stdout.strip()
    out(f"Updated: {before} -> {after or '?'}")
    return 0


# ------------------------------------------------------------------ the background service
def cmd_service(cfg, args):
    common.LOG_TO_STDOUT = True
    stop = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    log(f"service {__version__} started (pid {os.getpid()})")
    engines, wanted_key, cfg_m, kick_m, servers_at, busy_at = {}, None, -1.0, -1.0, 0.0, 0.0

    def mtime(p):
        try:
            return os.stat(p).st_mtime
        except OSError:
            return 0.0

    while not stop.is_set():
        m, k = mtime(common.config_path()), mtime(common.kick_path())
        if m != cfg_m or (busy_at and time.time() - busy_at > 30):
            cfg_m, busy_at = m, 0.0
            cfg = Config.load()
            secret = common.load_secret()
            linked = bool(cfg.device_id and secret)
            key = (cfg.url, cfg.device_id, secret)
            want = {d["id"]: d for d in cfg.drives} if linked else {}
            for did in list(engines):
                e = engines[did]
                if did not in want or want[did]["folder"] != e.root or key != wanted_key:
                    log(f"[{e.name}] stopped" + (" (unmounted)" if did not in want else ""))
                    e.stop()
                    del engines[did]
                    if did not in want:
                        forget_drive_state(did)
            wanted_key = key
            for did, d in want.items():
                if did in engines:
                    continue
                e = Engine(cfg, secret, d)
                try:
                    e.start()
                    engines[did] = e
                    log(f"[{e.name}] syncing {d['folder']}")
                except Busy as b:
                    log(str(b) + ": retried in 30 s")
                    busy_at = time.time()
                except OSError as x:
                    log(f"[{d.get('name')}] cannot start: {x}")
                    busy_at = time.time()
            if not linked:
                log("not paired yet: waiting (chronodrive pair)")
        if k != kick_m:
            if kick_m >= 0:
                for e in engines.values():
                    e.kick()
            kick_m = k
        if linked and time.time() - servers_at > 86400:
            servers_at = time.time()
            threading.Thread(target=net.refresh_servers, args=(cfg.url,), daemon=True).start()
        stop.wait(2)
    for e in engines.values():
        e.stop()
    log("service stopped")
    return 0


# ------------------------------------------------------------------ the menu
def ask(prompt, default=""):
    try:
        v = input(f"{prompt}" + (f" [{default}]" if default else "") + ": ").strip()
    except EOFError:
        return default
    return v or default


def menu(cfg):
    while True:
        cfg = Config.load()
        linked = bool(cfg.device_id and common.load_secret())
        out()
        out(f"ChronoDrive {__version__}  " + (f"{cfg.email}  ({len(cfg.drives)} mounted)" if linked else "not paired"))
        items = [("Status", lambda: cmd_status(cfg, [])), ("Drives of the account", lambda: cmd_drives(cfg, [])),
                 ("Mount a drive", lambda: menu_mount(cfg)), ("Unmount a drive", lambda: menu_unmount(cfg)),
                 ("Sync now", lambda: cmd_sync(cfg, ["--once"])), ("Log", lambda: cmd_log(cfg, ["20"])),
                 ("Unlink this PC" if linked else "Pair this PC", (lambda: cmd_unlink(cfg, [])) if linked else (lambda: cmd_pair(cfg, []))),
                 ("Update", lambda: cmd_update(cfg, []))]
        for i, (t, _) in enumerate(items, 1):
            out(f"  {i}  {t}")
        out("  0  Quit")
        c = ask("Choice")
        if c in ("0", "q", ""):
            return 0
        if c.isdigit() and 1 <= int(c) <= len(items):
            out()
            if items[int(c) - 1][0] == "Unlink this PC" and ask("Unlink this PC? (y/N)").lower() != "y":
                continue
            try:
                items[int(c) - 1][1]()
            except KeyboardInterrupt:
                out()


@guard
def menu_mount(cfg):
    ms = linked_ms(cfg)
    drives = [d for d in ms.json("GET", "/api/ms/drive/drives").get("drives", []) if not cfg.find(d["id"]) and not d.get("deleting")]
    if not drives:
        out(f"No drive left to mount. Create one on the web: {cfg.url}/chronodrive/")
        return 0
    for i, d in enumerate(drives, 1):
        out(f"  {i}  {d.get('name')}  ({d.get('size_gb')} GB on {d.get('node')})")
    c = ask("Drive")
    if not c.isdigit() or not 1 <= int(c) <= len(drives):
        return 0
    d = drives[int(c) - 1]
    folder = ask("Folder", short_home(os.path.join(os.path.expanduser("~/ChronoDrive"), common.safe_folder_name(d.get("name", d["id"])))))
    return cmd_mount(cfg, [d["id"], folder])


def menu_unmount(cfg):
    if not cfg.drives:
        out("Nothing mounted here.")
        return 0
    for i, d in enumerate(cfg.drives, 1):
        out(f"  {i}  {d['name']}  ->  {short_home(d['folder'])}")
    c = ask("Drive")
    if c.isdigit() and 1 <= int(c) <= len(cfg.drives):
        return cmd_unmount(cfg, [cfg.drives[int(c) - 1]["id"]])
    return 0


COMMANDS = {"pair": cmd_pair, "unlink": cmd_unlink, "drives": cmd_drives, "mount": cmd_mount, "unmount": cmd_unmount,
            "share": cmd_share, "unshare": cmd_unshare, "leave": cmd_leave,
            "status": cmd_status, "sync": cmd_sync, "log": cmd_log, "update": cmd_update, "service": cmd_service}


def main(argv=None):
    args = list(sys.argv[1:] if argv is None else argv)
    if args and args[0] in ("--version", "version"):
        out(__version__)
        return 0
    cfg = Config.load()
    if not args:
        if sys.stdin.isatty() and sys.stdout.isatty():
            return menu(cfg)
        return cmd_status(cfg, [])
    cmd = args.pop(0)
    if cmd in ("-h", "--help", "help") or cmd not in COMMANDS:
        out(__doc__.split("\n", 2)[2].rstrip())
        return 0 if cmd in ("-h", "--help", "help") else 2
    try:
        return COMMANDS[cmd](cfg, args) or 0
    except KeyboardInterrupt:
        out()
        return 130

"""The sync engine: one local folder <-> one drive on a Miniserver node (API.md section 17). Same rules as the Windows
client (sync.hpp), so both can share a drive:
  index-<drive>.json  what this PC last saw of every path: size, mtime, sha256, the drive's rev (the base rev of the next write)
  remote pass         the drive's change feed since the index rev: downloads, folders, deletes (to the trash), conflicts
  local pass          the folder against the index: uploads (small put or chunked), deletes, folders
A remote change over an untouched local file wins; a local change over an untouched remote file wins; both changed =
the remote version keeps the name and the local one is kept (and uploaded) as "name (conflict PC date).ext".
Nothing is lost silently."""
import collections
import ctypes
import ctypes.util
import errno
import fcntl
import hashlib
import json
import os
import select
import struct
import threading
import time

from . import common, local
from .common import log
from .net import HttpError, Miniserver, Revoked, grant_for, reach

FULL_SCAN = 600          # a full local scan every 10 minutes even when the watcher is quiet
FULL_SCAN_NO_WATCH = 30  # without inotify
QUIET = 2                # seconds of quiet after the last local event before a pass
FRESH = 2                # a file written less than this ago is left for the next pass (it may still be written)


class Stopped(Exception):
    pass


class Fatal(Exception):
    """Nothing to retry soon: the PC was unlinked, the drive is disabled or gone."""


class Busy(Exception):
    """Another process (the background service) syncs this drive."""


def sha256_file(path):
    h, n = hashlib.sha256(), 0
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
            n += len(b)
    return h.hexdigest(), n


# ------------------------------------------------------------------ the index
class Index:
    def __init__(self, drive_id):
        self.path = common.index_path(drive_id)
        self.rev = 0
        self.files = {}      # local path ("a/b.txt", as named on this PC) -> {dir, size, mtime, sha256, rev}
        self.uploads = {}    # path -> {id, sha256, size}: a chunked upload in progress (resumed after a restart)
        self._fold = {}

    def load(self):
        j = common.read_json(self.path)
        if not isinstance(j, dict):
            return False
        self.rev = int(j.get("rev", 0))
        self.files = {p: dict(e) for p, e in (j.get("files") or {}).items()}
        self.uploads = {p: u for p, u in (j.get("uploads") or {}).items() if isinstance(u, dict)}
        self._fold = {local.fold(p): p for p in self.files}
        return True

    def save(self):
        common.write_atomic(self.path, json.dumps({"rev": self.rev, "files": self.files, "uploads": self.uploads}, separators=(",", ":"), ensure_ascii=False))

    def find(self, drive_path):
        """The index key of a drive path (the drive compares case-insensitively)."""
        return drive_path if drive_path in self.files else self._fold.get(local.fold(drive_path))

    def set(self, p, **e):
        old = self._fold.get(local.fold(p))
        if old is not None and old != p:
            self.files.pop(old, None)
        self.files[p] = {"dir": e.get("dir", False), "size": e.get("size", 0), "mtime": e.get("mtime", 0), "sha256": e.get("sha256", ""), "rev": e.get("rev", 0)}
        self._fold[local.fold(p)] = p

    def drop(self, p):
        self.files.pop(p, None)
        self._fold.pop(local.fold(p), None)
        self.uploads.pop(p, None)

    def under(self, p):
        return [q for q in self.files if local.under(q, p)]


# ------------------------------------------------------------------ inotify (ctypes; a periodic scan when it is not there)
class Watcher:
    IN_ATTRIB, IN_CLOSE_WRITE, IN_MOVED_FROM, IN_MOVED_TO, IN_CREATE, IN_DELETE = 0x4, 0x8, 0x40, 0x80, 0x100, 0x200
    IN_DELETE_SELF, IN_MOVE_SELF, IN_Q_OVERFLOW, IN_IGNORED = 0x400, 0x800, 0x4000, 0x8000
    MASK = IN_ATTRIB | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF

    def __init__(self):
        self.fd, self.wds, self.full = -1, {}, False
        try:
            self.libc = ctypes.CDLL(ctypes.util.find_library("c") or "libc.so.6", use_errno=True)
            fd = self.libc.inotify_init1(os.O_NONBLOCK | os.O_CLOEXEC)
            if fd >= 0:
                self.fd = fd
        except (OSError, AttributeError):
            pass

    @property
    def ok(self):
        return self.fd >= 0 and not self.full

    def add(self, path):
        """Called for every folder after each local pass: the kernel keeps one watch per inode, so this is cheap."""
        if self.fd < 0 or self.full:
            return
        wd = self.libc.inotify_add_watch(self.fd, os.fsencode(path), self.MASK)
        if wd < 0:
            if ctypes.get_errno() == errno.ENOSPC and not self.full:
                self.full = True
                log("inotify: out of watches (fs.inotify.max_user_watches): a full scan every 30 s instead")
            return
        self.wds[wd] = path

    def wait(self, timeout):
        """True when something other than our own temporary files changed."""
        if self.fd < 0:
            time.sleep(timeout)
            return False
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return False
        try:
            buf = os.read(self.fd, 64 * 1024)
        except BlockingIOError:
            return False
        real, i = False, 0
        while i + 16 <= len(buf):
            wd, mask, _cookie, n = struct.unpack_from("iIII", buf, i)
            name = buf[i + 16:i + 16 + n].rstrip(b"\0").decode("utf-8", "replace")
            i += 16 + n
            if mask & self.IN_IGNORED:
                self.wds.pop(wd, None)
                continue
            if mask & self.IN_Q_OVERFLOW or not name or not local.ignored(name):
                real = True
        return real

    def close(self):
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1


# ------------------------------------------------------------------ the engine
class Engine:
    def __init__(self, cfg, secret, drive):
        self.cfg, self.drive = cfg, drive
        self.id, self.name, self.root = drive["id"], drive.get("name", drive["id"]), drive["folder"]
        self.pc = common.host_name()
        self.device = common.device_tag(cfg.device_name or self.pc)
        self.ms = Miniserver(cfg, secret)
        self.idx = Index(self.id)
        self.dc, self.cands, self.grant_exp, self.url_check = None, [], 0, 0
        self.running = False
        self.wake = threading.Event()
        self.dirty, self.dirty_at, self.again = True, 0.0, False
        self.threads, self.watcher, self.lockf = [], None, None
        self.recent = collections.deque(maxlen=20)
        self.skipped, self.status_at = [], 0.0
        self.st = {"state": "setup", "detail": "", "node_url": "", "pending": 0, "done": 0, "total": 0, "last_sync": 0,
                   "read_only": False, "local_only": 0}

    # ---- status (status-<drive>.json: what `chronodrive status` shows while the service runs)
    def set_state(self, state, detail=""):
        changed = (state, detail) != (self.st["state"], self.st["detail"])
        self.st.update(state=state, detail=detail)
        if changed:
            self.write_status(force=True)

    def progress(self, pending, done=None, total=None):
        self.st["pending"] = pending
        if done is not None:
            self.st["done"] = done
        if total is not None:
            self.st["total"] = total
        self.write_status()

    def note(self, what, path, by=""):
        self.recent.appendleft({"at": time.time(), "what": what, "path": path, "by": by})
        self.write_status(force=True)

    def write_status(self, force=False):
        if not force and time.time() - self.status_at < 1:
            return
        self.status_at = time.time()
        j = dict(self.st, pid=os.getpid(), drive=self.id, name=self.name, folder=self.root, rev=self.idx.rev,
                 recent=list(self.recent), skipped=[{"path": p, "why": w} for p, w in self.skipped[:50]], skipped_count=len(self.skipped))
        try:
            common.write_atomic(common.status_path(self.id), json.dumps(j, ensure_ascii=False))
        except OSError:
            pass

    # ---- one process per drive
    def lock(self):
        f = open(common.lock_path(self.id), "a")
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            f.close()
            raise Busy(f"{self.name} is synced by another process (the background service?)")
        self.lockf = f

    def unlock(self):
        if self.lockf:
            self.lockf.close()
            self.lockf = None

    def prepare(self):
        self.lock()
        loaded = self.idx.load()
        log(f"[{self.name}] index {'loaded' if loaded else 'not found (fresh start)'}: rev {self.idx.rev}, {len(self.idx.files)} entries")
        os.makedirs(self.root, exist_ok=True)

    # ---- grant and the node's url (every 10 minutes, and whenever a call fails)
    def connect(self):
        now = time.time()
        if not self.cands or now > self.grant_exp - 3600 or now - self.url_check > 600:
            try:
                g, cands = grant_for(self.ms, self.id, self.device)
                self.cands, self.grant_exp, self.url_check = cands, int(g.get("exp") or 0), now
                # a read-only member's grant has six parts, <id>.<user>.<device>.<exp>.r.<sig> (API.md section 17 "Sharing")
                ro = g.get("grant", "").count(".") == 5
                if ro != self.st["read_only"]:
                    log(f"[{self.name}] " + ("shared with this account to read only: nothing goes up" if ro else "writable"))
                self.st["read_only"] = ro
            except Revoked:
                raise Fatal("This PC was unlinked on the Miniserver: run 'chronodrive pair' again.")
            except HttpError as e:
                if e.status in (403, 404):
                    raise Fatal(f"The drive is disabled or gone: {e}")
                if not (self.cands and now < self.grant_exp - 120):
                    raise
                # the Miniserver is down (or only its read-only standby answers): the node checks the grant alone,
                # so keep syncing on the one we have until it expires, and ask again in a minute
                log(f"[{self.name}] grant not renewed ({e}): the current one is valid until {time.strftime('%H:%M', time.localtime(self.grant_exp))}")
                self.url_check = now - 540
        try:
            dc, info = reach(self.cands)
        except HttpError as e:
            raise Fatal(f"The drive refuses this PC: {e}")
        if dc is None:
            self.url_check = 0   # ask the primary again next time: the node's address may have changed
            raise OSError(info or "the drive's node does not answer (a home drive syncs only on its network)")
        if not self.dc or self.dc.base != dc.base:
            log(f"[{self.name}] drive at {dc.base}")
        self.dc = dc
        self.st["node_url"] = dc.base

    # ---- remote pass: the change feed since the index rev
    def pull_remote(self):
        while True:
            r = self.dc.changes(self.idx.rev, 0)
            changes = r.get("changes") or []
            if not changes:
                self.idx.rev = max(self.idx.rev, int(r.get("rev", self.idx.rev)))
                return
            log(f"[{self.name}] changes since {self.idx.rev}: {len(changes)} (up to rev {changes[-1]['rev']})")
            self.progress(len(changes), 0, 0)
            for n, e in enumerate(changes):
                if not self.running:
                    raise Stopped()
                self.apply(e)
                self.idx.rev = max(self.idx.rev, int(e["rev"]))
                self.progress(len(changes) - n - 1)
            self.idx.save()
            if not r.get("more"):
                return

    def unchanged(self, ie, lf):
        if lf is None:
            return True   # gone = nothing of ours to protect
        if ie["dir"]:
            return lf.dir
        return not lf.dir and lf.size == ie["size"] and lf.mtime // 1000 == ie["mtime"] // 1000

    def apply(self, e):
        path = e["path"]
        if local.ignored(path.rsplit("/", 1)[-1]):
            return
        key = self.idx.find(path) or local.resolve(self.root, path)
        ie = self.idx.files.get(key)
        if ie and int(e["rev"]) <= ie["rev"]:
            return   # ours, or already seen
        full = os.path.join(self.root, key)
        lf = local.stat_local(self.root, key)
        if e.get("deleted"):
            if not ie:
                return   # never had it
            if ie["dir"]:
                self.remote_rmdir(key, e.get("by", ""))
            elif lf and self.unchanged(ie, lf):
                local.trash(full)
                self.note(f"deleted on {e.get('by') or 'the drive'}", key, e.get("by", ""))
            elif lf:
                log(f"[{self.name}] kept local changes of {key} (deleted on the drive): it goes up again")
            for q in self.idx.under(key):
                self.idx.drop(q)
            return
        if e.get("kind") == "dir":
            if lf and not lf.dir:
                log(f"[{self.name}] {key} is a folder on the drive and a file here: left as it is")
                return
            os.makedirs(full, exist_ok=True)
            self.idx.set(key, dir=True, rev=int(e["rev"]))
            return
        if lf and lf.dir:
            log(f"[{self.name}] {key} is a file on the drive and a folder here: left as it is")
            return
        if lf and (ie is None or not self.unchanged(ie, lf)):
            # changed here too (or unknown to the index): the same content is adopted, else ours becomes a conflict copy
            try:
                sha, _ = sha256_file(full)
            except OSError:
                sha = ""
            if sha == e.get("sha256"):
                self.idx.set(key, size=int(e["size"]), mtime=int(e["mtime"]), sha256=sha, rev=int(e["rev"]))
                local.set_mtime(full, e["mtime"])
                return
            cname = local.conflict_name(key, self.pc)
            os.replace(full, os.path.join(self.root, cname))
            log(f"[{self.name}] conflict on {key}: local copy kept as {cname}")
            self.note("conflict, local copy kept", cname, e.get("by", ""))
            self.again = True
        self.download(e, key)

    def download(self, e, key):
        full = os.path.join(self.root, key)
        part = full + ".part"
        os.makedirs(os.path.dirname(full), exist_ok=True)
        try:
            have = os.path.getsize(part)
        except OSError:
            have = 0
        try:
            self.dc.download(e["path"], part, have)
        except HttpError as x:
            if x.status == 404:
                log(f"[{self.name}] download {key}: gone meanwhile")
                return
            raise
        sha, size = sha256_file(part)
        if size != int(e["size"]) or sha != e.get("sha256"):
            os.remove(part)
            raise OSError(f"download {key}: hash mismatch, retried later")
        os.replace(part, full)
        local.set_mtime(full, e["mtime"])
        self.idx.set(key, size=size, mtime=int(e["mtime"]), sha256=sha, rev=int(e["rev"]))
        self.note("downloaded", key, e.get("by", ""))

    def remote_rmdir(self, key, by):
        """A folder deleted on the drive: to the trash as a whole when nothing in it changed here; else only the
        untouched files go, and what changed here stays (and goes up again)."""
        full = os.path.join(self.root, key)
        if not os.path.isdir(full) or os.path.islink(full):
            return
        known = {q for q in self.idx.under(key)}
        files, skipped = local.scan(full, set())
        clean = not skipped
        for f in files:
            p = f"{key}/{f.path}"
            ie = self.idx.files.get(p)
            if p not in known or not ie or not self.unchanged(ie, f):
                clean = False
                break
        if clean:
            local.trash(full)
            self.note(f"deleted on {by or 'the drive'}", key + "/", by)
            return
        for f in sorted(files, key=lambda f: -len(f.path)):
            p = f"{key}/{f.path}"
            ie = self.idx.files.get(p)
            if not f.dir and ie and p in known and self.unchanged(ie, f):
                local.trash(os.path.join(self.root, p))
            elif f.dir:
                try:
                    os.rmdir(os.path.join(self.root, p))
                except OSError:
                    pass
        try:
            os.rmdir(full)
        except OSError:
            log(f"[{self.name}] {key}/ deleted on the drive: kept what changed here, it goes up again")
        self.note(f"deleted on {by or 'the drive'}", key + "/", by)

    # ---- local pass: the folder against the index
    def push_local(self):
        files, skipped = local.scan(self.root, set(self.idx.files))
        self.report_skipped(skipped)
        seen, ups, total, fresh = set(), [], 0, False
        now_ms = time.time() * 1000
        for f in files:
            seen.add(f.path)
            ie = self.idx.files.get(f.path)
            if f.dir:
                if not ie or not ie["dir"]:
                    ups.append(f)
                continue
            if not ie or ie["dir"] or ie["size"] != f.size or ie["mtime"] // 1000 != f.mtime // 1000:
                if now_ms - f.mtime < FRESH * 1000:
                    fresh = True   # still being written: the next pass takes it
                    continue
                ups.append(f)
                total += f.size
        held = [p for p, _ in skipped]
        gone = [p for p in self.idx.files if p not in seen and not local.ignored(p.rsplit("/", 1)[-1])
                and not any(local.under(p, h) for h in held)]
        pending, done = len(ups) + len(gone), 0
        self.progress(pending, 0, total)
        # deletes first, deepest paths first (a folder after its files)
        dropped = set()
        for p in sorted(gone, key=len, reverse=True):
            if not self.running:
                raise Stopped()
            if p in dropped or p not in self.idx.files:
                continue
            r = self.dc.delete(p, self.idx.files[p]["rev"])
            if r.ok:
                self.note("deleted", p, self.device)
            elif r.status == 412:
                log(f"[{self.name}] delete {p}: changed on the drive meanwhile, it comes back down")
            elif r.status != 404:
                raise HttpError(r)
            for q in self.idx.under(p):
                dropped.add(q)
                self.idx.drop(q)
            pending -= 1
            self.progress(pending)
        for f in ups:
            if not self.running:
                raise Stopped()
            try:
                if f.dir:
                    r = self.dc.mkdir(f.path)
                    self.idx.set(f.path, dir=True, rev=int(r.get("rev", 0)))
                else:
                    self.upload(f)
                    done += f.size
                    self.idx.save()
            except HttpError as e:
                if e.status not in (400, 409):   # 400 a path the drive refuses, 409 a file here is a folder there (or back)
                    raise
                log(f"[{self.name}] not synced: {f.path} ({e})")
                self.skipped.append((f.path, str(e)))
            pending -= 1
            self.progress(pending, done)
        if self.watcher:
            self.watcher.add(self.root)
            for f in files:
                if f.dir:
                    self.watcher.add(os.path.join(self.root, f.path))
        if fresh:
            self.again = True

    def keep_read_only(self):
        """A drive shared to read only: nothing goes up. Files deleted here come back from the drive; files new or
        changed here stay on this PC (counted in the status), and a later change on the drive keeps them as conflict
        copies (apply). Like the Windows client's KeepReadOnly."""
        files, skipped = local.scan(self.root, set(self.idx.files))
        self.report_skipped(skipped)
        seen, local_only = set(), 0
        for f in files:
            seen.add(f.path)
            ie = self.idx.files.get(f.path)
            if not f.dir and (not ie or ie["dir"] or ie["size"] != f.size or ie["mtime"] // 1000 != f.mtime // 1000):
                local_only += 1
        held = [p for p, _ in skipped]
        missing = [p for p in self.idx.files if p not in seen and not local.ignored(p.rsplit("/", 1)[-1])
                   and not any(local.under(p, h) for h in held)]
        for p in sorted(missing, key=len):   # folders before their files
            if not self.running:
                raise Stopped()
            ie = self.idx.files.get(p)
            if not ie:
                continue
            if ie["dir"]:
                os.makedirs(os.path.join(self.root, p), exist_ok=True)
                continue
            e = {"path": p, "rev": ie["rev"], "kind": "file", "size": ie["size"], "mtime": ie["mtime"], "sha256": ie["sha256"]}
            self.idx.drop(p)
            self.apply(e)   # downloads it again
        if local_only != self.st["local_only"] and local_only:
            log(f"[{self.name}] read only: {local_only} file(s) new or changed here stay on this PC (not uploaded)")
        self.st["local_only"] = local_only
        if self.watcher:
            self.watcher.add(self.root)
            for f in files:
                if f.dir:
                    self.watcher.add(os.path.join(self.root, f.path))

    def report_skipped(self, skipped):
        old = {p for p, _ in self.skipped}
        for p, why in skipped:
            if p not in old:
                log(f"[{self.name}] not synced: {p} ({why})")
        self.skipped = skipped

    def upload(self, f):
        full = os.path.join(self.root, f.path)
        try:
            sha, size = sha256_file(full)
        except OSError as e:
            log(f"[{self.name}] cannot read {f.path} ({e.strerror}): retried later")
            return
        ie = self.idx.files.get(f.path)
        base = ie["rev"] if ie and not ie["dir"] else 0
        if ie and not ie["dir"] and ie["sha256"] == sha:   # same content, only the date changed: nothing to send
            ie["size"], ie["mtime"] = size, f.mtime
            return
        if size <= common.SMALL_MAX:
            r = self.dc.put_small(f.path, full, size, sha, f.mtime, base)
        else:
            up = self.idx.uploads.get(f.path)
            off = 0
            if up and (up.get("sha256") != sha or up.get("size") != size):
                up = None
            if up:
                off = self.dc.upload_offset(up["id"])
                if off < 0:
                    up, off = None, 0
            if not up:
                r = self.dc.upload_start(f.path, size, sha, f.mtime, base)
                if r.status == 412:
                    return self.conflict(f.path)
                if not r.ok:
                    raise HttpError(r)
                up = {"id": r.json()["upload"], "sha256": sha, "size": size}
                self.idx.uploads[f.path] = up
                self.idx.save()
            while off < size:
                if not self.running:
                    raise Stopped()
                n = min(common.CHUNK, size - off)
                r = self.dc.upload_chunk(up["id"], full, off, n)
                if r.status == 409:
                    j = r.json() or {}
                    off = int(r.header("Upload-Offset") or j.get("offset") or 0)
                    continue
                if not r.ok:
                    if r.status in (400, 404):
                        self.idx.uploads.pop(f.path, None)
                    raise HttpError(r)
                off += n
                self.st["done"] += n
                self.write_status()
            r = self.dc.upload_commit(up["id"], base)
            self.idx.uploads.pop(f.path, None)
        if r.status == 412:
            return self.conflict(f.path)
        if not r.ok:
            raise HttpError(r)
        e = r.json() or {}
        self.idx.set(f.path, size=size, mtime=f.mtime, sha256=sha, rev=int(e.get("rev", 0)))
        self.note("uploaded" if base else "added", f.path, self.device)

    def conflict(self, path):
        """The drive has a newer version: ours is kept as a conflict copy (it goes up on the next pass) and the index
        entry is forgotten so the remote pass brings the drive's version down."""
        cname = local.conflict_name(path, self.pc)
        try:
            os.replace(os.path.join(self.root, path), os.path.join(self.root, cname))
            log(f"[{self.name}] conflict on {path}: local copy kept as {cname}")
            self.note("conflict, local copy kept", cname)
        except OSError as e:
            log(f"[{self.name}] conflict on {path}: cannot rename ({e.strerror})")
        self.idx.drop(path)
        self.again = True

    def sync_pass(self):
        self.again = False
        try:
            self.pull_remote()
            if self.st["read_only"]:
                self.keep_read_only()
            else:
                self.push_local()
        finally:
            self.idx.save()

    def idle_text(self):
        if not self.st["read_only"]:
            return "Up to date"
        n = self.st["local_only"]
        return "Up to date (read only" + (f"; {n} local change{'s' if n != 1 else ''} not uploaded)" if n else ")")

    # ---- foreground: passes until up to date (sync --once)
    def run_once(self):
        """One sync of this drive -> '' when up to date, else what went wrong."""
        self.running = True
        try:
            self.prepare()
            for _ in range(4):
                self.set_state("connecting", "Looking for the drive's node...")
                self.connect()
                self.set_state("syncing", "Checking the drive...")
                self.sync_pass()
                if not self.again:
                    break
                time.sleep(FRESH + 1)
            self.st.update(last_sync=time.time(), pending=0, done=0, total=0)
            self.set_state("idle", self.idle_text())
            return ""
        except Fatal as e:
            self.set_state("error", str(e))
            return str(e)
        except Busy:
            raise
        except Exception as e:   # noqa: BLE001 - shown as is
            log(f"[{self.name}] sync: {e}")
            self.set_state("offline", str(e))
            return str(e)
        finally:
            self.running = False
            self.unlock()

    # ---- background: threads
    def start(self):
        self.prepare()
        self.running = True
        self.watcher = Watcher()
        for fn in (self.loop, self.poll, self.watch):
            t = threading.Thread(target=fn, name=f"{fn.__name__}-{self.id}", daemon=True)
            t.start()
            self.threads.append(t)

    def stop(self, timeout=20):
        self.running = False
        self.wake.set()
        for t in self.threads:
            t.join(timeout)
        self.threads = []
        if self.watcher:
            self.watcher.close()
        self.unlock()

    def kick(self):
        self.dirty, self.dirty_at = True, 0.0
        self.wake.set()

    def watch(self):
        while self.running:
            if self.watcher.wait(1.0):
                self.dirty, self.dirty_at = True, time.time()
                self.wake.set()

    def poll(self):
        """Long polls on the change feed: wakes the loop when the drive changed."""
        while self.running:
            dc = self.dc
            if dc is None or self.st["state"] in ("error", "connecting"):
                time.sleep(2)
                continue
            try:
                r = dc.changes(self.idx.rev, 30)
            except Exception:   # noqa: BLE001 - the loop reports errors; here only wait and try again
                time.sleep(10)
                continue
            if r.get("changes") and self.running:
                self.kick()
                time.sleep(2)

    def loop(self):
        backoff, last_full = 30, 0.0
        while self.running:
            try:
                self.set_state("connecting", "Looking for the drive's node...")
                self.connect()
                self.set_state("syncing", "Checking the drive...")
                self.dirty = False
                self.sync_pass()
            except Stopped:
                break
            except Fatal as e:
                log(f"[{self.name}] {e}")
                self.set_state("error", str(e))
                self.sleep(300, early=False)
                continue
            except Exception as e:   # noqa: BLE001 - offline: retried with a backoff
                log(f"[{self.name}] sync: {e}")
                self.set_state("offline", str(e))
                self.url_check = 0
                self.sleep(backoff)
                backoff = min(300, backoff * 2)
                continue
            backoff = 30
            self.st.update(last_sync=time.time(), pending=0, done=0, total=0)
            self.set_state("idle", self.idle_text())
            last_full = time.time()
            # wait for the watcher (2 s of quiet after the last event), the poller, a re-pass, or the full scan
            interval = FULL_SCAN if self.watcher.ok else FULL_SCAN_NO_WATCH
            again_at = time.time() + FRESH + 1 if self.again else None
            while self.running:
                self.wake.wait(1.0)
                self.wake.clear()
                now = time.time()
                if self.dirty and now - self.dirty_at >= QUIET:
                    break
                if again_at and now >= again_at:
                    break
                if now - last_full >= interval:
                    break
        self.write_status(force=True)

    def sleep(self, seconds, early=True):
        """Waits; a local change or a kick ends the wait early (not after a fatal error: nothing to retry soon)."""
        end = time.time() + seconds
        while self.running and time.time() < end:
            if self.wake.wait(min(1.0, max(0.0, end - time.time()))):
                self.wake.clear()
                if early and self.dirty:
                    return

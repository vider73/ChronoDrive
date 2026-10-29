"""The local side: scanning a folder, the names the drive refuses, case folding, the freedesktop trash.

The drive is Windows-shaped (API.md section 17): names without \\ / : * ? " < > | or control characters, not a reserved
Windows name, not ending in a space or a dot, and names that differ only by case are one file (the drive compares
paths with SQLite NOCASE: ASCII letters only). A Linux file that breaks those rules is skipped and reported, never
sent; two local names that differ only by case sync as one (the one already synced, else the first by name)."""
import datetime
import errno
import os
import re
import shutil
import stat
import urllib.parse

from . import common
from .common import log

BAD_CHARS = re.compile(r'[\x00-\x1f\\/:*?"<>|]')
RESERVED = re.compile(r"^(con|prn|aux|nul|com[1-9]|lpt[1-9])(\..*)?$", re.I)
MAX_PATH = 400
_ASCII_FOLD = str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz")


def fold(p):
    """The drive's idea of "the same path" (SQLite NOCASE folds ASCII only)."""
    return p.translate(_ASCII_FOLD)


def under(p, folder):
    """p is folder or inside it, case-insensitively like the drive."""
    a, b = fold(p), fold(folder)
    return a == b or a.startswith(b + "/")


def ignored(name):
    """Never synced, either way: the Windows client's list plus Linux editors' and desktops' temporary files."""
    l = name.lower()
    if l in ("desktop.ini", "thumbs.db", ".msdrive", ".ds_store", ".directory") or l.startswith("~$"):
        return True
    if l.startswith((".~lock.", ".goutputstream-", ".trash-")) or l.endswith((".tmp", ".part", ".crdownload", ".swp", ".swx")):
        return True
    return False


def bad_name(name):
    """Why the drive would refuse this name, or ''."""
    try:
        name.encode("utf-8")
    except UnicodeEncodeError:
        return "not UTF-8"
    m = BAD_CHARS.search(name)
    if m:
        c = m.group(0)
        return f"'{c}' is not allowed" if ord(c) >= 32 else "control character"
    if RESERVED.match(name):
        return "reserved name on Windows"
    if name.endswith((" ", ".")):
        return "ends in a space or a dot"
    if len(name) > 255:
        return "name longer than 255 characters"
    return ""


class LocalFile:
    __slots__ = ("path", "dir", "size", "mtime")

    def __init__(self, path, is_dir, size=0, mtime=0):
        self.path, self.dir, self.size, self.mtime = path, is_dir, size, mtime


def mtime_ms(st):
    return st.st_mtime_ns // 1_000_000


def scan(root, known):
    """Every file and folder under root -> (files, skipped). `known` = the index's paths (they win a case clash).
    skipped = [(path, reason)]: what is not synced and why (symbolic links, special files, names the drive refuses,
    case clashes)."""
    out, skipped = [], []

    def walk(rel):
        d = os.path.join(root, rel) if rel else root
        try:
            with os.scandir(d) as it:
                ents = sorted(it, key=lambda e: e.name)
        except OSError as e:
            if rel:
                skipped.append((rel, f"cannot read the folder ({e.strerror})"))
            return
        byfold = {}
        for e in ents:
            if ignored(e.name):
                continue
            p = f"{rel}/{e.name}" if rel else e.name
            why = bad_name(e.name)
            if why:
                skipped.append((p, why))
                continue
            if len(p) > MAX_PATH:
                skipped.append((p, f"path longer than {MAX_PATH} characters"))
                continue
            byfold.setdefault(fold(e.name), []).append((p, e))
        for group in byfold.values():
            if len(group) > 1:   # names that differ only by case: one file on the drive
                group.sort(key=lambda g: (g[0] not in known, g[0]))
                for p, _ in group[1:]:
                    skipped.append((p, f"differs only by case from {group[0][0].rsplit('/', 1)[-1]}"))
            p, e = group[0]
            try:
                st = e.stat(follow_symlinks=False)
            except OSError:
                continue
            if stat.S_ISLNK(st.st_mode):
                skipped.append((p, "symbolic link (not synced)"))
            elif stat.S_ISDIR(st.st_mode):
                out.append(LocalFile(p, True))
                walk(p)
            elif stat.S_ISREG(st.st_mode):
                out.append(LocalFile(p, False, st.st_size, mtime_ms(st)))
            else:
                skipped.append((p, "special file (not synced)"))

    walk("")
    return out, skipped


def stat_local(root, rel):
    try:
        st = os.stat(os.path.join(root, rel), follow_symlinks=False)
    except OSError:
        return None
    if stat.S_ISLNK(st.st_mode):
        return None
    return LocalFile(rel, stat.S_ISDIR(st.st_mode), st.st_size, mtime_ms(st))


def resolve(root, drive_path):
    """The local path of a drive path: each segment matched to an existing local name case-insensitively (the drive
    says "Docs/a.txt", this PC has "docs/"), else as the drive spells it."""
    parts, cur, rel = drive_path.split("/"), root, []
    for i, seg in enumerate(parts):
        name = seg
        try:
            if not os.path.lexists(os.path.join(cur, seg)):
                f = fold(seg)
                for n in os.listdir(cur):
                    if fold(n) == f:
                        name = n
                        break
        except OSError:
            rel += parts[i:]
            break
        rel.append(name)
        cur = os.path.join(cur, name)
    return "/".join(rel)


def set_mtime(path, ms):
    try:
        os.utime(path, ns=(int(ms) * 1_000_000, int(ms) * 1_000_000))
    except OSError:
        pass


def conflict_name(path, pc):
    """name (conflict <PC> <date>).ext, like the Windows client."""
    d, _, name = path.rpartition("/")
    dot = name.rfind(".")
    stem, ext = (name, "") if dot <= 0 else (name[:dot], name[dot:])
    return (d + "/" if d else "") + f"{stem} (conflict {pc} {common.stamp_local()}){ext}"


# ------------------------------------------------------------------ the freedesktop trash (Trash spec 1.0)
def _trash_dirs(path):
    """The home trash when path is on the same filesystem, else $topdir/.Trash-$uid."""
    home = os.path.join(common.data_home(), "Trash")
    try:
        dev = os.stat(path, follow_symlinks=False).st_dev
        os.makedirs(home, exist_ok=True)
        if os.stat(home).st_dev == dev:
            return home
        top = os.path.abspath(path)
        while os.path.dirname(top) != top and os.stat(os.path.dirname(top)).st_dev == dev:
            top = os.path.dirname(top)
        return os.path.join(top, f".Trash-{os.getuid()}")
    except OSError:
        return home


def trash(path):
    """Moves path (file or folder) to the trash; deletes it when there is no trash to use. True when it is gone."""
    tdir = _trash_dirs(path)
    try:
        files, info = os.path.join(tdir, "files"), os.path.join(tdir, "info")
        os.makedirs(files, mode=0o700, exist_ok=True)
        os.makedirs(info, mode=0o700, exist_ok=True)
        base = os.path.basename(path.rstrip("/")) or "file"
        name, n = base, 1
        while True:   # reserve the .trashinfo first (O_EXCL), as the spec asks
            try:
                fd = os.open(os.path.join(info, name + ".trashinfo"), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                break
            except FileExistsError:
                n += 1
                stem, dot, ext = base.rpartition(".")
                name = f"{stem}.{n}.{ext}" if dot and stem else f"{base}.{n}"
        top = os.path.dirname(tdir) if os.path.basename(tdir).startswith(".Trash-") else None
        shown = os.path.relpath(os.path.abspath(path), top) if top else os.path.abspath(path)
        with os.fdopen(fd, "w") as f:
            f.write("[Trash Info]\nPath=" + urllib.parse.quote(shown) + "\nDeletionDate="
                    + datetime.datetime.now().strftime("%Y-%m-%dT%H:%M:%S") + "\n")
        try:
            os.rename(path, os.path.join(files, name))
            return True
        except OSError as e:
            os.remove(os.path.join(info, name + ".trashinfo"))
            if e.errno != errno.EXDEV:
                raise
    except OSError as e:
        log(f"trash {path}: {e}; deleted instead (the drive keeps it in its trash for 30 days)")
    try:
        if os.path.isdir(path) and not os.path.islink(path):
            shutil.rmtree(path)
        else:
            os.remove(path)
        return True
    except OSError:
        return not os.path.lexists(path)

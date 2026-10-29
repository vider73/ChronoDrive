"""End-to-end test of ChronoDrive for Linux against a real node, with a Windows PC on the same drive: a throwaway user,
the Linux client in WSL (its own XDG folders, so the real setup is not touched) and the Windows cdrive.exe, both paired
through the real pairing flow (approved with the user's token), one drive on a node with the drive role. Files both
ways, big files (chunked), names the drive refuses and a case clash on Linux, conflicts found on either side, deletes
(to the Linux trash), a folder delete, then the Linux background service (long poll + inotify). Everything is removed
at the end.

Run on Windows: python linux\\tools\\e2e_wsl.py [node] [--installed]
  node         default office
  --installed  use ~/.local/bin/chronodrive in WSL (what install.sh put there) instead of the checkout's linux/chronodrive
Needs build\\Release\\cdrive.exe, WSL (Ubuntu) and the Miniserver repo's .secrets.env."""
import hashlib, json, os, re, secrets, shutil, subprocess, sys, tempfile, time, urllib.request, urllib.error, uuid

env = dict(l.strip().split("=", 1) for l in open(r"I:\LLMLab\Azure\miniserver\.secrets.env", encoding="utf-8") if "=" in l)
B = env["MINISERVER_URL"].rstrip("/")
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
WIN = os.path.join(REPO, "build", "Release", "cdrive.exe")
args = [a for a in sys.argv[1:] if not a.startswith("--")]
NODE = args[0] if args else "office"
INSTALLED = "--installed" in sys.argv
DISTRO = os.environ.get("WSL_DISTRO", "Ubuntu")


def call(method, path, body=None, token=None):
    data = json.dumps(body).encode() if body is not None else None
    h = {"Content-Type": "application/json"}
    if token:
        h["Authorization"] = token
    req = urllib.request.Request(B + path, data=data, method=method, headers=h)
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            t = r.read().decode()
            return r.status, (json.loads(t) if t else None)
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()[:300]


def ok(c, msg):
    print(("  ok   " if c else "  FAIL ") + msg, flush=True)
    if not c:
        raise SystemExit(1)


def wsl(*cmd, timeout=180, stdin=None):
    r = subprocess.run(["wsl.exe", "-d", DISTRO, "--", *cmd], capture_output=True, timeout=timeout, input=stdin)
    return r.returncode, (r.stdout + r.stderr).decode("utf-8", "replace").strip()


# ---------------------------------------------------------------- the Linux side
rc, LHOME = wsl("bash", "-lc", "echo $HOME")
LT = f"{LHOME}/cd-e2e-{uuid.uuid4().hex[:8]}"           # config, state, trash and the synced folder of the Linux "PC"
LF = f"{LT}/drive"
LENV = [f"XDG_CONFIG_HOME={LT}/config", f"XDG_STATE_HOME={LT}/state", f"XDG_DATA_HOME={LT}/data", f"MINISERVER_URL={B}"]
LCLI = f"{LHOME}/.local/bin/chronodrive" if INSTALLED else "/mnt/" + REPO[0].lower() + REPO[2:].replace("\\", "/") + "/linux/chronodrive"


def lcli(*a, timeout=300):
    return wsl("env", *LENV, "python3", LCLI, *a, timeout=timeout)


def lx(script, timeout=120):
    """A bash snippet in the Linux drive folder."""
    return wsl("bash", "-c", f"cd '{LF}' && {script}", timeout=timeout)


def lread(rel):
    rc, o = wsl("cat", f"{LF}/{rel}")
    return o if rc == 0 else None


def lexists(rel):
    return wsl("test", "-e", f"{LF}/{rel}")[0] == 0


def lsha(rel):
    rc, o = wsl("sha256sum", f"{LF}/{rel}")
    return o.split()[0] if rc == 0 else None


def lsync(label):
    rc, o = lcli("sync", "--once")
    print(f"   L {label}: " + " | ".join(o.splitlines()[-6:]), flush=True)
    return rc, o


# ---------------------------------------------------------------- the Windows side
def wcli(data_dir, *a, timeout=180):
    e = dict(os.environ, CHRONODRIVE_DATA=data_dir, MINISERVER_URL=B)
    r = subprocess.run([WIN, *a], env=e, capture_output=True, text=True, timeout=timeout, encoding="utf-8", errors="replace")
    return r.returncode, (r.stdout + r.stderr).strip()


def wsync(data_dir, seconds, label):
    rc, o = wcli(data_dir, "sync", "--seconds", str(seconds), timeout=seconds + 60)
    print(f"   W {label}: " + " | ".join(o.splitlines()[-5:]), flush=True)
    return o


def wsha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest() if os.path.exists(path) else None


def pair(cmd, utok, label, penv=None):
    """Runs a client's pairing, reads the code from its first line and approves it as the test user."""
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace", env=penv)
    line = p.stdout.readline()
    m = re.search(r"Pairing code ([A-Z0-9-]+)", line)
    ok(m is not None, f"{label}: pairing started: " + line.strip()[:90])
    s, _ = call("POST", "/api/ms/link/approve", {"code": m.group(1)}, token=utok)
    ok(s == 200, f"{label}: approved as the test user ({s})")
    o = p.communicate(timeout=90)[0]
    ok(p.returncode == 0 and "Linked to" in o, f"{label}: " + o.strip().splitlines()[-1][:90] if o.strip() else "no output")


s, r = call("POST", "/api/collections/_superusers/auth-with-password", {"identity": env["PB_SUPERUSER_EMAIL"], "password": env["PB_SUPERUSER_PASSWORD"]})
su = r["token"]
email = f"cdtest-{uuid.uuid4().hex[:8]}@example.com"
pw = secrets.token_urlsafe(16)
s, r = call("POST", "/api/collections/users/records", {"email": email, "password": pw, "passwordConfirm": pw, "name": "cd linux test"})
ok(s == 200, "test user")
uid = r["id"]
s, r = call("PATCH", f"/api/collections/users/records/{uid}", {"verified": True, "approved": True}, token=su)   # the account gate
ok(s == 200, "test user verified and approved")
s, r = call("POST", "/api/collections/users/auth-with-password", {"identity": email, "password": pw})
utok = r["token"]
tmp = tempfile.mkdtemp(prefix="cdrive-lx-")
W, FW = os.path.join(tmp, "pcW"), os.path.join(tmp, "folderW")
did, svc = "", None
print(f"Linux client: {LCLI}  (state in {LT})")
try:
    wsl("mkdir", "-p", LT)
    pair(["wsl.exe", "-d", DISTRO, "--", "env", *LENV, "python3", LCLI, "pair", "--name", "LNX-e2e", "--no-qr"], utok, "Linux")
    pair([WIN, "pair", "--name", "PC-W", "--no-browser"], utok, "Windows", dict(os.environ, CHRONODRIVE_DATA=W, MINISERVER_URL=B))

    rc, o = wcli(W, "create", "--name", "cd linux test", "--node", NODE, "--size", "2")
    ok(rc == 0 and "created" in o, o[:100])
    did = o.split()[1]
    rc, o = wcli(W, "use", did, "--folder", FW)
    ok(rc == 0, o[:100])
    rc, o = lcli("mount", "cd linux test", LF)
    ok(rc == 0 and "Mounted" in o, "L mount: " + o.splitlines()[0])
    rc, o = lcli("drives")
    print("   " + "\n   ".join(o.splitlines()))
    ok(rc == 0 and did in o, "L drives lists the drive")
    print("   waiting 75 s: the node agent makes the drive root and starts serving it", flush=True)
    time.sleep(75)
    rc, o = lcli("drives")
    ok(rc == 0 and re.search(r"\byes\b", o) is not None, "L: the drive is reachable from here")

    # 1. Linux -> Windows: a small file in a folder, a big one (chunked), names the drive refuses, a case clash
    lx("mkdir -p docs && printf 'hello from L\\n' > docs/hello.txt && head -c 9437307 /dev/urandom > big.bin && "
       "printf x > 'bad?.txt' && printf x > 'CON.txt' && printf A > Case.txt && printf a > case.txt && printf n > notes.txt")
    big = lsha("big.bin")
    rc, o = lsync("first upload")
    ok(rc == 0 and "Up to date" in o, "L uploaded (small put + chunked)")
    ok("bad?.txt" in o and "CON.txt" in o and "case.txt" in o and "differs only by case" in o, "L skipped and reported the names the drive refuses and the case clash")
    wsync(W, 40, "first download")
    ok(open(os.path.join(FW, "docs", "hello.txt")).read() == "hello from L\n" and wsha(os.path.join(FW, "big.bin")) == big, "W got both files intact")
    names = set(os.listdir(FW))
    ok("Case.txt" in names and "case.txt" not in names and not any(n.startswith(("bad", "CON")) for n in names), f"W has one Case.txt and no refused names ({sorted(names)})")

    # 2. Windows -> Linux: an edit, a new file, a big file
    with open(os.path.join(FW, "docs", "hello.txt"), "a") as f:
        f.write("and W\n")
    open(os.path.join(FW, "fromW.txt"), "w").write("W")
    bigw = os.urandom(10 * 1024 * 1024 + 7)
    open(os.path.join(FW, "bigW.bin"), "wb").write(bigw)
    wsync(W, 45, "after its edits")
    lsync("pulling W's edits")
    ok(lread("docs/hello.txt") == "hello from L\nand W" and lread("fromW.txt") == "W" and lsha("bigW.bin") == hashlib.sha256(bigw).hexdigest(),
       "L got W's edit, new file and big file")

    # 3. conflict found on Windows: L syncs first
    lx("printf 'L wins?\\n' > docs/hello.txt")
    open(os.path.join(FW, "docs", "hello.txt"), "w").write("W wins?\n")
    lsync("conflict, L first")
    wsync(W, 35, "conflict, W second")
    lsync("conflict copy from W")
    wc = [f for f in os.listdir(os.path.join(FW, "docs")) if "conflict" in f]
    rc, lls = lx("ls docs")
    ok(wc and open(os.path.join(FW, "docs", "hello.txt")).read() == "L wins?\n" and lread("docs/hello.txt") == "L wins?" and wc[0] in lls,
       f"conflict found on W: W keeps its copy ({wc}), both have L's version, L has the copy too")

    # 4. conflict found on Linux: W syncs first
    open(os.path.join(FW, "notes.txt"), "w").write("W notes\n")
    lx("printf 'L notes\\n' > notes.txt")
    wsync(W, 30, "conflict, W first")
    rc, o = lsync("conflict, L second")
    rc, lls = lx("ls")
    lc = [n for n in lls.splitlines() if n.startswith("notes (conflict ")]
    ok(lc and lread("notes.txt") == "W notes" and lread(lc[0]) == "L notes", f"conflict found on L: its copy kept as {lc}, W's version under the name")
    wsync(W, 30, "the Linux conflict copy")
    ok(os.path.exists(os.path.join(FW, lc[0])), "the Linux conflict copy went up to W")

    # 5. deletes: a file on W (L puts it in its trash), a file on L, then a whole folder on W
    os.remove(os.path.join(FW, "big.bin"))
    wsync(W, 30, "delete big.bin")
    lsync("delete from W")
    ok(not lexists("big.bin") and wsl("test", "-f", f"{LT}/data/Trash/files/big.bin")[0] == 0 and wsl("test", "-f", f"{LT}/data/Trash/info/big.bin.trashinfo")[0] == 0,
       "a delete on W sends the file to L's trash (with its .trashinfo)")
    lx("rm fromW.txt")
    lsync("delete fromW.txt")
    wsync(W, 30, "delete from L")
    ok(not os.path.exists(os.path.join(FW, "fromW.txt")), "a delete on L removes the file on W")
    shutil.rmtree(os.path.join(FW, "docs"))
    wsync(W, 30, "delete the docs folder")
    lsync("folder delete from W")
    ok(not lexists("docs") and wsl("test", "-d", f"{LT}/data/Trash/files/docs")[0] == 0, "a folder deleted on W goes to L's trash as a whole")

    # 6. the background service: long poll (W -> L) and inotify (L -> W)
    svc = "cd-e2e-" + LT.rsplit("-", 1)[-1]   # a transient systemd --user unit, like chronodrive.service
    rc, o = wsl("systemd-run", "--user", "--unit", svc, "--collect", "-p", "RuntimeMaxSec=300", *[f"--setenv={v}" for v in LENV],
                "--setenv=PYTHONUNBUFFERED=1", "python3", LCLI, "service")
    ok(rc == 0, f"the Linux service runs as a systemd --user unit ({o.splitlines()[-1][:80] if o else ''})")
    time.sleep(8)
    open(os.path.join(FW, "live.txt"), "w").write("live from W")
    wsync(W, 20, "live.txt")
    t0 = time.time()
    while time.time() - t0 < 60 and lread("live.txt") != "live from W":
        time.sleep(2)
    ok(lread("live.txt") == "live from W", f"the Linux service brought W's new file down on its own ({time.time() - t0:.0f} s after W's sync)")
    lx("printf 'from the Linux service' > lnx-live.txt")
    time.sleep(10)
    wsync(W, 25, "lnx-live.txt")
    ok(os.path.exists(os.path.join(FW, "lnx-live.txt")) and open(os.path.join(FW, "lnx-live.txt")).read() == "from the Linux service",
       "the Linux service sent a local change up on its own (inotify)")
    rc, o = lcli("status")
    print("   " + "\n   ".join(o.splitlines()))
    ok("Up to date" in o, "chronodrive status shows the drive up to date")

    s, d = call("GET", f"/api/ms/drive/drives/{did}", token=utok)
    print("   drive on the server:", d.get("used"), "bytes,", d.get("files"), "files, rev", d.get("rev"), "online", d.get("online"))
    want = len([f for f in os.listdir(FW) if os.path.isfile(os.path.join(FW, f))])
    ok(s == 200 and d.get("files") == want, f"the node reported the drive ({d.get('files')} files, W has {want})")
    print(f"ALL OK (ChronoDrive Linux <-> Windows end to end on node {NODE})")
finally:
    if svc:
        wsl("systemctl", "--user", "stop", svc)
        print("   service log:\n     " + "\n     ".join(wsl("journalctl", "--user", "-u", svc, "--no-pager", "-o", "cat", "-n", "15")[1].splitlines()))
    if did:
        wcli(W, "remove", did)
        call("DELETE", f"/api/ms/drive/drives/{did}", token=utok)
    call("DELETE", f"/api/collections/users/records/{uid}", token=su)
    shutil.rmtree(tmp, ignore_errors=True)
    if LT.count("/") >= 3 and "/cd-e2e-" in LT:
        wsl("rm", "-rf", LT)
    print("cleaned up (the drive is marked deleting: the node agent removes its folder; the test user is gone)")

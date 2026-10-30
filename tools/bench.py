"""Sync speed of ChronoDrive against a real node: a throwaway user, two "PCs" (A uploads, B downloads), one drive, a
folder of many small files (like a ChronoScan job folder). Prints files per second for A's first upload and for B's
first download. Everything is removed at the end.

Usage: python tools/bench.py [--files 2000] [--node office] [--cli build\\Release\\cdrive.exe] [--workers N]
(needs the Miniserver repo's .secrets.env; --workers sets CHRONODRIVE_WORKERS for both PCs)"""
import argparse, json, os, random, secrets, shutil, subprocess, sys, tempfile, time, urllib.request, urllib.error, uuid, re
ap = argparse.ArgumentParser()
ap.add_argument("--files", type=int, default=2000)
ap.add_argument("--node", default="office")
ap.add_argument("--cli", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "Release", "cdrive.exe"))
ap.add_argument("--workers", default="")
a = ap.parse_args()
env = dict(l.strip().split("=", 1) for l in open(r"I:\LLMLab\Azure\miniserver\.secrets.env", encoding="utf-8") if "=" in l)
B = env["MINISERVER_URL"].rstrip("/")


def call(method, path, body=None, token=None):
    data = json.dumps(body).encode() if body is not None else None
    h = {"Content-Type": "application/json"}
    if token:
        h["Authorization"] = token
    req = urllib.request.Request(B + path, data=data, method=method, headers=h)
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            t = r.read().decode(); return r.status, (json.loads(t) if t else None)
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()[:300]


def ok(c, msg):
    print(("  ok   " if c else "  FAIL ") + msg, flush=True)
    if not c:
        raise SystemExit(1)


def cenv(data_dir):
    e = dict(os.environ, CHRONODRIVE_DATA=data_dir, MINISERVER_URL=B)
    if a.workers:
        e["CHRONODRIVE_WORKERS"] = a.workers
    return e


def cli(data_dir, *args, timeout=120):
    r = subprocess.run([a.cli, *args], env=cenv(data_dir), capture_output=True, text=True, timeout=timeout, encoding="utf-8", errors="replace")
    return r.returncode, (r.stdout + r.stderr).strip()


def pair(data_dir, utok, name):
    p = subprocess.Popen([a.cli, "pair", "--name", name, "--no-browser"], env=cenv(data_dir), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
    m = re.search(r"Pairing code ([A-Z0-9-]+)", p.stdout.readline())
    ok(m is not None, "pair started")
    s, r = call("POST", "/api/ms/link/approve", {"code": m.group(1)}, token=utok)
    ok(s == 200, "approved")
    p.communicate(timeout=60)
    ok(p.returncode == 0, "paired " + name)


def timed_sync(data_dir, label, limit=7200):
    """cdrive sync until its first pass ends ("idle"); seconds from the start."""
    t0 = time.time()
    p = subprocess.Popen([a.cli, "sync"], env=cenv(data_dir), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
    last = ""
    try:
        for line in p.stdout:
            line = line.rstrip()
            if line != last and ("pending" in line or "idle" in line or "offline" in line or "error" in line):
                print(f"   {label} {time.time() - t0:6.1f}s  {line[:150]}", flush=True)
            last = line
            if " idle " in line or line.split("  ", 1)[-1].startswith("idle"):
                break
            if time.time() - t0 > limit:
                break
    finally:
        p.kill()
        p.wait()
    return time.time() - t0


s, r = call("POST", "/api/collections/_superusers/auth-with-password", {"identity": env["PB_SUPERUSER_EMAIL"], "password": env["PB_SUPERUSER_PASSWORD"]})
su = r["token"]
email = f"cdbench-{uuid.uuid4().hex[:8]}@example.com"; pw = secrets.token_urlsafe(16)
s, r = call("POST", "/api/collections/users/records", {"email": email, "password": pw, "passwordConfirm": pw, "name": "cd bench"})
ok(s == 200, "test user"); uid = r["id"]
s, r = call("PATCH", f"/api/collections/users/records/{uid}", {"verified": True, "approved": True}, token=su)
ok(s == 200, "test user verified and approved")
s, r = call("POST", "/api/collections/users/auth-with-password", {"identity": email, "password": pw}); utok = r["token"]
tmp = tempfile.mkdtemp(prefix="cdbench-")
A, Bd = os.path.join(tmp, "pcA"), os.path.join(tmp, "pcB"); FA, FB = os.path.join(tmp, "folderA"), os.path.join(tmp, "folderB")
did = ""
try:
    pair(A, utok, "PC-A"); pair(Bd, utok, "PC-B")
    rc, out = cli(A, "create", "--name", "cd bench", "--node", a.node, "--size", "2")
    ok(rc == 0 and "created" in out, out[:100]); did = out.split()[1]
    # a ChronoScan-like tree: job folders of a few dozen small files (2-40 KB)
    rnd = random.Random(7); total = 0
    for i in range(a.files):
        d = os.path.join(FA, f"job{i // 40:04d}", "pages" if i % 3 else "data")
        os.makedirs(d, exist_ok=True)
        n = rnd.randint(2048, 40960); total += n
        with open(os.path.join(d, f"f{i:06d}.bin"), "wb") as f:
            f.write(os.urandom(n))
    print(f"   {a.files} files, {total / 1048576:.1f} MB in {FA}", flush=True)
    rc, out = cli(A, "use", did, "--folder", FA); ok(rc == 0, out[:80])
    rc, out = cli(Bd, "use", did, "--folder", FB); ok(rc == 0, out[:80])
    time.sleep(75)   # the node agent makes the drive root and starts the service (30 s runs on the office node)
    up = timed_sync(A, "A")
    down = timed_sync(Bd, "B")
    got = sum(len(fs) for _, _, fs in os.walk(FB))
    ok(got == a.files, f"B has {got} of {a.files} files")
    print(f"RESULT upload {a.files / up:.1f} files/s ({up:.0f} s), download {a.files / down:.1f} files/s ({down:.0f} s), workers={a.workers or 'default'}", flush=True)
finally:
    if did:
        call("DELETE", f"/api/ms/drive/drives/{did}", token=utok)
    call("DELETE", f"/api/collections/users/records/{uid}", token=su)
    shutil.rmtree(tmp, ignore_errors=True)
    print("cleaned up")

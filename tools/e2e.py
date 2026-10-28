"""End-to-end test of ChronoDrive against a real node: a throwaway user, two "PCs" (two data dirs, two devices paired
through the real pairing flow and approved with the user's token), one drive on a node with the drive role, cdrive
sync on both with files going both ways, a conflict, a delete. Everything is removed at the end.

Usage: python tools/e2e.py [node]     (default office; needs build\Release\cdrive.exe and the Miniserver repo's .secrets.env)"""
import json, os, secrets, shutil, subprocess, sys, tempfile, time, urllib.request, urllib.error, uuid, re
env = dict(l.strip().split("=", 1) for l in open(r"I:\LLMLab\Azure\miniserver\.secrets.env", encoding="utf-8") if "=" in l)
B = env["MINISERVER_URL"].rstrip("/")
CLI = r"I:\LLMLab\ChronoDrive\build\Release\cdrive.exe"

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

def cli(data_dir, *args, timeout=120):
    e = dict(os.environ, CHRONODRIVE_DATA=data_dir, MINISERVER_URL=B)
    r = subprocess.run([CLI, *args], env=e, capture_output=True, text=True, timeout=timeout, encoding="utf-8", errors="replace")
    return r.returncode, (r.stdout + r.stderr).strip()

def pair(data_dir, utok, name):
    e = dict(os.environ, CHRONODRIVE_DATA=data_dir, MINISERVER_URL=B)
    p = subprocess.Popen([CLI, "pair", "--name", name], env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
    line = p.stdout.readline()
    m = re.search(r"Pairing code ([A-Z0-9-]+)", line)
    ok(m is not None, "pair started: " + line.strip()[:80])
    s, r = call("POST", "/api/ms/link/approve", {"code": m.group(1)}, token=utok)
    ok(s == 200, f"approved as the test user ({s})")
    out = p.communicate(timeout=60)[0]
    ok(p.returncode == 0 and "Linked to" in out, "paired: " + out.strip().splitlines()[-1][:80])

def wait_sync(data_dir, seconds):
    rc, out = cli(data_dir, "sync", "--seconds", str(seconds), timeout=seconds + 60)
    return out

s, r = call("POST", "/api/collections/_superusers/auth-with-password", {"identity": env["PB_SUPERUSER_EMAIL"], "password": env["PB_SUPERUSER_PASSWORD"]})
su = r["token"]
email = f"cdtest-{uuid.uuid4().hex[:8]}@example.com"; pw = secrets.token_urlsafe(16)
s, r = call("POST", "/api/collections/users/records", {"email": email, "password": pw, "passwordConfirm": pw, "name": "cd test"})
ok(s == 200, "test user"); uid = r["id"]
s, r = call("POST", "/api/collections/users/auth-with-password", {"identity": email, "password": pw}); utok = r["token"]
tmp = tempfile.mkdtemp(prefix="cdrive-")
A, Bd = os.path.join(tmp, "pcA"), os.path.join(tmp, "pcB"); FA, FB = os.path.join(tmp, "folderA"), os.path.join(tmp, "folderB")
did = ""
try:
    pair(A, utok, "PC-A"); pair(Bd, utok, "PC-B")
    rc, out = cli(A, "create", "--name", "cd test", "--node", NODE, "--size", "2")
    ok(rc == 0 and "created" in out, out[:100]); did = out.split()[1]
    rc, out = cli(A, "use", did, "--folder", FA); ok(rc == 0, out[:80])
    rc, out = cli(Bd, "use", did, "--folder", FB); ok(rc == 0, out[:80])
    # the node agent needs a run (30 s office, 60 s home) to make the drive root and start the service
    time.sleep(75)
    os.makedirs(os.path.join(FA, "docs")); open(os.path.join(FA, "docs", "hello.txt"), "w").write("hello from A\n")
    big = os.urandom(9 * 1024 * 1024 + 123); open(os.path.join(FA, "big.bin"), "wb").write(big)
    out = wait_sync(A, 40); print("   A:", " | ".join(out.splitlines()[-4:]))
    ok("Up to date" in out and "uploaded" in out or "added" in out, "A uploaded its files (small put + chunked)")
    out = wait_sync(Bd, 40); print("   B:", " | ".join(out.splitlines()[-4:]))
    ok(os.path.exists(os.path.join(FB, "docs", "hello.txt")) and open(os.path.join(FB, "big.bin"), "rb").read() == big, "B downloaded both files intact")
    # B changes a file and adds one; A gets them
    open(os.path.join(FB, "docs", "hello.txt"), "a").write("and B\n"); open(os.path.join(FB, "fromB.txt"), "w").write("B")
    out = wait_sync(Bd, 30)
    out = wait_sync(A, 30)
    ok(open(os.path.join(FA, "docs", "hello.txt")).read() == "hello from A\nand B\n" and os.path.exists(os.path.join(FA, "fromB.txt")), "A got B's change and new file")
    # conflict: both edit hello.txt before syncing
    open(os.path.join(FA, "docs", "hello.txt"), "w").write("A wins?\n"); open(os.path.join(FB, "docs", "hello.txt"), "w").write("B wins?\n")
    out = wait_sync(A, 30); out = wait_sync(Bd, 30); out = wait_sync(A, 30)
    conflicts = [f for f in os.listdir(os.path.join(FB, "docs")) if "conflict" in f]
    ok(conflicts and open(os.path.join(FA, "docs", "hello.txt")).read() == "A wins?\n" and open(os.path.join(FB, "docs", "hello.txt")).read() == "A wins?\n", f"conflict kept as a copy on B ({conflicts}), both have A's version")
    ok(os.path.exists(os.path.join(FA, "docs", conflicts[0])), "the conflict copy went up to A too")
    # delete on A, gone on B (Recycle Bin)
    os.remove(os.path.join(FA, "big.bin"))
    out = wait_sync(A, 30); out = wait_sync(Bd, 30)
    ok(not os.path.exists(os.path.join(FB, "big.bin")), "a delete on A removes the file on B")
    s, d = call("GET", f"/api/ms/drive/drives/{did}", token=utok)
    print("   drive on the server:", d.get("used"), "bytes,", d.get("files"), "files, rev", d.get("rev"), "online", d.get("online"))
    ok(s == 200 and d["files"] == 3, f"the node reported the drive ({d.get('files')} files)")
    print("ALL OK (ChronoDrive end to end on node " + NODE + ")")
finally:
    if did:
        call("DELETE", f"/api/ms/drive/drives/{did}", token=utok)
    call("DELETE", f"/api/collections/users/records/{uid}", token=su)
    shutil.rmtree(tmp, ignore_errors=True)
    print("cleaned up (the drive is marked deleting: the office agent removes its folder)")

"""End-to-end test of ChronoDrive against a real node: a throwaway user, two "PCs" (two data dirs, two devices paired
through the real pairing flow and approved with the user's token), one drive on a node with the drive role, cdrive
sync on both with files going both ways, a conflict, a delete; then the drive shared with a second account (PC-C),
read only (nothing goes up, a delete comes back) and then editable. Everything is removed at the end.
CHRONODRIVE_NODE_URL=https://host:port reaches a home node over Tailscale instead of its LAN address.

Usage: python tools/e2e.py [node]     (default office; needs build\Release\cdrive.exe and the Miniserver repo's .secrets.env)"""
import json, os, secrets, shutil, subprocess, sys, tempfile, time, urllib.request, urllib.error, uuid, re
env = dict(l.strip().split("=", 1) for l in open(r"I:\LLMLab\Azure\miniserver\.secrets.env", encoding="utf-8") if "=" in l)
B = env["MINISERVER_URL"].rstrip("/")
CLI = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "Release", "cdrive.exe")
NODE = sys.argv[1] if len(sys.argv) > 1 else "office"

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

HOMES = {}   # data dir -> USERPROFILE for that "PC" (where a drive letter's files are kept)


def free_letter():
    import string, ctypes
    used = ctypes.windll.kernel32.GetLogicalDrives()
    for c in "YXWVUTSRQPONM":
        if not (used >> (ord(c) - 65)) & 1:
            return c
    raise SystemExit("no free drive letter for the test")


def cli(data_dir, *args, timeout=120):
    e = dict(os.environ, CHRONODRIVE_DATA=data_dir, MINISERVER_URL=B)
    if data_dir in HOMES:
        e["USERPROFILE"] = HOMES[data_dir]
    r = subprocess.run([CLI, *args], env=e, capture_output=True, text=True, timeout=timeout, encoding="utf-8", errors="replace")
    return r.returncode, (r.stdout + r.stderr).strip()

def pair(data_dir, utok, name):
    e = dict(os.environ, CHRONODRIVE_DATA=data_dir, MINISERVER_URL=B)
    p = subprocess.Popen([CLI, "pair", "--name", name, "--no-browser"], env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
    line = p.stdout.readline()
    m = re.search(r"Pairing code ([A-Z0-9-]+)", line)
    ok(m is not None, "pair started: " + line.strip()[:80])
    s, r = call("POST", "/api/ms/link/approve", {"code": m.group(1)}, token=utok)
    ok(s == 200, f"approved as the test user ({s})")
    out = p.communicate(timeout=60)[0]
    ok(p.returncode == 0 and "Linked to" in out, "paired: " + out.strip().splitlines()[-1][:80])

def wait_sync(data_dir, seconds, label=""):
    rc, out = cli(data_dir, "sync", "--seconds", str(seconds), timeout=seconds + 60)
    if label:
        print("   " + label + ": " + " | ".join(out.splitlines()[-5:]), flush=True)
    return out

s, r = call("POST", "/api/collections/_superusers/auth-with-password", {"identity": env["PB_SUPERUSER_EMAIL"], "password": env["PB_SUPERUSER_PASSWORD"]})
su = r["token"]
email = f"cdtest-{uuid.uuid4().hex[:8]}@example.com"; pw = secrets.token_urlsafe(16)
s, r = call("POST", "/api/collections/users/records", {"email": email, "password": pw, "passwordConfirm": pw, "name": "cd test"})
ok(s == 200, "test user"); uid = r["id"]
s, r = call("PATCH", f"/api/collections/users/records/{uid}", {"verified": True, "approved": True}, token=su)   # the account gate: only verified + approved accounts use the API
ok(s == 200, "test user verified and approved")
s, r = call("POST", "/api/collections/users/auth-with-password", {"identity": email, "password": pw}); utok = r["token"]
tmp = tempfile.mkdtemp(prefix="cdrive-")
A, Bd = os.path.join(tmp, "pcA"), os.path.join(tmp, "pcB"); FA, FB = os.path.join(tmp, "folderA"), os.path.join(tmp, "folderB")
did = ""; uid2 = ""
try:
    pair(A, utok, "PC-A"); pair(Bd, utok, "PC-B")
    rc, out = cli(A, "create", "--name", "cd test", "--node", NODE, "--size", "2")
    ok(rc == 0 and "created" in out, out[:100]); did = out.split()[1]
    # PC A shows the drive as a drive letter (its files kept under its own USERPROFILE), PC B in a folder
    HOMES[A] = os.path.join(tmp, "homeA"); os.makedirs(HOMES[A])
    L = free_letter()
    rc, out = cli(A, "use", did, "--letter", L); ok(rc == 0 and (L + ":") in out, out[:120])
    FA = L + ":\\"
    ok(os.path.isdir(FA), f"the drive letter {L}: exists")
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
    out = wait_sync(Bd, 45, "B after its edit")
    out = wait_sync(A, 45, "A pulling B's edit")
    ok(open(os.path.join(FA, "docs", "hello.txt")).read() == "hello from A\nand B\n" and os.path.exists(os.path.join(FA, "fromB.txt")), "A got B's change and new file")
    # conflict: both edit hello.txt before syncing
    open(os.path.join(FA, "docs", "hello.txt"), "w").write("A wins?\n"); open(os.path.join(FB, "docs", "hello.txt"), "w").write("B wins?\n")
    out = wait_sync(A, 45, "A conflict"); out = wait_sync(Bd, 45, "B conflict"); out = wait_sync(A, 45, "A after conflict")
    conflicts = [f for f in os.listdir(os.path.join(FB, "docs")) if "conflict" in f]
    ok(conflicts and open(os.path.join(FA, "docs", "hello.txt")).read() == "A wins?\n" and open(os.path.join(FB, "docs", "hello.txt")).read() == "A wins?\n", f"conflict kept as a copy on B ({conflicts}), both have A's version")
    ok(os.path.exists(os.path.join(FA, "docs", conflicts[0])), "the conflict copy went up to A too")
    # delete on A, gone on B (Recycle Bin)
    os.remove(os.path.join(FA, "big.bin"))
    out = wait_sync(A, 45, "A delete"); out = wait_sync(Bd, 45, "B delete")
    ok(not os.path.exists(os.path.join(FB, "big.bin")), "a delete on A removes the file on B")
    s, d = call("GET", f"/api/ms/drive/drives/{did}", token=utok)
    print("   drive on the server:", d.get("used"), "bytes,", d.get("files"), "files, rev", d.get("rev"), "online", d.get("online"))
    ok(s == 200 and d["files"] == 3, f"the node reported the drive ({d.get('files')} files)")
    # sharing (API.md section 17): a second account, PC-C, gets the drive read only, then editable
    email2 = f"cdtest2-{uuid.uuid4().hex[:8]}@example.com"; pw2 = secrets.token_urlsafe(16)
    s, r = call("POST", "/api/collections/users/records", {"email": email2, "password": pw2, "passwordConfirm": pw2, "name": "cd test 2"}); uid2 = r["id"]
    call("PATCH", f"/api/collections/users/records/{uid2}", {"verified": True, "approved": True}, token=su)
    s, r = call("POST", "/api/collections/users/auth-with-password", {"identity": email2, "password": pw2}); utok2 = r["token"]
    C, FC = os.path.join(tmp, "pcC"), os.path.join(tmp, "folderC")
    pair(C, utok2, "PC-C")
    rc, out = cli(A, "share", did, email2, "--read"); ok(rc == 0 and "read only" in out, "A shares it read only: " + out[:80])
    rc, out = cli(C, "drives"); ok(did in out and "shared by " + email in out and "(read only)" in out, "C lists the shared drive")
    rc, out = cli(C, "use", did, "--folder", FC); ok(rc == 0, out[:80])
    out = wait_sync(C, 40, "C first sync")
    ok(open(os.path.join(FC, "docs", "hello.txt")).read() == "A wins?\n" and "read only" in out, "C got the files, read only")
    open(os.path.join(FC, "fromC.txt"), "w").write("C"); os.remove(os.path.join(FC, "docs", "hello.txt"))
    out = wait_sync(C, 30, "C read only after its edits")
    ok(os.path.exists(os.path.join(FC, "docs", "hello.txt")) and "1 local changes not sent" in out, "a delete on C comes back, its new file stays local")
    out = wait_sync(A, 30)
    ok(not os.path.exists(os.path.join(FA, "fromC.txt")) and os.path.exists(os.path.join(FA, "docs", "hello.txt")), "nothing of C reached the drive")
    rc, out = cli(A, "share", did, email2); ok(rc == 0 and "can edit" in out, "A lets C edit: " + out[:80])
    out = wait_sync(C, 40, "C editable")
    out = wait_sync(A, 40)
    ok(os.path.exists(os.path.join(FA, "fromC.txt")), "now C's file reaches A")
    rc, out = cli(C, "leave", did); ok(rc == 0, "C leaves: " + out[:60])
    rc, out = cli(C, "drives"); ok(did not in out, "gone from C's drives")
    print("ALL OK (ChronoDrive end to end on node " + NODE + ")")
finally:
    if did and A in HOMES:
        cli(A, "remove", did)   # the drive letter goes
    if did:
        call("DELETE", f"/api/ms/drive/drives/{did}", token=utok)
    call("DELETE", f"/api/collections/users/records/{uid}", token=su)
    if uid2:
        call("DELETE", f"/api/collections/users/records/{uid2}", token=su)
    shutil.rmtree(tmp, ignore_errors=True)
    print("cleaned up (the drive is marked deleting: the office agent removes its folder)")

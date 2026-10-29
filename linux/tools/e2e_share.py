"""End-to-end test of sharing with the Linux client (API.md section 17 "Sharing"): two throwaway accounts, two Linux
"PCs" in WSL (their own XDG folders), one drive of the owner on a node. The owner shares it read only from the
console; the member sees "shared by ... (read only)", mounts it, gets the owner's files, and its own changes stay on
its PC (a deleted file comes back, nothing goes up). Then the member leaves, the owner shares it again (can edit)
and unshares it. Everything is removed at the end.

Run on Windows: python linux\\tools\\e2e_share.py [node] [--installed]"""
import json, os, re, secrets, subprocess, sys, time, urllib.request, urllib.error, uuid

env = dict(l.strip().split("=", 1) for l in open(r"I:\LLMLab\Azure\miniserver\.secrets.env", encoding="utf-8") if "=" in l)
B = env["MINISERVER_URL"].rstrip("/")
REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
args = [a for a in sys.argv[1:] if not a.startswith("--")]
NODE = args[0] if args else "office"
DISTRO = os.environ.get("WSL_DISTRO", "Ubuntu")


def call(method, path, body=None, token=None):
    req = urllib.request.Request(B + path, data=json.dumps(body).encode() if body is not None else None, method=method,
                                 headers=dict({"Content-Type": "application/json"}, **({"Authorization": token} if token else {})))
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


def wsl(*cmd, timeout=300):
    r = subprocess.run(["wsl.exe", "-d", DISTRO, "--", *cmd], capture_output=True, timeout=timeout)
    return r.returncode, (r.stdout + r.stderr).decode("utf-8", "replace").strip()


_, LHOME = wsl("bash", "-lc", "echo $HOME")
LT = f"{LHOME}/cd-share-{uuid.uuid4().hex[:8]}"
LCLI = f"{LHOME}/.local/bin/chronodrive" if "--installed" in sys.argv else "/mnt/" + REPO[0].lower() + REPO[2:].replace("\\", "/") + "/linux/chronodrive"


class PC:
    def __init__(self, name):
        self.name, self.base = name, f"{LT}/{name}"
        self.folder = f"{self.base}/drive"
        self.env = [f"XDG_CONFIG_HOME={self.base}/config", f"XDG_STATE_HOME={self.base}/state", f"XDG_DATA_HOME={self.base}/data", f"MINISERVER_URL={B}"]

    def cli(self, *a):
        return wsl("env", *self.env, "python3", LCLI, *a)

    def sh(self, script):
        return wsl("bash", "-c", f"cd '{self.folder}' && {script}")

    def read(self, rel):
        rc, o = wsl("cat", f"{self.folder}/{rel}")
        return o if rc == 0 else None

    def sync(self, label):
        rc, o = self.cli("sync", "--once")
        print(f"   {self.name} {label}: " + " | ".join(o.splitlines()[-4:]), flush=True)
        return rc, o

    def pair(self, utok):
        p = subprocess.Popen(["wsl.exe", "-d", DISTRO, "--", "env", *self.env, "python3", LCLI, "pair", "--name", self.name, "--no-qr"],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
        m = re.search(r"Pairing code ([A-Z0-9-]+)", p.stdout.readline())
        ok(m is not None and call("POST", "/api/ms/link/approve", {"code": m.group(1)}, token=utok)[0] == 200, f"{self.name}: pairing approved")
        o = p.communicate(timeout=90)[0]
        ok(p.returncode == 0 and "Linked to" in o, f"{self.name}: paired")


s, r = call("POST", "/api/collections/_superusers/auth-with-password", {"identity": env["PB_SUPERUSER_EMAIL"], "password": env["PB_SUPERUSER_PASSWORD"]})
su = r["token"]
users = []


def user(tag):
    email, pw = f"cdshare-{tag}-{uuid.uuid4().hex[:6]}@example.com", secrets.token_urlsafe(16)
    s, r = call("POST", "/api/collections/users/records", {"email": email, "password": pw, "passwordConfirm": pw, "name": f"cd share {tag}"})
    ok(s == 200, f"user {tag}")
    users.append(r["id"])
    ok(call("PATCH", f"/api/collections/users/records/{r['id']}", {"verified": True, "approved": True}, token=su)[0] == 200, f"user {tag} verified and approved")
    return email, call("POST", "/api/collections/users/auth-with-password", {"identity": email, "password": pw})[1]["token"]


did, otok = "", ""
try:
    oem, otok = user("owner")
    mem, mtok = user("member")
    O, M = PC("owner-pc"), PC("member-pc")
    wsl("mkdir", "-p", O.base, M.base)
    O.pair(otok)
    M.pair(mtok)
    s, d = call("POST", "/api/ms/drive/drives", {"name": "cd share test", "node": NODE, "size_gb": 1}, token=otok)
    ok(s == 200, f"owner's drive on {NODE}")
    did = d["id"]
    rc, o = O.cli("share", "cd share test", mem, "--read")
    ok(rc == 0 and "read only" in o, "owner: " + o)
    rc, o = O.cli("drives")
    ok(rc == 0 and f"with {mem} (read)" in o, "owner's list shows the member")
    rc, o = M.cli("drives")
    print("   " + "\n   ".join(o.splitlines()))
    ok(rc == 0 and f"shared by {oem} (read only)" in o, "member's list: shared by the owner, read only")
    ok(O.cli("mount", did, O.folder)[0] == 0, "owner mounted it")
    rc, o = M.cli("mount", "cd share", M.folder)
    ok(rc == 0 and "read only" in o, "member mounted it: " + o.splitlines()[-1])
    rc, o = M.cli("share", did, oem)
    ok(rc != 0 and "only the owner" in o, "the member cannot share it: " + o)
    print("   waiting 75 s: the node agent makes the drive root and starts serving it", flush=True)
    time.sleep(75)

    O.sh("mkdir -p docs && printf 'from the owner' > docs/a.txt && printf keep > b.txt")
    rc, o = O.sync("upload")
    ok(rc == 0 and "Up to date" in o, "owner uploaded")
    rc, o = M.sync("download")
    ok(rc == 0 and M.read("docs/a.txt") == "from the owner" and M.read("b.txt") == "keep" and "read only" in o, "member got the owner's files (read only)")

    M.sh("rm b.txt && printf mine > mine.txt && printf 'changed by the member' > docs/a.txt")
    rc, o = M.sync("local changes")
    ok(rc == 0 and M.read("b.txt") == "keep", "a file the member deleted came back from the drive")
    ok(M.read("mine.txt") == "mine" and M.read("docs/a.txt") == "changed by the member" and "2 local changes not uploaded" in o,
       "the member's new and changed files stay on its PC, counted: " + o.splitlines()[-1])
    rc, o = M.cli("status")
    print("   " + "\n   ".join(o.splitlines()[4:8]))
    ok(f"shared by {oem}, read only" in o, "member's status says shared by the owner, read only")
    ok("rev 3" in o, "nothing went up from the member: the drive is still at rev 3 (a folder and two files)")
    rc, o = O.sync("after the member")
    ok(O.read("mine.txt") is None and O.read("docs/a.txt") == "from the owner", "the owner's copy is untouched")

    O.sh("printf 'owner again' > docs/a.txt")
    O.sync("edit")
    rc, o = M.sync("owner's edit over a local change")
    rc, ls = M.sh("ls docs")
    ok(M.read("docs/a.txt") == "owner again" and any("conflict" in n for n in ls.splitlines()), f"a drive change over the member's edit: the drive wins, the member's version kept as a copy ({ls.splitlines()})")

    rc, o = M.cli("leave", "cd share test")
    ok(rc == 0 and "You left" in o, "member: " + o)
    rc, o = M.cli("drives")
    ok(did not in o, "the drive is gone from the member's list")
    rc, o = M.cli("status")
    ok("cd share test" not in o, "and from its mounts")
    rc, o = O.cli("share", did, mem)
    ok(rc == 0 and "can edit" in o, "owner shares it again, can edit: " + o)
    rc, o = O.cli("unshare", did, mem)
    ok(rc == 0 and "no longer shared" in o, "owner: " + o)
    s, r = call("GET", f"/api/ms/drive/drives/{did}/members", token=otok)
    ok(s == 200 and not r.get("members"), "no members left")
    print(f"ALL OK (ChronoDrive Linux sharing end to end on node {NODE})")
finally:
    if did:
        call("DELETE", f"/api/ms/drive/drives/{did}", token=otok)
    for u in users:
        call("DELETE", f"/api/collections/users/records/{u}", token=su)
    if "/cd-share-" in LT:
        wsl("rm", "-rf", LT)
    print("cleaned up (drive marked deleting, test users gone)")

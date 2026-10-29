"""HTTP (http.client, streamed bodies, pinned home-node certificates), the Miniserver control plane (pairing, device
token, standby retry: API.md sections 2 and 11) and the drive service client (API.md section 17)."""
import hashlib
import http.client
import json
import os
import ssl
import time
import urllib.parse

from . import common
from .common import log

UA = "ChronoDrive-Linux/1"


class Response:
    def __init__(self, status=0, body=b"", headers=None, error=""):
        self.status, self.body, self.headers, self.error = status, body, headers or {}, error

    @property
    def ok(self):
        return 200 <= self.status < 300

    @property
    def transport(self):
        """"Try the standby" (API.md section 11): no answer, or the proxy says the server is down."""
        return self.status in (0, 502, 503, 504) and not self.read_only

    @property
    def read_only(self):
        return self.status == 503 and b"standby_read_only" in self.body

    def header(self, k):
        return self.headers.get(k.lower(), "")

    def json(self):
        try:
            return json.loads(self.body) if self.body else {}
        except ValueError:
            return None

    def message(self):
        j = self.json()
        if isinstance(j, dict):
            m = j.get("message") or j.get("error") or ""
            if isinstance(j.get("data"), dict) and self.status == 423:
                m = (m + " " + ",".join(j["data"].keys())).strip()
            return str(m)
        return self.body[:200].decode("utf-8", "replace")

    def describe(self):
        if not self.status:
            return self.error or "no answer"
        m = self.message()
        return f"http {self.status}" + (f": {m}" if m else "")


class HttpError(Exception):
    def __init__(self, resp):
        super().__init__(resp.describe())
        self.status = resp.status
        self.resp = resp


class Revoked(HttpError):
    """403 on /api/ms/device/token: the user revoked this PC on the dashboard."""


def _context(pin_pem, pinned):
    if not pinned:
        return ssl.create_default_context()
    # a home node's own certificate (self-signed, CA:TRUE): trusted as the only CA, and its exact SHA-256 is compared
    # after the handshake. No name check: the address can be a VPN one (CHRONODRIVE_NODE_URL) the SAN does not list;
    # the pin is stricter than a name.
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    if pin_pem:
        try:
            ctx.load_verify_locations(cadata=pin_pem)
            ctx.verify_mode = ssl.CERT_REQUIRED
        except (ssl.SSLError, ValueError):
            pass   # a pem that does not load: the pin alone decides
    return ctx


def request(method, url, headers=None, body=None, body_file=None, out_file=None, out_append=0, timeout=20,
            pin_sha="", pin_pem=""):
    """One HTTP request. body = bytes; body_file = (path, offset, length) streamed; out_file = stream a 2xx body there
    (truncated at out_append first: Range resume). pin_sha = trust only that certificate (DER SHA-256, hex)."""
    u = urllib.parse.urlsplit(url)
    path = (u.path or "/") + ("?" + u.query if u.query else "")
    https = u.scheme == "https"
    try:
        if https:
            conn = http.client.HTTPSConnection(u.hostname, u.port or 443, timeout=timeout, context=_context(pin_pem, bool(pin_sha)))
        else:
            conn = http.client.HTTPConnection(u.hostname, u.port or 80, timeout=timeout)
    except (ValueError, OSError) as e:
        return Response(error=f"bad url {url}: {e}")
    try:
        conn.connect()
        if https and pin_sha:
            der = conn.sock.getpeercert(binary_form=True)
            if not der or hashlib.sha256(der).hexdigest() != pin_sha.lower():
                return Response(error="certificate pin mismatch (the node's certificate is not the one the server handed out)")
        h = {"User-Agent": UA, "Accept": "application/json"}
        h.update(headers or {})
        length = len(body) if body is not None else (body_file[2] if body_file else 0)
        if body is not None or body_file or method in ("POST", "PUT", "PATCH"):
            h["Content-Length"] = str(length)
        conn.putrequest(method, path, skip_accept_encoding=True)
        for k, v in h.items():
            conn.putheader(k, v)
        conn.endheaders()
        if body:
            conn.send(body)
        elif body_file and length:
            with open(body_file[0], "rb") as f:
                f.seek(body_file[1])
                left = length
                while left:
                    b = f.read(min(1 << 20, left))
                    if not b:
                        raise OSError("the file got shorter while it was sent")
                    conn.send(b)
                    left -= len(b)
        resp = conn.getresponse()
        r = Response(resp.status, b"", {k.lower(): v for k, v in resp.getheaders()})
        if out_file and r.ok:
            mode = "r+b" if os.path.exists(out_file) else "wb"
            with open(out_file, mode) as f:
                f.seek(out_append)
                f.truncate()
                while True:
                    b = resp.read(1 << 20)
                    if not b:
                        break
                    f.write(b)
                f.flush()
                os.fsync(f.fileno())
        else:
            r.body = resp.read(16 << 20) if method != "HEAD" else b""
        return r
    except (OSError, http.client.HTTPException, ssl.SSLError) as e:
        return Response(error=f"{type(e).__name__}: {e}")
    finally:
        conn.close()


def request_json(method, url, body=None, headers=None, timeout=20):
    h = {"Content-Type": "application/json"}
    h.update(headers or {})
    return request(method, url, h, json.dumps(body).encode() if body is not None else None, timeout=timeout)


# ------------------------------------------------------------------ the Miniserver (API.md sections 2 and 11)
def _host(url):
    return urllib.parse.urlsplit(url).hostname or ""


def server_list(base, configured_standby=""):
    """The base first, then where to fail over: MINISERVER_STANDBY_URL > the servers cached from /api/ms/info >
    the configured standby > the default standby (only for the two primary names)."""
    out = [base]
    env = os.environ.get("MINISERVER_STANDBY_URL", "").rstrip("/")
    cached = (common.read_json(os.path.join(common.state_dir(), "servers.json"), {}) or {}).get(_host(base), {})
    if env:
        out.append(env)
    elif cached.get("servers"):
        out += [s.rstrip("/") for s in cached["servers"]]
    elif configured_standby:
        out.append(configured_standby.rstrip("/"))
    elif _host(base) in common.PRIMARY_NAMES:
        out.append(common.DEFAULT_STANDBY)
    seen, res = set(), []
    for s in out:
        if s and s not in seen:
            seen.add(s)
            res.append(s)
    return res


def refresh_servers(base, force=False):
    """At start and every 24 h: GET <seed>/api/ms/info and cache its `servers` per seed host."""
    p = os.path.join(common.state_dir(), "servers.json")
    cache = common.read_json(p, {}) or {}
    ent = cache.get(_host(base), {})
    if not force and time.time() - ent.get("at", 0) < 86400:
        return
    for s in server_list(base):
        r = request("GET", s + "/api/ms/info", timeout=4)
        j = r.json() if r.ok else None
        if isinstance(j, dict):
            servers = j.get("servers") or [x for x in (j.get("url"), j.get("standby")) if x]
            cache[_host(base)] = {"servers": servers, "at": time.time()}
            try:
                common.write_atomic(p, json.dumps(cache))
            except OSError:
                pass
            return


class Miniserver:
    BACK_TO_PRIMARY = 300   # after a failover, try the primary first again after 5 minutes

    def __init__(self, cfg, secret):
        self.base = cfg.url
        self.servers = server_list(cfg.url, cfg.standby)
        self.device_id, self.secret = cfg.device_id, secret
        self.active, self.failed_at = 0, 0
        self.token_, self.expires = "", 0

    @property
    def linked(self):
        return bool(self.device_id and self.secret)

    def _order(self):
        if self.active and time.time() - self.failed_at > self.BACK_TO_PRIMARY:
            self.active = 0
        n = len(self.servers)
        return [(self.active + i) % n for i in range(n)]

    def _each(self, fn):
        r = Response(error="no server")
        for idx in self._order():
            r = fn(self.servers[idx])
            if not r.transport:
                if idx != self.active:
                    self.active, self.failed_at = idx, time.time()
                return r
            log(f"miniserver: {self.servers[idx]} -> {r.describe()}")
        return r

    def token(self):
        """The 12 h device token. Raises HttpError: 403 = this device was revoked, 423 = the account is not usable yet."""
        if self.token_ and time.time() < self.expires - 300:
            return self.token_
        r = self._each(lambda s: request_json("POST", s + "/api/ms/device/token", {"device_id": self.device_id, "device_secret": self.secret}))
        if r.status == 403:
            raise Revoked(r)
        if not r.ok:
            raise HttpError(r)
        j = r.json() or {}
        self.token_, self.expires = j.get("token", ""), time.time() + int(j.get("expires_in", 43200))
        return self.token_

    def call(self, method, path, body=None):
        tok = self.token()
        r = self._each(lambda s: request_json(method, s + path, body, {"Authorization": tok}))
        if r.status == 401:
            self.token_ = ""
            tok = self.token()
            r = self._each(lambda s: request_json(method, s + path, body, {"Authorization": tok}))
        return r

    def json(self, method, path, body=None):
        r = self.call(method, path, body)
        if not r.ok:
            raise HttpError(r)
        j = r.json()
        if j is None:
            raise HttpError(Response(r.status, b"", error="bad json"))
        return j

    # pairing (no auth, primary only: /api/ms/link/* waits for the primary)
    @staticmethod
    def link_start(base, name):
        return request_json("POST", base + "/api/ms/link/start", {"name": name, "platform": "linux", "app": common.APP_ID})

    @staticmethod
    def link_poll(base, code, secret):
        return request_json("POST", base + "/api/ms/link/poll", {"code": code, "secret": secret})


def describe_error(e):
    """A readable line for the console."""
    if isinstance(e, Revoked):
        return "this PC was unlinked on the Miniserver: run 'chronodrive pair' again"
    if isinstance(e, HttpError):
        if e.status == 423:
            return "the account is not usable yet (423: " + e.resp.message() + "): verify the email / wait for approval"
        return str(e)
    return f"{type(e).__name__}: {e}"


# ------------------------------------------------------------------ the drive service (API.md section 17)
def enc_path(p):
    return urllib.parse.quote(p, safe="/-_.~")


class DriveClient:
    def __init__(self, base="", grant="", pin="", pem=""):
        self.base, self.grant, self.pin, self.pem = base, grant, pin, pem

    def _rq(self, method, path, headers=None, timeout=30, **kw):
        h = {"Authorization": "Drive " + self.grant}
        h.update(headers or {})
        return request(method, self.base + path, h, timeout=timeout, pin_sha=self.pin, pin_pem=self.pem, **kw)

    @staticmethod
    def _j(r):
        if not r.ok:
            raise HttpError(r)
        j = r.json()
        if j is None:
            raise HttpError(Response(r.status, b"", error="bad json"))
        return j

    def info(self, timeout=10):
        return self._j(self._rq("GET", "", timeout=timeout))

    def changes(self, since, wait=0):
        return self._j(self._rq("GET", f"/changes?since={int(since)}&wait={int(wait)}", timeout=wait + 30))

    def download(self, path, part, have):
        h = {"Range": f"bytes={have}-"} if have else {}
        r = self._rq("GET", "/files/" + enc_path(path), h, timeout=120, out_file=part, out_append=have)
        if have and r.status == 200:
            log(f"download {path}: the server ignored the range, written from 0")   # out_append truncated at `have`: redo
            r = self._rq("GET", "/files/" + enc_path(path), {}, timeout=120, out_file=part, out_append=0)
        if not r.ok:
            raise HttpError(r)

    def put_small(self, path, local, size, sha, mtime, base_rev):
        return self._rq("PUT", "/files/" + enc_path(path), {"Content-Type": "application/octet-stream", "X-Drive-Base-Rev": str(base_rev),
                        "X-Drive-Mtime": str(mtime), "X-Drive-Sha256": sha}, timeout=120, body_file=(local, 0, size))

    def upload_start(self, path, size, sha, mtime, base_rev):
        return self._rq("POST", "/uploads", {"Content-Type": "application/json"},
                        body=json.dumps({"path": path, "size": size, "sha256": sha, "mtime": mtime, "base_rev": base_rev}).encode())

    def upload_offset(self, upload):
        r = self._rq("HEAD", "/uploads/" + upload)
        return int(r.header("Upload-Offset") or 0) if r.ok else -1

    def upload_chunk(self, upload, local, offset, length):
        return self._rq("PATCH", "/uploads/" + upload, {"Content-Type": "application/offset+octet-stream", "Upload-Offset": str(offset)},
                        timeout=300, body_file=(local, offset, length))

    def upload_commit(self, upload, base_rev):
        return self._rq("POST", f"/uploads/{upload}/commit", {"Content-Type": "application/json"}, timeout=300,
                        body=json.dumps({"base_rev": base_rev}).encode())

    def delete(self, path, base_rev):
        return self._rq("DELETE", "/files/" + enc_path(path), {"X-Drive-Base-Rev": str(base_rev)})

    def mkdir(self, path):
        return self._j(self._rq("POST", "/mkdir", {"Content-Type": "application/json"}, body=json.dumps({"path": path}).encode()))


def grant_for(ms, drive_id, device):
    """POST /grant -> (grant json, [DriveClient candidates in order]). CHRONODRIVE_NODE_URL=https://host:port replaces the
    node's address (a home node over the user's own VPN, tests); the pinned certificate still has to match."""
    g = ms.json("POST", f"/api/ms/drive/drives/{drive_id}/grant", {"device": device})
    urls = g.get("urls") or []
    over = os.environ.get("CHRONODRIVE_NODE_URL", "").rstrip("/")
    if over:
        urls = [{"url": f"{over}/drive/{drive_id}", "lan": bool(g.get("cert_sha256")), "nearby": False}]
    urls = sorted(urls, key=lambda u: not u.get("nearby"))   # this network's address first
    cands = []
    for u in urls:
        lan = bool(u.get("lan"))
        cands.append(DriveClient(u.get("url", "").rstrip("/"), g.get("grant", ""), g.get("cert_sha256", "") if lan else "",
                                 g.get("cert_pem", "") if lan else ""))
    return g, cands


def reach(cands, timeout=10):
    """The first candidate that answers GET / -> (DriveClient, info) or (None, last error)."""
    err = "the drive's node has no address yet (a home node: is it on and reporting?)" if not cands else ""
    for dc in cands:
        try:
            return dc, dc.info(timeout=timeout)
        except HttpError as e:
            if e.status == 403:
                raise
            err = f"{dc.base}: {e}"
    return None, err

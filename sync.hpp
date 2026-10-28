// sync.hpp — ChronoDrive: the sync engine. One local folder <-> one drive on a Miniserver node (API.md section 17).
//   index.json   what this PC last saw of every path: size, mtime, sha256, the drive's rev (the base rev of the next write)
//   remote pass  the drive's change feed since the index rev: downloads, folders, deletes (to the Recycle Bin), conflicts
//   local pass   the folder against the index: uploads (small put or chunked), deletes, folders
// Rules: a remote change over an untouched local file wins; a local change over an untouched remote file wins; both
// changed = the remote version keeps the name and the local one is kept (and uploaded) as "name (conflict PC date).ext".
// Nothing is ever lost silently. Design: the Miniserver's docs/chronodrive.md section 3.4.
#pragma once
#include "msclient.hpp"
#include <shellapi.h>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <deque>
#include <set>

namespace cd {

struct IndexEntry { bool dir = false; uint64_t size = 0; int64_t mtime = 0; std::string sha256; int64_t rev = 0; };
struct Index {
  int64_t rev = 0;                                   // the drive rev this index is in step with
  std::map<std::string, IndexEntry> files;           // drive paths ("a/b.txt")
  std::map<std::string, std::string> uploads;        // path -> upload id of a chunked upload in progress (resumed after a restart)
  bool Load() {
    std::string s; if (!ReadTextFile(IndexPath(), s)) return false;
    try { json j = json::parse(s); rev = j.value("rev", (int64_t)0);
      for (auto& el : j.value("files", json::object()).items()) { IndexEntry e; auto& v = el.value(); e.dir = v.value("dir", false); e.size = v.value("size", (uint64_t)0); e.mtime = v.value("mtime", (int64_t)0); e.sha256 = v.value("sha256", ""); e.rev = v.value("rev", (int64_t)0); files[el.key()] = e; }
      for (auto& el : j.value("uploads", json::object()).items()) uploads[el.key()] = el.value().get<std::string>();
    } catch (...) { files.clear(); rev = 0; return false; }
    return true;
  }
  bool Save() const {
    json f = json::object(); for (auto& [p, e] : files) f[p] = {{"dir", e.dir}, {"size", e.size}, {"mtime", e.mtime}, {"sha256", e.sha256}, {"rev", e.rev}};
    json u = json::object(); for (auto& [p, id] : uploads) u[p] = id;
    return WriteTextFile(IndexPath(), json({{"rev", rev}, {"files", f}, {"uploads", u}}).dump());
  }
};

struct RecentChange { time_t at; std::string what, path, by; };
struct SyncStatus {
  std::string state = "setup";    // setup | connecting | syncing | idle | paused | offline | error
  std::string detail, nodeUrl;
  int pending = 0; uint64_t bytesDone = 0, bytesTotal = 0;
  time_t lastSync = 0;
  std::deque<RecentChange> recent;
};

// ------------------------------------------------------------------ local folder helpers
struct LocalFile { std::string path; bool dir = false; uint64_t size = 0; int64_t mtime = 0; };
inline int64_t FtToMs(const FILETIME& ft) { ULARGE_INTEGER u{ft.dwLowDateTime, ft.dwHighDateTime}; return (int64_t)(u.QuadPart / 10000ull) - 11644473600000ll; }
inline FILETIME MsToFt(int64_t ms) { ULARGE_INTEGER u; u.QuadPart = (ULONGLONG)(ms + 11644473600000ll) * 10000ull; FILETIME ft{u.LowPart, u.HighPart}; return ft; }
inline bool Ignored(const std::wstring& name) {
  std::wstring l = LowerW(name);
  if (l == L"desktop.ini" || l == L"thumbs.db" || l == L".msdrive" || l == L".ds_store") return true;
  if (l.rfind(L"~$", 0) == 0) return true;
  size_t d = l.find_last_of(L'.'); std::wstring ext = d == std::wstring::npos ? L"" : l.substr(d);
  return ext == L".tmp" || ext == L".part" || ext == L".crdownload" || ext == L".lnk" && false;
}
inline std::wstring ToLocal(const std::wstring& root, const std::string& drivePath) { std::wstring p = Wide(drivePath); for (auto& c : p) if (c == L'/') c = L'\\'; return JoinPath(root, p); }
inline void ScanDir(const std::wstring& root, const std::wstring& rel, std::vector<LocalFile>& out) {
  std::wstring dir = rel.empty() ? root : JoinPath(root, rel);
  WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileExW((LongPath(dir) + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || Ignored(fd.cFileName)) continue;
    if (fd.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_OFFLINE)) continue;
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) && (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)) continue;
    std::wstring r = rel.empty() ? fd.cFileName : rel + L"\\" + fd.cFileName;
    LocalFile f; f.path = Utf8(r); for (auto& c : f.path) if (c == '\\') c = '/';
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { f.dir = true; out.push_back(f); ScanDir(root, r, out); }
    else { f.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow; f.mtime = FtToMs(fd.ftLastWriteTime); out.push_back(f); }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}
inline bool StatLocal(const std::wstring& p, LocalFile& f) {
  WIN32_FILE_ATTRIBUTE_DATA fa{}; if (!GetFileAttributesExW(LongPath(p).c_str(), GetFileExInfoStandard, &fa)) return false;
  f.dir = (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0; f.size = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow; f.mtime = FtToMs(fa.ftLastWriteTime); return true;
}
inline void SetMtime(const std::wstring& p, int64_t ms) {
  HANDLE h = CreateFileW(LongPath(p).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return; FILETIME ft = MsToFt(ms); SetFileTime(h, nullptr, nullptr, &ft); CloseHandle(h);
}
inline bool Recycle(const std::wstring& p) {   // to the Recycle Bin (undo possible); a plain delete when the shell refuses
  std::wstring dbl = p; dbl.push_back(0); dbl.push_back(0);
  SHFILEOPSTRUCTW op{}; op.wFunc = FO_DELETE; op.pFrom = dbl.c_str(); op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
  if (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted) return true;
  return DeleteFileW(LongPath(p).c_str()) || RemoveDirectoryW(LongPath(p).c_str());
}
inline std::string ConflictName(const std::string& path, const std::string& pc) {
  size_t slash = path.find_last_of('/'); std::string dir = slash == std::string::npos ? "" : path.substr(0, slash + 1), name = slash == std::string::npos ? path : path.substr(slash + 1);
  size_t dot = name.find_last_of('.'); std::string stem = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot), ext = dot == std::string::npos || dot == 0 ? "" : name.substr(dot);
  return dir + stem + " (conflict " + pc + " " + StampLocal(Now()) + ")" + ext;
}

// ------------------------------------------------------------------ the engine
class Engine {
 public:
  explicit Engine(Config cfg, std::string deviceSecret) : m_cfg(std::move(cfg)), m_secret(std::move(deviceSecret)) {}
  ~Engine() { Stop(); }
  void Start() { if (m_run) return; m_run = true; m_worker = std::thread([this] { Loop(); }); }
  void Stop() { m_run = false; Kick(); if (m_worker.joinable()) m_worker.join(); if (m_watcher.joinable()) m_watcher.join(); if (m_poller.joinable()) m_poller.join(); }
  void Pause(bool on) { m_paused = on; Kick(); }
  bool Paused() const { return m_paused; }
  void Kick() { { std::lock_guard<std::mutex> lk(m_mx); m_dirty = true; } m_cv.notify_all(); }
  SyncStatus Status() { std::lock_guard<std::mutex> lk(m_smx); return m_status; }
  const Config& Cfg() const { return m_cfg; }

 private:
  Config m_cfg; std::string m_secret;
  std::atomic<bool> m_run{false}, m_paused{false}; bool m_dirty = true;
  std::mutex m_mx; std::condition_variable m_cv;
  std::thread m_worker, m_watcher, m_poller;
  std::mutex m_smx; SyncStatus m_status;
  Index m_idx; DriveClient m_dc; std::string m_grantUrls; time_t m_grantExp = 0, m_lastUrlCheck = 0; json m_urls; std::string m_certSha;
  std::wstring m_root; std::string m_pc;

  void SetState(const std::string& st, const std::string& detail = "") { std::lock_guard<std::mutex> lk(m_smx); m_status.state = st; m_status.detail = detail; }
  void Note(const std::string& what, const std::string& path, const std::string& by = "") {
    std::lock_guard<std::mutex> lk(m_smx); m_status.recent.push_front({Now(), what, path, by}); while (m_status.recent.size() > 20) m_status.recent.pop_back();
  }
  void Progress(int pending, uint64_t done, uint64_t total) { std::lock_guard<std::mutex> lk(m_smx); m_status.pending = pending; m_status.bytesDone = done; m_status.bytesTotal = total; }

  // ---- grant and the node's url (every 10 minutes, and whenever a call fails)
  bool Connect(std::string& err) {
    Miniserver ms(m_cfg.url, m_cfg.standby, m_cfg.device_id, m_secret);
    if (m_dc.grant.empty() || Now() > m_grantExp - 3600 || Now() - m_lastUrlCheck > 600) {
      json g = ms.Json(L"POST", "/api/ms/drive/drives/" + m_cfg.drive_id + "/grant", json({{"device", m_pc}}), err);
      if (g.is_null()) { if (err.find("http 403") != std::string::npos || err.find("http 404") != std::string::npos || err == "revoked") { SetState("error", err == "revoked" ? "This PC was unlinked on the Miniserver: pair it again." : "The drive is disabled or gone: " + err); return false; } SetState("offline", "Miniserver: " + err); return false; }
      m_dc.grant = g.value("grant", ""); m_grantExp = g.value("exp", (int64_t)0); m_urls = g.value("urls", json::array()); m_certSha = g.value("cert_sha256", ""); m_lastUrlCheck = Now();
    }
    // the first url that answers: a LAN address needs the node's pinned certificate, a public one the normal chain
    for (auto& u : m_urls) {
      DriveClient dc; dc.grant = m_dc.grant; dc.base = u.value("url", ""); dc.pin = u.value("lan", false) ? m_certSha : "";
      std::string e; json info = dc.Info(e);
      if (!info.is_null()) { if (m_dc.base != dc.base) Log("drive at " + dc.base); m_dc = dc; { std::lock_guard<std::mutex> lk(m_smx); m_status.nodeUrl = dc.base; } return true; }
      if (e.find("http 403") != std::string::npos) { SetState("error", "The drive refuses this PC: " + e); return false; }
      Log("drive url " + dc.base + ": " + e);
    }
    err = m_urls.empty() ? "the drive's node has no address yet (a home node: is it on and reporting?)" : "the drive's node does not answer (a home drive syncs only on its network)";
    SetState("offline", err); m_lastUrlCheck = 0;   // ask the primary again next time: the node's address may have changed
    return false;
  }

  // ---- remote pass: the change feed since the index rev
  bool PullRemote(std::string& err) {
    for (;;) {
      json r = m_dc.Changes(m_idx.rev, 0, err);
      if (r.is_null()) return false;
      std::vector<Entry> list; for (auto& c : r.value("changes", json::array())) list.push_back(EntryOf(c));
      if (list.empty()) { m_idx.rev = (std::max)(m_idx.rev, r.value("rev", m_idx.rev)); return true; }
      Progress((int)list.size(), 0, 0);
      for (auto& e : list) { if (!m_run) return false; Apply(e); m_idx.rev = (std::max)(m_idx.rev, e.rev); }
      m_idx.Save();
      if (!r.value("more", false)) return true;
    }
  }
  bool LocalUnchanged(const std::string& path, const IndexEntry& ie, bool* exists) {
    LocalFile f; bool ok = StatLocal(ToLocal(m_root, path), f); if (exists) *exists = ok;
    if (!ok) return true;   // gone = nothing of ours to protect
    if (ie.dir) return f.dir;
    return !f.dir && f.size == ie.size && f.mtime / 1000 == ie.mtime / 1000;
  }
  void Apply(const Entry& e) {
    auto it = m_idx.files.find(e.path);
    if (it != m_idx.files.end() && e.rev <= it->second.rev) return;   // ours, or already seen
    std::wstring local = ToLocal(m_root, e.path);
    if (e.deleted) {
      bool exists = false, same = it == m_idx.files.end() ? true : LocalUnchanged(e.path, it->second, &exists);
      if (it == m_idx.files.end()) { return; }   // never had it
      if (exists && same) { Recycle(local); Note("deleted on " + (e.by.empty() ? std::string("the drive") : e.by), e.path, e.by); }
      else if (exists) Log("kept local changes of " + e.path + " (deleted on the drive): it goes up again");
      // entries under a deleted folder go too
      std::vector<std::string> under; for (auto& [p, ie] : m_idx.files) if (p == e.path || p.rfind(e.path + "/", 0) == 0) under.push_back(p);
      for (auto& p : under) m_idx.files.erase(p);
      return;
    }
    if (e.kind == "dir") {
      EnsureDir(local);
      IndexEntry ie; ie.dir = true; ie.rev = e.rev; m_idx.files[e.path] = ie; return;
    }
    // a file: download unless the local copy already is this content
    LocalFile f; bool exists = StatLocal(local, f);
    if (exists && !f.dir) {
      std::string sha; if ((it == m_idx.files.end() || !LocalUnchanged(e.path, it->second, nullptr)) && Sha256File(local, sha) && sha == e.sha256) {
        IndexEntry ie; ie.size = e.size; ie.mtime = e.mtime; ie.sha256 = e.sha256; ie.rev = e.rev; m_idx.files[e.path] = ie; SetMtime(local, e.mtime); return;
      }
      if (it != m_idx.files.end() && !LocalUnchanged(e.path, it->second, nullptr) || it == m_idx.files.end()) {
        // changed here too (or unknown to the index): keep ours as a conflict copy, it goes up in the local pass
        std::string cname = ConflictName(e.path, m_pc);
        if (MoveFileExW(LongPath(local).c_str(), LongPath(ToLocal(m_root, cname)).c_str(), MOVEFILE_REPLACE_EXISTING)) { Log("conflict on " + e.path + ": local copy kept as " + cname); Note("conflict, local copy kept", cname, e.by); }
      }
    }
    std::wstring part = local + L".part";
    size_t slash = local.find_last_of(L'\\'); if (slash != std::wstring::npos) EnsureDir(local.substr(0, slash));
    LocalFile pf; uint64_t have = StatLocal(part, pf) && !pf.dir ? pf.size : 0;
    std::string err; int status = 0;
    if (!m_dc.Download(e.path, part, have, err, &status)) {
      if (status == 404) { Log("download " + e.path + ": gone meanwhile"); return; }
      Log("download " + e.path + " failed: " + err); throw std::runtime_error(err);
    }
    std::string sha; uint64_t size = 0;
    if (!Sha256File(part, sha, &size) || (size != e.size) || (sha != e.sha256)) { DeleteFileW(LongPath(part).c_str()); Log("download " + e.path + ": hash mismatch, retried later"); throw std::runtime_error("hash mismatch on " + e.path); }
    if (!MoveFileExW(LongPath(part).c_str(), LongPath(local).c_str(), MOVEFILE_REPLACE_EXISTING)) { Log("cannot replace " + e.path + " (open in another program?): " + LastErrorText()); throw std::runtime_error("file busy: " + e.path); }
    SetMtime(local, e.mtime);
    IndexEntry ie; ie.size = e.size; ie.mtime = e.mtime; ie.sha256 = e.sha256; ie.rev = e.rev; m_idx.files[e.path] = ie;
    Note("downloaded", e.path, e.by);
  }

  // ---- local pass: the folder against the index
  bool PushLocal(std::string& err) {
    std::vector<LocalFile> files; ScanDir(m_root, L"", files);
    std::set<std::string> seen;
    std::vector<LocalFile> ups; uint64_t total = 0;
    for (auto& f : files) {
      seen.insert(f.path);
      auto it = m_idx.files.find(f.path);
      if (f.dir) { if (it == m_idx.files.end()) ups.push_back(f); continue; }
      if (it == m_idx.files.end() || it->second.dir || it->second.size != f.size || it->second.mtime / 1000 != f.mtime / 1000) { ups.push_back(f); total += f.size; }
    }
    std::vector<std::string> gone; for (auto& [p, ie] : m_idx.files) if (!seen.count(p)) gone.push_back(p);
    uint64_t done = 0; int pending = (int)ups.size() + (int)gone.size();
    Progress(pending, 0, total);
    // deletes first, deepest paths first (a folder after its files)
    std::sort(gone.begin(), gone.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    std::set<std::string> dropped;
    for (auto& p : gone) {
      if (!m_run) return false;
      if (dropped.count(p)) continue;
      auto ie = m_idx.files[p]; int status = 0; std::string e;
      json r = m_dc.Delete(p, ie.rev, e, &status);
      if (status == 200 || status == 404) { if (status == 200) Note("deleted", p); }
      else if (status == 412) { Log("delete " + p + ": changed on the drive meanwhile, it comes back down"); }
      else { err = e; return false; }
      for (auto& [q, x] : m_idx.files) if (q == p || q.rfind(p + "/", 0) == 0) dropped.insert(q);
      m_idx.files.erase(p); Progress(--pending, done, total);
    }
    for (auto& q : dropped) m_idx.files.erase(q);
    for (auto& f : ups) {
      if (!m_run) return false;
      std::string e;
      if (f.dir) { json r = m_dc.Mkdir(f.path, e); if (r.is_null()) { err = e; return false; } IndexEntry ie; ie.dir = true; ie.rev = r.value("rev", (int64_t)0); m_idx.files[f.path] = ie; Progress(--pending, done, total); continue; }
      if (!Upload(f, e)) { if (e == "busy") { Progress(--pending, done, total); continue; } err = e; return false; }
      done += f.size; Progress(--pending, done, total);
      m_idx.Save();
    }
    return true;
  }
  bool Upload(const LocalFile& f, std::string& err) {
    std::wstring local = ToLocal(m_root, f.path);
    std::string sha; uint64_t size = 0;
    if (!Sha256File(local, sha, &size)) { Log("cannot read " + f.path + " (open for writing?): retried later"); err = "busy"; return false; }
    auto it = m_idx.files.find(f.path); int64_t base = it == m_idx.files.end() || it->second.dir ? 0 : it->second.rev;
    if (it != m_idx.files.end() && !it->second.dir && it->second.sha256 == sha) {   // same content, only the date changed: nothing to send
      it->second.size = size; it->second.mtime = f.mtime; return true;
    }
    int status = 0; json r;
    if (size <= kSmallMax) r = m_dc.PutSmall(f.path, local, size, sha, f.mtime, base, err, &status);
    else {
      std::string up = m_idx.uploads.count(f.path) ? m_idx.uploads[f.path] : "";
      uint64_t off = 0;
      if (!up.empty()) { std::string e2; off = m_dc.UploadOffset(up, e2); if (off == (uint64_t)-1) { up.clear(); off = 0; } }
      if (up.empty()) { json s = m_dc.UploadStart(f.path, size, sha, f.mtime, base, err, &status); if (s.is_null()) { if (status == 412) return Conflict(f.path, s); return false; } up = s.value("upload", ""); m_idx.uploads[f.path] = up; m_idx.Save(); }
      while (off < size) {
        if (!m_run) { err = "stopped"; return false; }
        uint64_t len = (std::min)((uint64_t)kChunk, size - off), real = 0; std::string e2;
        if (!m_dc.UploadChunk(up, local, off, len, e2, &real)) { if (e2 == "offset mismatch") { off = real; continue; } err = e2; return false; }
        off += len; { std::lock_guard<std::mutex> lk(m_smx); m_status.bytesDone += len; }
      }
      r = m_dc.UploadCommit(up, base, err, &status);
      m_idx.uploads.erase(f.path);
      if (r.is_null() && status != 412) { if (status == 400 || status == 404) m_idx.uploads.erase(f.path); return false; }
    }
    if (status == 412) return Conflict(f.path, r);
    if (r.is_null()) return false;
    Entry e = EntryOf(r);
    IndexEntry ie; ie.size = size; ie.mtime = f.mtime; ie.sha256 = sha; ie.rev = e.rev; m_idx.files[f.path] = ie;
    Note(base ? "uploaded" : "added", f.path, m_pc);
    return true;
  }
  bool Conflict(const std::string& path, const json& r) {
    // the drive has a newer version: keep ours as a conflict copy (it goes up on the next pass) and forget the index
    // entry so the remote pass brings the drive's version down
    std::string cname = ConflictName(path, m_pc);
    if (MoveFileExW(LongPath(ToLocal(m_root, path)).c_str(), LongPath(ToLocal(m_root, cname)).c_str(), MOVEFILE_REPLACE_EXISTING)) { Log("conflict on " + path + ": local copy kept as " + cname); Note("conflict, local copy kept", cname); }
    m_idx.files.erase(path); m_idx.uploads.erase(path);
    (void)r; return true;
  }

  // ---- threads
  void Watch() {
    HANDLE h = CreateFileW(LongPath(m_root).c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) { Log("watcher: cannot open " + Utf8(m_root)); return; }
    std::vector<BYTE> buf(64 * 1024); OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    while (m_run) {
      ResetEvent(ov.hEvent);
      if (!ReadDirectoryChangesW(h, buf.data(), (DWORD)buf.size(), TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE, nullptr, &ov, nullptr)) break;
      while (m_run && WaitForSingleObject(ov.hEvent, 500) == WAIT_TIMEOUT) {}
      if (!m_run) { CancelIo(h); break; }
      DWORD n = 0; GetOverlappedResult(h, &ov, &n, FALSE);
      bool ours = false;
      if (n) { FILE_NOTIFY_INFORMATION* fi = (FILE_NOTIFY_INFORMATION*)buf.data(); ours = true;
        for (;;) { std::wstring name(fi->FileName, fi->FileNameLength / 2); if (name.size() < 5 || LowerW(name.substr(name.size() - 5)) != L".part") ours = false; if (!fi->NextEntryOffset) break; fi = (FILE_NOTIFY_INFORMATION*)((BYTE*)fi + fi->NextEntryOffset); } }
      if (!ours) { { std::lock_guard<std::mutex> lk(m_mx); m_dirty = true; m_dirtyAt = Now(); } m_cv.notify_all(); }
    }
    CloseHandle(ov.hEvent); CloseHandle(h);
  }
  void Poll() {   // long polls on the change feed: wakes the worker when the drive changed
    while (m_run) {
      if (m_paused || m_dc.grant.empty() || m_dc.base.empty()) { std::this_thread::sleep_for(std::chrono::seconds(2)); continue; }
      std::string err; int64_t since; { std::lock_guard<std::mutex> lk(m_mx); since = m_idx.rev; }
      DriveClient dc = m_dc; json r = dc.Changes(since, 30, err);
      if (r.is_null()) { std::this_thread::sleep_for(std::chrono::seconds(10)); continue; }
      if (!r.value("changes", json::array()).empty()) { { std::lock_guard<std::mutex> lk(m_mx); m_dirty = true; } m_cv.notify_all(); std::this_thread::sleep_for(std::chrono::seconds(2)); }
    }
  }
  time_t m_dirtyAt = 0;
  void Loop() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_root = m_cfg.folder; m_pc = ComputerName();
    m_idx.Load();
    if (m_cfg.drive_id.empty() || m_root.empty() || m_cfg.device_id.empty()) { SetState("setup", "Pair this PC and pick a drive and a folder."); return; }
    EnsureDir(m_root);
    m_watcher = std::thread([this] { Watch(); });
    m_poller = std::thread([this] { Poll(); });
    time_t lastFull = 0; int backoff = 30;
    while (m_run) {
      if (m_paused) { SetState("paused", "Sync paused."); std::unique_lock<std::mutex> lk(m_mx); m_cv.wait_for(lk, std::chrono::seconds(5)); continue; }
      std::string err;
      SetState("connecting", "Looking for the drive's node\xe2\x80\xa6");
      if (!Connect(err)) { std::unique_lock<std::mutex> lk(m_mx); m_cv.wait_for(lk, std::chrono::seconds(backoff)); backoff = (std::min)(300, backoff * 2); continue; }
      backoff = 30;
      SetState("syncing", "Checking the drive\xe2\x80\xa6");
      try {
        bool ok = PullRemote(err) && PushLocal(err);
        m_idx.Save();
        if (!ok) { Log("sync: " + err); SetState("offline", err); m_lastUrlCheck = 0; std::unique_lock<std::mutex> lk(m_mx); m_cv.wait_for(lk, std::chrono::seconds(30)); continue; }
      } catch (std::exception& ex) { m_idx.Save(); Log(std::string("sync: ") + ex.what()); SetState("offline", ex.what()); std::unique_lock<std::mutex> lk(m_mx); m_cv.wait_for(lk, std::chrono::seconds(30)); continue; }
      { std::lock_guard<std::mutex> lk(m_smx); m_status.lastSync = Now(); m_status.pending = 0; m_status.bytesDone = m_status.bytesTotal = 0; }
      SetState("idle", "Up to date");
      lastFull = Now();
      // wait for the watcher (2 s of quiet after the last event), the poller, or the 10-minute full scan
      std::unique_lock<std::mutex> lk(m_mx);
      m_dirty = false;
      for (;;) {
        m_cv.wait_for(lk, std::chrono::seconds(2));
        if (!m_run || m_paused) break;
        if (m_dirty && Now() - m_dirtyAt >= 2) break;
        if (Now() - lastFull >= 600) break;
      }
    }
    CoUninitialize();
  }
};

} // namespace cd

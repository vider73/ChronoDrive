// drives.hpp — ChronoDrive: the drives shown on this PC. Each Mapping (msclient.hpp) gets its own sync Engine
// (sync.hpp); a mapping in "letter" mode also gets a drive letter that points at its backing folder.
//
// Drive letters: DefineDosDevice (what `subst` does) in the user's logon session, so Explorer shows the letter with the
// drive's name and icon (HKCU\Software\Classes\Applications\Explorer.exe\Drives\<L>). The letter lasts until sign-out;
// the app defines it again when it starts. The files themselves are real files in the backing folder
// (%USERPROFILE%\ChronoDrive\<name>): nothing is fetched on demand.
#pragma once
#include "sync.hpp"
#include <shlobj.h>
#include <memory>

namespace cd {

// ------------------------------------------------------------------ letters
inline std::wstring LetterRoot(const std::wstring& L) { return L.substr(0, 1) + L":"; }
inline std::wstring LetterTarget(const std::wstring& L) {   // what the letter points at now ("" when free)
  wchar_t buf[2048]; DWORD n = QueryDosDeviceW(LetterRoot(L).c_str(), buf, 2048);
  if (!n) return L"";
  std::wstring t = buf; if (t.rfind(L"\\??\\", 0) == 0) t = t.substr(4);
  return t;
}
inline bool LetterInUse(const std::wstring& L) { return (GetLogicalDrives() >> (towupper(L[0]) - L'A')) & 1; }
// Letters a drive may take: D..Z not used by anything, plus `keep` (the one a mapping already has). Z first.
inline std::vector<std::wstring> FreeLetters(const std::wstring& keep = L"") {
  std::vector<std::wstring> v;
  DWORD used = GetLogicalDrives();
  for (wchar_t c = L'Z'; c >= L'D'; --c) {
    std::wstring L(1, c);
    if (!((used >> (c - L'A')) & 1) || (!keep.empty() && towupper(keep[0]) == c)) v.push_back(L);
  }
  return v;
}
inline std::wstring SafeName(const std::string& name) {
  std::wstring w = Wide(name); std::wstring o;
  for (wchar_t c : w) o += (wcschr(L"\\/:*?\"<>|", c) || c < 32) ? L'_' : c;
  while (!o.empty() && (o.back() == L'.' || o.back() == L' ')) o.pop_back();
  return o.empty() ? L"Drive" : o;
}
inline std::wstring BackingFolder(const std::string& name) {
  std::wstring home = EnvW(L"USERPROFILE"); if (home.empty()) home = DataDir();
  return JoinPath(JoinPath(home, L"ChronoDrive"), SafeName(name));
}
inline void ExplorerLabel(const std::wstring& L, const std::string& name, bool on) {
  std::wstring key = L"Software\\Classes\\Applications\\Explorer.exe\\Drives\\" + L.substr(0, 1);
  if (!on) { RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str()); return; }
  HKEY k;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, (key + L"\\DefaultLabel").c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
    std::wstring v = Wide(name); RegSetValueExW(k, nullptr, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t))); RegCloseKey(k);
  }
  wchar_t exe[MAX_PATH * 2]; GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
  if (RegCreateKeyExW(HKEY_CURRENT_USER, (key + L"\\DefaultIcon").c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
    std::wstring v = std::wstring(exe) + L",0"; RegSetValueExW(k, nullptr, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t))); RegCloseKey(k);
  }
}
// Points `L` at `folder` (created when missing). Fine when it already points there; refused when something else uses it.
inline bool MountLetter(const std::wstring& L, const std::wstring& folder, const std::string& name, std::string& err) {
  EnsureDir(folder);
  std::wstring now = LetterTarget(L);
  if (!now.empty()) {
    if (LowerW(now) == LowerW(folder)) { ExplorerLabel(L, name, true); return true; }
    err = Utf8(LetterRoot(L)) + " is already used by " + Utf8(now); return false;
  }
  if (LetterInUse(L)) { err = Utf8(LetterRoot(L)) + " is already used by another disk"; return false; }
  if (!DefineDosDeviceW(0, LetterRoot(L).c_str(), folder.c_str())) { err = "cannot create " + Utf8(LetterRoot(L)) + ": " + LastErrorText(); return false; }
  ExplorerLabel(L, name, true);
  std::wstring root = LetterRoot(L) + L"\\";
  SHChangeNotify(SHCNE_DRIVEADD, SHCNF_PATHW, root.c_str(), nullptr);
  Log("drive letter " + Utf8(LetterRoot(L)) + " -> " + Utf8(folder));
  return true;
}
inline void UnmountLetter(const std::wstring& L, const std::wstring& folder) {
  std::wstring now = LetterTarget(L);
  if (!now.empty() && LowerW(now) == LowerW(folder)) {
    DefineDosDeviceW(DDD_RAW_TARGET_PATH | DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE, LetterRoot(L).c_str(), (L"\\??\\" + folder).c_str());
    if (!LetterTarget(L).empty()) DefineDosDeviceW(DDD_REMOVE_DEFINITION, LetterRoot(L).c_str(), nullptr);
    std::wstring root = LetterRoot(L) + L"\\";
    SHChangeNotify(SHCNE_DRIVEREMOVED, SHCNF_PATHW, root.c_str(), nullptr);
    Log("drive letter " + Utf8(LetterRoot(L)) + " removed");
  }
  ExplorerLabel(L, "", false);
}
// Where the user sees a mapping: "Z:" or the folder.
inline std::wstring ShownAt(const Mapping& m) { return m.mode == "letter" && !m.letter.empty() ? LetterRoot(m.letter) : m.folder; }

// ------------------------------------------------------------------ the set of engines
class DriveSet {
 public:
  DriveSet(Config& cfg, std::string secret) : m_cfg(cfg), m_secret(std::move(secret)) {}
  ~DriveSet() { StopAll(); }
  void SetSecret(const std::string& s) { m_secret = s; }
  // Starts (or restarts) the engine of every mapping, defining the drive letters first. Returns problems to show.
  std::vector<std::string> StartAll() {
    std::vector<std::string> notes;
    for (auto& m : m_cfg.maps) { std::string n = Start(m); if (!n.empty()) notes.push_back(m.name + ": " + n); }
    return notes;
  }
  std::string Start(Mapping& m) {
    Stop(m.id);
    if (m.mode == "letter") {
      if (m.folder.empty()) m.folder = BackingFolder(m.name);
      std::string err;
      if (!m.letter.empty() && !MountLetter(m.letter, m.folder, m.name, err)) { Log("mount " + m.name + ": " + err); return err; }
    }
    if (m.folder.empty()) return "no folder";
    EnsureDir(m.folder);
    auto e = std::make_unique<Engine>(ForMapping(m_cfg, m), m_secret);
    e->Pause(m.paused); e->Start();
    m_eng[m.id] = std::move(e);
    return "";
  }
  void Stop(const std::string& id) { auto it = m_eng.find(id); if (it != m_eng.end()) { it->second->Stop(); m_eng.erase(it); } }
  void StopAll() { for (auto& [id, e] : m_eng) e->Stop(); m_eng.clear(); }
  // Takes a drive off this PC: the engine stops, the letter goes, the index is forgotten. The local files stay.
  void Remove(const std::string& id) {
    Stop(id);
    for (auto it = m_cfg.maps.begin(); it != m_cfg.maps.end(); ++it) {
      if (it->id != id) continue;
      if (it->mode == "letter" && !it->letter.empty()) UnmountLetter(it->letter, it->folder);
      DeleteFileW(IndexPathFor(id).c_str());
      m_cfg.maps.erase(it); break;
    }
  }
  void Pause(const std::string& id, bool on) { if (auto* m = m_cfg.Find(id)) m->paused = on; auto it = m_eng.find(id); if (it != m_eng.end()) it->second->Pause(on); }
  bool Running(const std::string& id) const { return m_eng.count(id) > 0; }
  SyncStatus Status(const std::string& id) { auto it = m_eng.find(id); return it == m_eng.end() ? SyncStatus{} : it->second->Status(); }
  void KickAll() { for (auto& [id, e] : m_eng) e->Kick(); }
  // The worst state across the drives, for the tray: error > offline > syncing > paused > idle.
  std::string Overall(int* pending = nullptr) {
    static const char* order[] = {"error", "offline", "connecting", "syncing", "paused", "idle", "setup"};
    std::string worst = m_eng.empty() ? "setup" : "idle"; int p = 0;
    auto rank = [&](const std::string& s) { for (int i = 0; i < 7; ++i) if (s == order[i]) return i; return 6; };
    for (auto& [id, e] : m_eng) { auto st = e->Status(); p += st.pending; if (rank(st.state) < rank(worst)) worst = st.state; }
    if (pending) *pending = p;
    return worst;
  }
 private:
  Config& m_cfg; std::string m_secret;
  std::map<std::string, std::unique_ptr<Engine>> m_eng;
};

} // namespace cd

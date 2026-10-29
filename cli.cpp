// cli.cpp — cdrive.exe: the console twin of ChronoDrive (the same engine, no window). For scripts and for testing.
//   cdrive pair [--url <miniserver>] [--name <device name>] [--no-browser]   pair this PC (prints the code, opens the QR page, waits)
//   cdrive unlink                                              forget the account on this PC
//   cdrive drives                                              the account's drives and where a new one may go
//   cdrive create --name <n> --node <node> --size <GB> [--backup] [--mirror <node>]
//   cdrive use <drive id> (--letter Z | --folder <path>)       show a drive on this PC as a drive letter or in a folder
//   cdrive remove <drive id>                                   take it off this PC (the files stay)
//   cdrive sync [--seconds <n>]                                sync every drive shown here (until Ctrl+C, or n seconds)
//   cdrive update [--install]                                  is there a newer ChronoDrive? download and verify it (and install it)
//   cdrive status                                              the configuration and the index summary
//   cdrive log [n]                                             the last n lines of the log
#include "drives.hpp"
#include "updater.hpp"
#include <shellapi.h>
#include <iostream>
#include "version.h"
using namespace cd;

static std::string Arg(int argc, wchar_t** argv, const wchar_t* name, const std::string& dflt = "") { for (int i = 1; i + 1 < argc; ++i) if (!wcscmp(argv[i], name)) return Utf8(argv[i + 1]); return dflt; }
static bool Flag(int argc, wchar_t** argv, const wchar_t* name) { for (int i = 1; i < argc; ++i) if (!wcscmp(argv[i], name)) return true; return false; }
static std::atomic<bool> g_stop{false};
static BOOL WINAPI OnCtrl(DWORD) { g_stop = true; return TRUE; }

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  std::string cmd = argc > 1 ? Utf8(argv[1]) : "status";
  Config cfg = LoadConfig(); Secrets sec; sec.Load();
  auto out = [](const std::string& s) { std::cout << s << std::endl; };   // flushed: a script reads the pairing code from a pipe
  if (cmd == "pair") {
    std::string url = Arg(argc, argv, L"--url", cfg.url), name = Arg(argc, argv, L"--name", ComputerName()), err;
    while (!url.empty() && url.back() == '/') url.pop_back();
    json r = Miniserver::LinkStart(url, name, err);
    if (r.is_null()) { out("cannot start pairing: " + err); return 1; }
    out("Pairing code " + r.value("code", "") + "  (approve it at " + r.value("link_url", "") + ")");
    if (!Flag(argc, argv, L"--no-browser")) ShellExecuteW(nullptr, L"open", Wide(r.value("qr_page", "")).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    for (int i = 0; i < 150; ++i) {
      Sleep(2000); std::string st; json p = Miniserver::LinkPoll(url, r.value("code", ""), r.value("secret", ""), st, err);
      if (st == "pending") continue;
      if (st != "approved") { out("pairing " + st + ": " + err); return 1; }
      cfg.url = url; cfg.email = p.value("record", json::object()).value("email", ""); cfg.device_id = p.value("device_id", ""); cfg.device_name = name;
      sec.Set("device_secret", p.value("device_secret", "")); sec.Save();
      if (cfg.email.empty()) { Miniserver m2(cfg.url, cfg.standby, cfg.device_id, sec.Get("device_secret")); std::string e2; json me = m2.Json(L"POST", "/api/collections/users/auth-refresh", json::object(), e2); if (me.is_object()) cfg.email = me.value("record", json::object()).value("email", ""); }   // PocketBase hides the email in the pairing answer
      SaveConfig(cfg);
      out("Linked to " + cfg.email + " (device " + cfg.device_id + ")"); return 0;
    }
    out("pairing timed out"); return 1;
  }
  if (cmd == "unlink") { { DriveSet set(cfg, ""); std::vector<std::string> ids; for (auto& m : cfg.maps) ids.push_back(m.id); for (auto& id : ids) set.Remove(id); } Secrets s; s.Clear(); s.Save(); cfg.email.clear(); cfg.device_id.clear(); SaveConfig(cfg); out("unlinked"); return 0; }
  if (cmd == "log") { for (auto& l : LogTail(argc > 2 ? (size_t)_wtoi(argv[2]) : 40)) out(l); return 0; }
  if (cmd == "status") {
    out("server     " + cfg.url + (cfg.standby.empty() ? "" : "  standby " + cfg.standby));
    out("account    " + (cfg.email.empty() ? std::string("(not paired)") : cfg.email + "  device " + cfg.device_id));
    if (cfg.maps.empty()) out("drives     (none shown on this PC)");
    for (auto& m : cfg.maps) {
      Index idx; idx.path = IndexPathFor(m.id); size_t files = 0; uint64_t bytes = 0; bool ok = idx.Load(); for (auto& [p, e] : idx.files) if (!e.dir) { ++files; bytes += e.size; }
      out("drive      " + m.name + " [" + m.id + "] on " + m.node + "  ->  " + Utf8(ShownAt(m)) + (m.mode == "letter" ? "  (files in " + Utf8(m.folder) + ")" : "") + (m.paused ? "  paused" : "")
          + (ok ? "  | rev " + std::to_string(idx.rev) + ", " + std::to_string(files) + " files, " + Human(bytes) : ""));
    }
    return 0;
  }
  Miniserver ms(cfg.url, cfg.standby, cfg.device_id, sec.Get("device_secret"));
  if (!ms.Linked()) { out("not paired: cdrive pair"); return 1; }
  if (cmd == "update") {
    upd::Info i = upd::Check(ms, CD_VERSION_STR);
    if (!i.error.empty()) { out("check: " + i.error); return 1; }
    out("running " CD_VERSION_STR ", published " + i.version + (i.available ? "  -> newer" : "  -> up to date"));
    if (!i.available && !Flag(argc, argv, L"--force")) return 0;
    std::wstring setup; std::string e = upd::Download(ms, i, setup);
    if (!e.empty()) { out("download: " + e); return 1; }
    out("downloaded and verified (sha256 + ChronoScan signature): " + Utf8(setup));
    if (!Flag(argc, argv, L"--install")) return 0;
    e = upd::Install(setup); if (!e.empty()) { out("install: " + e); return 1; }
    out("setup started"); return 0;
  }
  if (cmd == "drives") {
    std::string err; json d = ms.Json(L"GET", "/api/ms/drive/drives", nullptr, err);
    if (d.is_null()) { out("drives: " + err); return 1; }
    for (auto& x : d["drives"]) out(x.value("id", "") + "  " + x.value("name", "") + "  " + std::to_string(x.value("size_gb", 0)) + " GB on " + x.value("node", "") + " (" + x.value("node_kind", "") + ")  used " + Human(x.value("used", (uint64_t)0)) + "  " + (x.value("online", false) ? "online" : "offline") + "  urls " + x.value("urls", json::array()).dump());
    json n = ms.Json(L"GET", "/api/ms/drive/nodes", nullptr, err);
    if (!n.is_null()) { out("places (max " + std::to_string(n.value("max_gb", 0)) + " GB in all, " + std::to_string(n.value("used_gb", 0)) + " used):"); for (auto& p : n["nodes"]) out("  " + p.value("node", "") + " (" + p.value("kind", "") + ") " + p.value("mount", "") + "  " + std::to_string(p.value("free_gb", 0)) + " GB free of " + std::to_string(p.value("cap_gb", 0)) + (p.value("online", false) ? "  online" : "  offline")); }
    return 0;
  }
  if (cmd == "create") {
    std::string err; json body = {{"name", Arg(argc, argv, L"--name")}, {"node", Arg(argc, argv, L"--node")}, {"size_gb", atoi(Arg(argc, argv, L"--size", "10").c_str())}, {"backup", Flag(argc, argv, L"--backup")}};
    std::string m = Arg(argc, argv, L"--mirror"); if (!m.empty()) body["mirror_node"] = m;
    json d = ms.Json(L"POST", "/api/ms/drive/drives", body, err);
    if (d.is_null()) { out("create: " + err); return 1; }
    out("created " + d.value("id", "") + "  " + d.value("name", "") + " on " + d.value("node", "")); return 0;
  }
  if (cmd == "use" && argc > 2) {
    std::string id = Utf8(argv[2]), folder = Arg(argc, argv, L"--folder"), letter = Arg(argc, argv, L"--letter"), err;
    json d = ms.Json(L"GET", "/api/ms/drive/drives/" + id, nullptr, err);
    if (d.is_null()) { out("drive: " + err); return 1; }
    if (folder.empty() == letter.empty()) { out("give --letter Z or --folder <path>"); return 1; }
    Mapping m; m.id = id; m.name = d.value("name", ""); m.node = d.value("node", ""); m.node_kind = d.value("node_kind", "");
    if (!letter.empty()) { m.mode = "letter"; m.letter = Wide(letter.substr(0, 1)); m.letter[0] = towupper(m.letter[0]); m.folder = BackingFolder(m.name); }
    else { m.mode = "folder"; m.folder = Wide(folder); }
    DriveSet set(cfg, sec.Get("device_secret"));
    if (auto* old = cfg.Find(id)) { if (old->mode != m.mode || LowerW(old->folder) != LowerW(m.folder) || old->letter != m.letter) set.Remove(id); }
    if (!cfg.Find(id)) cfg.maps.push_back(m);
    if (m.mode == "letter" && !MountLetter(m.letter, m.folder, m.name, err)) { out("letter: " + err); set.Remove(id); SaveConfig(cfg); return 1; }
    EnsureDir(m.folder); SaveConfig(cfg);
    out("drive " + m.name + " shown at " + Utf8(ShownAt(m)) + (m.mode == "letter" ? " (files in " + Utf8(m.folder) + ")" : "")); return 0;
  }
  if (cmd == "remove" && argc > 2) {
    std::string id = Utf8(argv[2]); if (!cfg.Find(id)) { out("not shown on this PC: " + id); return 1; }
    { DriveSet set(cfg, ""); set.Remove(id); } SaveConfig(cfg); out("removed from this PC (the files stay)"); return 0;
  }
  if (cmd == "sync") {
    int secs = atoi(Arg(argc, argv, L"--seconds", "0").c_str());
    SetConsoleCtrlHandler(OnCtrl, TRUE);
    if (cfg.maps.empty()) { out("no drive shown on this PC: cdrive use <id> --letter Z | --folder <path>"); return 1; }
    DriveSet set(cfg, sec.Get("device_secret"));
    for (auto& n : set.StartAll()) out("  " + n);
    SaveConfig(cfg);
    time_t t0 = Now(); std::map<std::string, std::string> last; bool err = false;
    while (!g_stop && (!secs || Now() - t0 < secs)) {
      Sleep(2000);
      for (auto& m : cfg.maps) {
        auto st = set.Status(m.id);
        std::string line = st.state + "  " + st.detail + (st.pending ? "  pending " + std::to_string(st.pending) : "") + (st.bytesTotal ? "  " + Human(st.bytesDone) + "/" + Human(st.bytesTotal) : "") + (st.nodeUrl.empty() ? "" : "  @ " + st.nodeUrl);
        if (line != last[m.id]) { out(TimeLocal(Now()) + "  " + (cfg.maps.size() > 1 ? "[" + m.name + "] " : "") + line); last[m.id] = line; }
        if (st.state == "error") err = true;
      }
      if (err) break;
    }
    for (auto& m : cfg.maps) for (auto& r : set.Status(m.id).recent) out("  " + TimeLocal(r.at) + "  " + (cfg.maps.size() > 1 ? "[" + m.name + "] " : "") + r.what + "  " + r.path + (r.by.empty() ? "" : "  (" + r.by + ")"));
    set.StopAll();
    return err ? 1 : 0;
  }
  out("cdrive " CD_VERSION_STR ": pair | unlink | drives | create | use | remove | sync | update | status | log"); return 2;
}

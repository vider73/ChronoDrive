// cli.cpp — cdrive.exe: the console twin of ChronoDrive (the same engine, no window). For scripts and for testing.
//   cdrive pair [--url <miniserver>] [--name <device name>]     pair this PC (prints the code, opens the QR page, waits)
//   cdrive unlink                                              forget the account on this PC
//   cdrive drives                                              the account's drives and where a new one may go
//   cdrive create --name <n> --node <node> --size <GB> [--backup] [--mirror <node>]
//   cdrive use <drive id> --folder <path>                      which drive this PC syncs, and into which folder
//   cdrive sync [--seconds <n>]                                run the engine (until Ctrl+C, or n seconds), status every 2 s
//   cdrive status                                              the configuration and the index summary
//   cdrive log [n]                                             the last n lines of the log
#include "sync.hpp"
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
  auto out = [](const std::string& s) { std::cout << s << "\n"; };
  if (cmd == "pair") {
    std::string url = Arg(argc, argv, L"--url", cfg.url), name = Arg(argc, argv, L"--name", ComputerName()), err;
    while (!url.empty() && url.back() == '/') url.pop_back();
    json r = Miniserver::LinkStart(url, name, err);
    if (r.is_null()) { out("cannot start pairing: " + err); return 1; }
    out("Pairing code " + r.value("code", "") + "  (approve it at " + r.value("link_url", "") + ")");
    ShellExecuteW(nullptr, L"open", Wide(r.value("qr_page", "")).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    for (int i = 0; i < 150; ++i) {
      Sleep(2000); std::string st; json p = Miniserver::LinkPoll(url, r.value("code", ""), r.value("secret", ""), st, err);
      if (st == "pending") continue;
      if (st != "approved") { out("pairing " + st + ": " + err); return 1; }
      cfg.url = url; cfg.email = p.value("email", ""); cfg.device_id = p.value("device_id", ""); cfg.device_name = name;
      sec.Set("device_secret", p.value("device_secret", "")); sec.Save(); SaveConfig(cfg);
      out("Linked to " + cfg.email + " (device " + cfg.device_id + ")"); return 0;
    }
    out("pairing timed out"); return 1;
  }
  if (cmd == "unlink") { Secrets s; s.Clear(); s.Save(); cfg.email.clear(); cfg.device_id.clear(); cfg.drive_id.clear(); SaveConfig(cfg); DeleteFileW(IndexPath().c_str()); out("unlinked"); return 0; }
  if (cmd == "log") { for (auto& l : LogTail(argc > 2 ? (size_t)_wtoi(argv[2]) : 40)) out(l); return 0; }
  if (cmd == "status") {
    out("server     " + cfg.url + (cfg.standby.empty() ? "" : "  standby " + cfg.standby));
    out("account    " + (cfg.email.empty() ? std::string("(not paired)") : cfg.email + "  device " + cfg.device_id));
    out("drive      " + (cfg.drive_id.empty() ? std::string("(none)") : cfg.drive_name + " [" + cfg.drive_id + "] on " + cfg.drive_node + " (" + cfg.drive_node_kind + ")"));
    out("folder     " + Utf8(cfg.folder) + (cfg.paused ? "  (paused)" : ""));
    Index idx; if (idx.Load()) { size_t files = 0; uint64_t bytes = 0; for (auto& [p, e] : idx.files) if (!e.dir) { ++files; bytes += e.size; } out("index      rev " + std::to_string(idx.rev) + ", " + std::to_string(files) + " files, " + Human(bytes) + (idx.uploads.empty() ? "" : ", " + std::to_string(idx.uploads.size()) + " upload(s) to resume")); }
    return 0;
  }
  Miniserver ms(cfg.url, cfg.standby, cfg.device_id, sec.Get("device_secret"));
  if (!ms.Linked()) { out("not paired: cdrive pair"); return 1; }
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
    std::string id = Utf8(argv[2]), folder = Arg(argc, argv, L"--folder"), err;
    json d = ms.Json(L"GET", "/api/ms/drive/drives/" + id, nullptr, err);
    if (d.is_null()) { out("drive: " + err); return 1; }
    if (!folder.empty()) cfg.folder = Wide(folder);
    if (cfg.folder.empty()) { out("--folder <path> is required the first time"); return 1; }
    if (cfg.drive_id != id) DeleteFileW(IndexPath().c_str());
    cfg.drive_id = id; cfg.drive_name = d.value("name", ""); cfg.drive_node = d.value("node", ""); cfg.drive_node_kind = d.value("node_kind", "");
    SaveConfig(cfg); EnsureDir(cfg.folder);
    out("syncing drive " + cfg.drive_name + " into " + Utf8(cfg.folder)); return 0;
  }
  if (cmd == "sync") {
    int secs = atoi(Arg(argc, argv, L"--seconds", "0").c_str());
    SetConsoleCtrlHandler(OnCtrl, TRUE);
    Engine eng(cfg, sec.Get("device_secret")); eng.Start();
    time_t t0 = Now(); std::string last;
    while (!g_stop && (!secs || Now() - t0 < secs)) {
      Sleep(2000); auto st = eng.Status();
      std::string line = st.state + "  " + st.detail + (st.pending ? "  pending " + std::to_string(st.pending) : "") + (st.bytesTotal ? "  " + Human(st.bytesDone) + "/" + Human(st.bytesTotal) : "") + (st.nodeUrl.empty() ? "" : "  @ " + st.nodeUrl);
      if (line != last) { out(TimeLocal(Now()) + "  " + line); last = line; }
      if (st.state == "setup" || st.state == "error") break;
    }
    eng.Stop();
    auto st = eng.Status(); for (auto& r : st.recent) out("  " + TimeLocal(r.at) + "  " + r.what + "  " + r.path + (r.by.empty() ? "" : "  (" + r.by + ")"));
    return st.state == "error" ? 1 : 0;
  }
  out("cdrive " CD_VERSION_STR ": pair | unlink | drives | create | use | sync | status | log"); return 2;
}

// hubpipe.hpp — ChronoDrive and ChronoScan Tools (the hub, I:\LLMLab\ChronoTools): while the hub runs, ChronoDrive hides
// its own tray icon and the hub shows the drives. The hub reads them on a local pipe, \\.\pipe\ChronoDrive.<session>,
// open to this user only. Same shape as ChronoBackup's pipe: one JSON line in; one line out,
// {"final":true,"ok":true,"result":{...}} or {"final":true,"ok":false,"error":"..."}.
//   {"cmd":"status"}            -> {version, paired, email, overall, drives:[{id, name, at, state, detail, pending, paused}]}
//   {"cmd":"pause","on":bool}   pause or resume every drive on this PC
//   {"cmd":"sync"}              look at the server now
//   {"cmd":"show"}              bring the window up
#pragma once
#include "msclient.hpp"
#include <sddl.h>
#include <thread>

namespace cd { namespace hub {

constexpr const wchar_t* kHubMutex = L"Local\\ChronoTools.Hub";   // held by ChronoTools.exe while it runs
inline bool HubRunning() { HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, kHubMutex); if (!h) return false; CloseHandle(h); return true; }
inline std::wstring PipeName(const std::wstring& inst) { DWORD s = 0; ProcessIdToSessionId(GetCurrentProcessId(), &s); return L"\\\\.\\pipe\\ChronoDrive." + std::to_wstring(s) + inst; }
inline std::wstring UserSid() {
  HANDLE t; std::wstring s; if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return s;
  DWORD n = 0; GetTokenInformation(t, TokenUser, nullptr, 0, &n); std::vector<BYTE> b(n);
  if (n && GetTokenInformation(t, TokenUser, b.data(), n, &n)) { LPWSTR p = nullptr; if (ConvertSidToStringSidW(((TOKEN_USER*)b.data())->User.Sid, &p)) { s = p; LocalFree(p); } }
  CloseHandle(t); return s;
}

// One thread, one client at a time (the requests are tiny). `status` answers from what the UI thread last published;
// the other commands go to the UI thread through `post`.
class Server {
 public:
  void Publish(json s) { std::lock_guard<std::mutex> lk(m_mx); m_status = std::move(s); }
  void Start(const std::wstring& name, std::function<void(const json&)> post) {
    m_post = std::move(post);
    std::thread([this, name] { Loop(name); }).detach();
  }
 private:
  std::mutex m_mx; json m_status = json::object(); std::function<void(const json&)> m_post;
  void Loop(const std::wstring& name) {
    std::wstring sddl = L"D:(A;;GA;;;SY)(A;;GRGW;;;" + UserSid() + L")";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) { Log("hub pipe: no security descriptor"); return; }
    SECURITY_ATTRIBUTES sa{sizeof sa, sd, FALSE};
    for (;;) {
      HANDLE h = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 4, 65536, 65536, 0, &sa);
      if (h == INVALID_HANDLE_VALUE) { Log("hub pipe: " + LastErrorText()); Sleep(10000); continue; }
      if (!ConnectNamedPipe(h, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) { CloseHandle(h); continue; }
      std::string buf; char b[4096]; DWORD n = 0; json req;
      while (ReadFile(h, b, sizeof b, &n, nullptr) && n) { buf.append(b, n); size_t p = buf.find('\n'); if (p != std::string::npos) { try { req = json::parse(buf.substr(0, p)); } catch (...) {} break; } if (buf.size() > 65536) break; }
      std::string cmd = req.is_object() ? req.value("cmd", "") : "";
      json out;
      if (cmd == "status") { std::lock_guard<std::mutex> lk(m_mx); out = {{"final", true}, {"ok", true}, {"result", m_status}}; }
      else if (cmd == "pause" || cmd == "sync" || cmd == "show") { if (m_post) m_post(req); out = {{"final", true}, {"ok", true}, {"result", json::object()}}; }
      else out = {{"final", true}, {"ok", false}, {"error", "unknown command: " + cmd}};
      std::string line = out.dump() + "\n"; DWORD wr = 0;
      WriteFile(h, line.data(), (DWORD)line.size(), &wr, nullptr); FlushFileBuffers(h);
      DisconnectNamedPipe(h); CloseHandle(h);
    }
  }
};

} } // namespace cd::hub

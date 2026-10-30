// msclient.hpp — ChronoDrive: Miniserver client (pairing, device token, standby retry, nearby), the drive service
// client (grants, change feed, files, chunked uploads) and the small things every file needs (paths, config, secrets,
// log). Header only, Windows (WinHTTP, DPAPI), nlohmann/json. Contract: the Miniserver's API.md sections 2, 11 and 17.
#pragma once
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <mutex>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <cstdint>
#include "nlohmann/json.hpp"

namespace cd {
using json = nlohmann::json;

constexpr const char* kAppId = "chronodrive";
constexpr const char* kDefaultServer = "https://cs-miniserver.chronoscan.org";
constexpr const char* kDefaultStandby = "https://cs-miniserver-r.chronoscan.org";
constexpr size_t kSmallMax = 8u << 20;      // whole-file PUT up to here; chunked uploads past it
constexpr size_t kChunk = 8u << 20;         // bytes per upload chunk
constexpr int kWorkers = 6;                 // files uploaded or downloaded at once per drive (Config::workers, 1..16)

// ------------------------------------------------------------------ strings, time, paths
inline std::wstring Wide(const std::string& s) { if (s.empty()) return L""; int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0); std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n); return w; }
inline std::string Utf8(const std::wstring& w) { if (w.empty()) return ""; int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr); std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr); return s; }
inline std::string Lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
inline std::wstring LowerW(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }
inline std::string Human(uint64_t b) { char buf[64]; if (b >= (1ull << 30)) snprintf(buf, sizeof buf, "%.2f GB", b / 1073741824.0); else if (b >= (1ull << 20)) snprintf(buf, sizeof buf, "%.1f MB", b / 1048576.0); else if (b >= 1024) snprintf(buf, sizeof buf, "%llu KB", (unsigned long long)(b / 1024)); else snprintf(buf, sizeof buf, "%llu B", (unsigned long long)b); return buf; }
inline time_t Now() { return time(nullptr); }
inline int64_t NowMs() { FILETIME ft; GetSystemTimeAsFileTime(&ft); ULARGE_INTEGER u{ft.dwLowDateTime, ft.dwHighDateTime}; return (int64_t)(u.QuadPart / 10000ull - 11644473600000ull); }
inline std::string IsoUtc(time_t t) { tm gt{}; gmtime_s(&gt, &t); char b[32]; strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &gt); return b; }
inline std::string StampLocal(time_t t) { tm lt{}; localtime_s(&lt, &t); char b[32]; strftime(b, sizeof b, "%Y-%m-%d %H-%M", &lt); return b; }
inline std::string TimeLocal(time_t t) { if (!t) return ""; tm lt{}; localtime_s(&lt, &t); char b[32]; strftime(b, sizeof b, "%H:%M:%S", &lt); return b; }
inline std::wstring EnvW(const wchar_t* name) { wchar_t b[4096]; DWORD n = GetEnvironmentVariableW(name, b, 4096); return n && n < 4096 ? std::wstring(b, n) : L""; }
inline bool DirExists(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
inline bool FileExists(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
inline void EnsureDir(const std::wstring& p) { if (p.empty() || DirExists(p)) return; size_t s = p.find_last_of(L'\\'); if (s != std::wstring::npos && s > 2) EnsureDir(p.substr(0, s)); CreateDirectoryW(p.c_str(), nullptr); }
inline std::wstring JoinPath(const std::wstring& a, const std::wstring& b) { if (a.empty()) return b; if (b.empty()) return a; return (a.back() == L'\\' ? a : a + L"\\") + b; }
inline std::wstring ExeDir() { wchar_t b[MAX_PATH * 2]; GetModuleFileNameW(nullptr, b, MAX_PATH * 2); std::wstring p = b; size_t s = p.find_last_of(L'\\'); return s == std::wstring::npos ? L"." : p.substr(0, s); }
inline std::wstring DataDir() {
  static std::wstring d;
  if (d.empty()) {
    std::wstring o = EnvW(L"CHRONODRIVE_DATA");
    if (o.empty()) { PWSTR p = nullptr; if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) { o = JoinPath(p, L"ChronoDrive"); CoTaskMemFree(p); } else o = JoinPath(ExeDir(), L"data"); }
    EnsureDir(o); d = o;
  }
  return d;
}
inline std::wstring ConfigPath()  { return JoinPath(DataDir(), L"config.json"); }
inline std::wstring SecretsPath() { return JoinPath(DataDir(), L"secrets.bin"); }
inline std::wstring LogDir()      { auto d = JoinPath(DataDir(), L"logs"); EnsureDir(d); return d; }
inline std::wstring LongPath(const std::wstring& p) { return p.rfind(L"\\\\?\\", 0) == 0 ? p : L"\\\\?\\" + p; }
inline bool ReadTextFile(const std::wstring& path, std::string& out) { std::ifstream f(path, std::ios::binary); if (!f) return false; std::ostringstream ss; ss << f.rdbuf(); out = ss.str(); return true; }
inline bool WriteTextFile(const std::wstring& path, const std::string& data) {
  std::wstring tmp = path + L".tmp";
  { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); if (!f) return false; f.write(data.data(), (std::streamsize)data.size()); if (!f) return false; }
  return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}
inline std::string LastErrorText(DWORD e = GetLastError()) { char* m = nullptr; FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_FROM_HMODULE, GetModuleHandleW(L"winhttp.dll"), e, 0, (LPSTR)&m, 0, nullptr); std::string s = m ? m : ("error " + std::to_string(e)); if (m) LocalFree(m); while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back(); return s; }
inline std::string ComputerName() { wchar_t b[256]; DWORD n = 256; return GetComputerNameW(b, &n) ? Utf8(std::wstring(b, n)) : "pc"; }

// ------------------------------------------------------------------ log (logs\chronodrive.log, rotated at 2 MB)
inline std::mutex& LogMutex() { static std::mutex m; return m; }
inline void Log(const std::string& line) {
  std::lock_guard<std::mutex> lk(LogMutex());
  std::wstring p = JoinPath(LogDir(), L"chronodrive.log");
  WIN32_FILE_ATTRIBUTE_DATA fa{}; if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 2u << 20) { std::wstring old = JoinPath(LogDir(), L"chronodrive.1.log"); DeleteFileW(old.c_str()); MoveFileW(p.c_str(), old.c_str()); }
  std::ofstream f(p, std::ios::app); time_t t = Now(); tm lt{}; localtime_s(&lt, &t); char b[32]; strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &lt);
  f << b << " " << line << "\n";
}
inline std::vector<std::string> LogTail(size_t lines) {
  std::string all; ReadTextFile(JoinPath(LogDir(), L"chronodrive.log"), all);
  std::vector<std::string> v; size_t pos = 0; while (pos < all.size()) { size_t e = all.find('\n', pos); if (e == std::string::npos) e = all.size(); v.push_back(all.substr(pos, e - pos)); pos = e + 1; }
  if (v.size() > lines) v.erase(v.begin(), v.end() - (ptrdiff_t)lines);
  return v;
}

// ------------------------------------------------------------------ config (plain) and secrets (DPAPI, this user)
// One Mapping per drive shown on this PC: as a drive letter (the files live in %USERPROFILE%\ChronoDrive\<name> and the
// letter points there) or in a folder the user picked. Each mapping has its own sync engine and its own index.
struct Mapping {
  std::string id, name, node, node_kind;
  std::string mode = "letter";       // letter | folder
  std::wstring letter;               // "Z" (mode letter)
  std::wstring folder;               // where the files are on this PC (the backing folder of a letter, or the user's folder)
  bool paused = false;
};
struct Config {
  std::string url = kDefaultServer, standby, email, device_id, device_name;
  std::vector<Mapping> maps;
  int workers = 0;                   // files moved at once per drive (0 = the default, kWorkers); CHRONODRIVE_WORKERS wins
  // the engine's view of ONE drive (filled per mapping; legacy single-drive configs load into maps)
  std::string drive_id, drive_name, drive_node, drive_node_kind;
  std::wstring folder;
  bool paused = false;
  Mapping* Find(const std::string& id) { for (auto& m : maps) if (m.id == id) return &m; return nullptr; }
};
inline std::wstring IndexPathFor(const std::string& driveId) { return JoinPath(DataDir(), L"index-" + Wide(driveId) + L".json"); }
inline json ToJson(const Mapping& m) { return {{"id", m.id}, {"name", m.name}, {"node", m.node}, {"node_kind", m.node_kind}, {"mode", m.mode}, {"letter", Utf8(m.letter)}, {"folder", Utf8(m.folder)}, {"paused", m.paused}}; }
inline json ToJson(const Config& c) {
  json maps = json::array(); for (auto& m : c.maps) maps.push_back(ToJson(m));
  json j = {{"url", c.url}, {"standby", c.standby}, {"email", c.email}, {"device_id", c.device_id}, {"device_name", c.device_name}, {"drives", maps}};
  if (c.workers) j["workers"] = c.workers;
  return j;
}
inline Config LoadConfig() {
  Config c; std::string s;
  if (ReadTextFile(ConfigPath(), s)) {
    try {
      json j = json::parse(s); c.url = j.value("url", c.url); c.standby = j.value("standby", ""); c.email = j.value("email", ""); c.device_id = j.value("device_id", ""); c.device_name = j.value("device_name", ""); c.workers = j.value("workers", 0);
      json maps = j.value("drives", json::array());   // a named copy: iterating a temporary json dangles
      for (auto& x : maps) {
        Mapping m; m.id = x.value("id", ""); m.name = x.value("name", ""); m.node = x.value("node", ""); m.node_kind = x.value("node_kind", "");
        m.mode = x.value("mode", "folder"); m.letter = Wide(x.value("letter", "")); m.folder = Wide(x.value("folder", "")); m.paused = x.value("paused", false);
        if (!m.id.empty()) c.maps.push_back(m);
      }
      json d = j.value("drive", json::object());       // 0.1.0: one drive in a folder
      if (c.maps.empty() && !d.value("id", std::string()).empty() && !j.value("folder", std::string()).empty()) {
        Mapping m; m.id = d.value("id", ""); m.name = d.value("name", ""); m.node = d.value("node", ""); m.node_kind = d.value("node_kind", "");
        m.mode = "folder"; m.folder = Wide(j.value("folder", "")); m.paused = j.value("paused", false);
        c.maps.push_back(m);
        std::wstring old = JoinPath(DataDir(), L"index.json");
        if (FileExists(old)) MoveFileExW(old.c_str(), IndexPathFor(m.id).c_str(), MOVEFILE_REPLACE_EXISTING);
      }
    } catch (...) {}
  }
  std::wstring env = EnvW(L"MINISERVER_URL"); if (!env.empty()) c.url = Utf8(env);   // the environment wins (the lab rule)
  std::wstring w = EnvW(L"CHRONODRIVE_WORKERS"); if (!w.empty()) c.workers = _wtoi(w.c_str());
  while (!c.url.empty() && c.url.back() == '/') c.url.pop_back();
  return c;
}
inline bool SaveConfig(const Config& c) { return WriteTextFile(ConfigPath(), ToJson(c).dump(2)); }
// The engine's config for one mapping: the account plus that drive.
inline Config ForMapping(const Config& c, const Mapping& m) { Config e = c; e.maps.clear(); e.drive_id = m.id; e.drive_name = m.name; e.drive_node = m.node; e.drive_node_kind = m.node_kind; e.folder = m.folder; e.paused = m.paused; return e; }

class Secrets {
 public:
  bool Load() {
    std::string blob; m_map = json::object();
    if (!ReadTextFile(SecretsPath(), blob) || blob.empty()) return true;
    DATA_BLOB in{(DWORD)blob.size(), (BYTE*)blob.data()}, out{}, ent = Entropy();
    if (!CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) { Log("secrets.bin: cannot decrypt (" + LastErrorText() + ")"); return false; }
    try { m_map = json::parse(std::string((char*)out.pbData, out.cbData)); } catch (...) { m_map = json::object(); }
    LocalFree(out.pbData); return true;
  }
  bool Save() {
    std::string plain = m_map.dump();
    DATA_BLOB in{(DWORD)plain.size(), (BYTE*)plain.data()}, out{}, ent = Entropy();
    if (!CryptProtectData(&in, L"ChronoDrive", &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) { Log("secrets: CryptProtectData failed: " + LastErrorText()); return false; }
    bool ok = WriteTextFile(SecretsPath(), std::string((char*)out.pbData, out.cbData)); LocalFree(out.pbData); return ok;
  }
  std::string Get(const std::string& k) const { return m_map.value(k, ""); }
  void Set(const std::string& k, const std::string& v) { if (v.empty()) m_map.erase(k); else m_map[k] = v; }
  void Clear() { m_map = json::object(); }
 private:
  static DATA_BLOB Entropy() { static BYTE e[] = "ChronoDrive/1 secrets"; return DATA_BLOB{(DWORD)sizeof(e) - 1, e}; }
  json m_map = json::object();
};

// ------------------------------------------------------------------ SHA-256 (CNG) of a file or a buffer
inline std::string HexOf(const BYTE* d, size_t n) { static const char* h = "0123456789abcdef"; std::string s; s.reserve(n * 2); for (size_t i = 0; i < n; ++i) { s += h[d[i] >> 4]; s += h[d[i] & 15]; } return s; }
inline bool Sha256File(const std::wstring& path, std::string& hex, uint64_t* size = nullptr) {
  HANDLE f = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  HCRYPTPROV p = 0; HCRYPTHASH h = 0; bool ok = false;
  if (CryptAcquireContextW(&p, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) && CryptCreateHash(p, CALG_SHA_256, 0, 0, &h)) {
    std::vector<BYTE> buf(1 << 20); DWORD rd = 0; uint64_t total = 0; ok = true;
    while (ReadFile(f, buf.data(), (DWORD)buf.size(), &rd, nullptr) && rd) { if (!CryptHashData(h, buf.data(), rd, 0)) { ok = false; break; } total += rd; }
    if (ok) { BYTE out[32]; DWORD n = 32; ok = CryptGetHashParam(h, HP_HASHVAL, out, &n, 0) != FALSE; if (ok) hex = HexOf(out, 32); if (size) *size = total; }
  }
  if (h) CryptDestroyHash(h); if (p) CryptReleaseContext(p, 0); CloseHandle(f);
  return ok;
}
inline std::string Sha256Of(const void* d, size_t n) {
  HCRYPTPROV p = 0; HCRYPTHASH h = 0; std::string hex;
  if (CryptAcquireContextW(&p, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) && CryptCreateHash(p, CALG_SHA_256, 0, 0, &h) && CryptHashData(h, (const BYTE*)d, (DWORD)n, 0)) { BYTE out[32]; DWORD l = 32; if (CryptGetHashParam(h, HP_HASHVAL, out, &l, 0)) hex = HexOf(out, 32); }
  if (h) CryptDestroyHash(h); if (p) CryptReleaseContext(p, 0);
  return hex;
}

// ------------------------------------------------------------------ HTTP (WinHTTP): JSON calls, streamed bodies, pinned certificates
struct HttpRequest {
  std::wstring method = L"GET";
  std::string url;
  std::vector<std::wstring> headers;          // "Name: value"
  std::string body;                           // in memory, or:
  std::wstring bodyFile; uint64_t bodyOffset = 0, bodyLength = 0;   // a slice of a file (chunked uploads)
  std::wstring outFile;                       // stream the response body here (downloads); else into HttpResponse::body
  uint64_t outAppend = 0;                     // with outFile: append from this offset (Range resume)
  int timeoutMs = 20000;
  std::string pinSha256;                      // a home node's certificate (DER SHA-256, hex): trust that one only
};
struct HttpResponse {
  int status = 0; std::string body, error; std::map<std::string, std::string> headers; DWORD winerr = 0;
  bool ok() const { return status >= 200 && status < 300; }
  bool transport() const { return status == 0 || status == 502 || status == 503 || status == 504; }   // "try the standby" (API.md section 11)
  std::string header(const std::string& k) const { auto it = headers.find(Lower(k)); return it == headers.end() ? "" : it->second; }
  std::string message() const { try { return json::parse(body).value("message", json::parse(body).value("error", "")); } catch (...) { return body.substr(0, 200); } }
};
struct PinCtx { std::string want; bool failed = false, checked = false; };
inline void CALLBACK PinCallback(HINTERNET h, DWORD_PTR ctx, DWORD status, LPVOID, DWORD) {
  if (status != WINHTTP_CALLBACK_STATUS_SENDING_REQUEST || !ctx) return;
  PinCtx* p = (PinCtx*)ctx; if (p->checked) return;
  PCCERT_CONTEXT cert = nullptr; DWORD sz = sizeof cert;
  if (!WinHttpQueryOption(h, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &cert, &sz) || !cert) { p->failed = true; return; }
  p->checked = true; p->failed = Lower(Sha256Of(cert->pbCertEncoded, cert->cbCertEncoded)) != Lower(p->want);
  CertFreeCertificateContext(cert);
}
// One WinHTTP session for the whole process: WinHTTP keeps a session's connections alive and reuses them, so a run of
// small requests to a node pays one TLS handshake per connection instead of one per file (and the sync workers share it).
inline HINTERNET HttpSession() {
  static HINTERNET s = [] {
    HINTERNET h = WinHttpOpen(L"ChronoDrive/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (h) { DWORD n = 32; WinHttpSetOption(h, WINHTTP_OPTION_MAX_CONNS_PER_SERVER, &n, sizeof n); }
    return h;
  }();
  return s;
}
inline HttpResponse HttpOnce(const HttpRequest& rq) {
  HttpResponse r; std::wstring wurl = Wide(rq.url);
  URL_COMPONENTS uc{}; uc.dwStructSize = sizeof uc; wchar_t host[256], path[4096]; uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 4096;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { r.error = "bad url"; return r; }
  HINTERNET s = HttpSession();
  if (!s) { r.error = "WinHttpOpen"; return r; }
  HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
  bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
  HINTERNET q = c ? WinHttpOpenRequest(c, rq.method.c_str(), path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0) : nullptr;
  if (!q) { r.winerr = GetLastError(); r.error = "connect: " + LastErrorText(r.winerr); if (c) WinHttpCloseHandle(c); return r; }
  WinHttpSetTimeouts(q, rq.timeoutMs, rq.timeoutMs, rq.timeoutMs, rq.timeoutMs);
  PinCtx pin; pin.want = rq.pinSha256;
  if (https && !rq.pinSha256.empty()) {
    // a home node's self-signed certificate: ignore the CA chain and the name, then check the pin before the body goes
    DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
    WinHttpSetOption(q, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof flags);
    WinHttpSetStatusCallback(q, PinCallback, WINHTTP_CALLBACK_FLAG_SEND_REQUEST, 0);
  }
  std::wstring hdr; for (auto& h : rq.headers) hdr += h + L"\r\n";
  if (hdr.empty()) hdr = L"Accept: application/json\r\n";
  uint64_t total = rq.bodyFile.empty() ? rq.body.size() : rq.bodyLength;
  BOOL ok = WinHttpSendRequest(q, hdr.c_str(), (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, (DWORD)total, https && !rq.pinSha256.empty() ? (DWORD_PTR)&pin : 0);
  if (ok && pin.failed) { r.error = "certificate pin mismatch (the node's certificate is not the one the server handed out)"; r.status = 0; WinHttpCloseHandle(q); WinHttpCloseHandle(c); return r; }
  if (ok && total) {
    if (rq.bodyFile.empty()) { DWORD wr = 0; ok = WinHttpWriteData(q, rq.body.data(), (DWORD)rq.body.size(), &wr); }
    else {
      HANDLE f = CreateFileW(LongPath(rq.bodyFile).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
      if (f == INVALID_HANDLE_VALUE) ok = FALSE;
      else {
        LARGE_INTEGER off; off.QuadPart = (LONGLONG)rq.bodyOffset; SetFilePointerEx(f, off, nullptr, FILE_BEGIN);
        std::vector<char> buf(1 << 20); uint64_t left = total;
        while (ok && left) { DWORD rd = 0; if (!ReadFile(f, buf.data(), (DWORD)(std::min)((uint64_t)buf.size(), left), &rd, nullptr) || !rd) { ok = FALSE; break; } DWORD wr = 0; ok = WinHttpWriteData(q, buf.data(), rd, &wr); left -= rd; }
        CloseHandle(f);
      }
    }
  }
  if (ok) ok = WinHttpReceiveResponse(q, nullptr);
  if (!ok) { r.winerr = GetLastError(); r.error = "request: " + LastErrorText(r.winerr); }
  else {
    DWORD st = 0, sz = sizeof st; WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz, WINHTTP_NO_HEADER_INDEX); r.status = (int)st;
    DWORD hl = 0; WinHttpQueryHeaders(q, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &hl, WINHTTP_NO_HEADER_INDEX);
    if (hl) { std::wstring raw(hl / 2, 0); if (WinHttpQueryHeaders(q, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, &raw[0], &hl, WINHTTP_NO_HEADER_INDEX)) {
      std::string a = Utf8(raw.c_str()); size_t pos = 0; while (pos < a.size()) { size_t e = a.find("\r\n", pos); if (e == std::string::npos) e = a.size(); std::string line = a.substr(pos, e - pos); size_t cl = line.find(':'); if (cl != std::string::npos) { std::string k = Lower(line.substr(0, cl)), v = line.substr(cl + 1); while (!v.empty() && v[0] == ' ') v.erase(0, 1); r.headers[k] = v; } pos = e + 2; } } }
    HANDLE out = INVALID_HANDLE_VALUE;
    if (!rq.outFile.empty() && r.ok()) {
      out = CreateFileW(LongPath(rq.outFile).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (out == INVALID_HANDLE_VALUE) { r.error = "cannot write " + Utf8(rq.outFile); r.status = 0; }
      else { LARGE_INTEGER off; off.QuadPart = (LONGLONG)rq.outAppend; SetFilePointerEx(out, off, nullptr, FILE_BEGIN); SetEndOfFile(out); }
    }
    std::vector<char> buf(1 << 20); DWORD avail = 0;
    while (r.status && WinHttpQueryDataAvailable(q, &avail) && avail) {
      DWORD want = (std::min)(avail, (DWORD)buf.size()), rd = 0;
      if (!WinHttpReadData(q, buf.data(), want, &rd) || !rd) break;
      if (out != INVALID_HANDLE_VALUE) { DWORD wr = 0; if (!WriteFile(out, buf.data(), rd, &wr, nullptr) || wr != rd) { r.error = "disk write failed"; r.status = 0; break; } }
      else { r.body.append(buf.data(), rd); if (r.body.size() > (16u << 20)) break; }
    }
    if (out != INVALID_HANDLE_VALUE) { FlushFileBuffers(out); CloseHandle(out); }
  }
  WinHttpCloseHandle(q); WinHttpCloseHandle(c);
  return r;
}
// A kept-alive connection the node closed meanwhile fails as "connection error": once more on a fresh one. Repeating a
// write is safe here: the drive answers a repeated PUT/commit with 412 (same content, adopted by the engine) and a
// repeated DELETE with 404.
inline HttpResponse Http(const HttpRequest& rq) {
  HttpResponse r = HttpOnce(rq);
  if (r.status == 0 && r.winerr == ERROR_WINHTTP_CONNECTION_ERROR) r = HttpOnce(rq);
  return r;
}
inline HttpResponse HttpJson(const std::wstring& method, const std::string& url, const json& body = nullptr, const std::vector<std::wstring>& extra = {}, int timeoutMs = 20000, const std::string& pin = "") {
  HttpRequest rq; rq.method = method; rq.url = url; rq.timeoutMs = timeoutMs; rq.pinSha256 = pin;
  rq.headers = {L"Accept: application/json", L"Content-Type: application/json"}; for (auto& h : extra) rq.headers.push_back(h);
  if (!body.is_null()) rq.body = body.dump();
  return Http(rq);
}

// ------------------------------------------------------------------ Miniserver (API.md sections 2 and 11): the control plane
class Miniserver {
 public:
  Miniserver(std::string base, std::string standby, std::string deviceId, std::string deviceSecret)
    : m_servers{base}, m_id(std::move(deviceId)), m_secret(std::move(deviceSecret)) { if (!standby.empty() && standby != base) m_servers.push_back(standby); else if (base == kDefaultServer) m_servers.push_back(kDefaultStandby); }
  bool Linked() const { return !m_id.empty() && !m_secret.empty(); }
  const std::string& Base() const { return m_servers[0]; }
  // One call, primary first; on a transport failure the standby (writes answer 503 there: reported as is).
  HttpResponse Call(const std::wstring& method, const std::string& path, const json& body, bool auth, std::string& err) {
    std::string tok;
    if (auth && !Token(tok, err)) { HttpResponse r; r.error = err; return r; }
    std::vector<std::wstring> h; if (auth) h.push_back(L"Authorization: " + Wide(tok));
    HttpResponse r;
    for (size_t i = 0; i < m_servers.size(); ++i) {
      size_t idx = (m_active + i) % m_servers.size();
      r = HttpJson(method, m_servers[idx] + path, body, h);
      if (r.status == 401 && auth) { m_token.clear(); if (!Token(tok, err)) return r; h = {L"Authorization: " + Wide(tok)}; r = HttpJson(method, m_servers[idx] + path, body, h); }
      if (!r.transport()) { m_active = idx; return r; }
      Log("miniserver: " + m_servers[idx] + path + " -> " + (r.status ? std::to_string(r.status) : r.error));
    }
    return r;
  }
  json Json(const std::wstring& method, const std::string& path, const json& body, std::string& err, int* status = nullptr) {
    auto r = Call(method, path, body, true, err);
    if (status) *status = r.status;
    if (!r.ok()) { if (err.empty()) err = r.status ? "http " + std::to_string(r.status) + (r.message().empty() ? "" : ": " + r.message()) : r.error; return nullptr; }
    try { return json::parse(r.body); } catch (...) { err = "bad json"; return nullptr; }
  }
  // 12 h device token, refreshed when missing or expired. "revoked" = the device was revoked on the dashboard.
  bool Token(std::string& tok, std::string& err) {
    if (!m_token.empty() && Now() < m_expires - 300) { tok = m_token; return true; }
    HttpResponse r;
    for (size_t i = 0; i < m_servers.size(); ++i) { r = HttpJson(L"POST", m_servers[(m_active + i) % m_servers.size()] + "/api/ms/device/token", json({{"device_id", m_id}, {"device_secret", m_secret}})); if (!r.transport()) break; }
    if (!r.ok()) { err = r.status == 403 ? "revoked" : (r.status ? "token http " + std::to_string(r.status) : r.error); return false; }
    try { auto j = json::parse(r.body); m_token = j.value("token", ""); m_expires = Now() + j.value("expires_in", 43200); } catch (...) { err = "bad token json"; return false; }
    tok = m_token; return !m_token.empty();
  }
  static json LinkStart(const std::string& base, const std::string& deviceName, std::string& err) {
    auto r = HttpJson(L"POST", base + "/api/ms/link/start", json({{"name", deviceName}, {"platform", "windows"}, {"app", kAppId}}));
    if (!r.ok()) { err = r.status ? "http " + std::to_string(r.status) : r.error; return nullptr; }
    try { return json::parse(r.body); } catch (...) { err = "bad json"; return nullptr; }
  }
  static json LinkPoll(const std::string& base, const std::string& code, const std::string& secret, std::string& status, std::string& err) {
    auto r = HttpJson(L"POST", base + "/api/ms/link/poll", json({{"code", code}, {"secret", secret}}));
    if (r.status == 202) { status = "pending"; return nullptr; }
    if (r.status == 200) { status = "approved"; try { return json::parse(r.body); } catch (...) { err = "bad json"; return nullptr; } }
    status = r.status == 410 ? "expired" : "error"; err = r.status ? "http " + std::to_string(r.status) : r.error; return nullptr;
  }
 private:
  std::vector<std::string> m_servers; size_t m_active = 0; std::string m_id, m_secret, m_token; time_t m_expires = 0;
};

// ------------------------------------------------------------------ the drive service (API.md section 17)
struct Entry { std::string path; int64_t rev = 0; std::string kind; uint64_t size = 0; int64_t mtime = 0; std::string sha256; bool deleted = false; std::string by, at; };
inline Entry EntryOf(const json& j) { Entry e; e.path = j.value("path", ""); e.rev = j.value("rev", (int64_t)0); e.kind = j.value("kind", "file"); e.size = j.value("size", (uint64_t)0); e.mtime = j.value("mtime", (int64_t)0); e.sha256 = j.value("sha256", ""); e.deleted = j.value("deleted", false); e.by = j.value("by", ""); e.at = j.value("at", ""); return e; }
inline std::string EncPath(const std::string& p) {   // percent-encode each segment (UTF-8 bytes), keep '/'
  static const char* h = "0123456789ABCDEF"; std::string o;
  for (unsigned char c : p) { if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') o += (char)c; else { o += '%'; o += h[c >> 4]; o += h[c & 15]; } }
  return o;
}
class DriveClient {
 public:
  std::string base, grant, pin;   // base = <node url>/drive/<id>
  std::vector<std::wstring> H(std::vector<std::wstring> extra = {}) const { std::vector<std::wstring> h{L"Authorization: Drive " + Wide(grant), L"Accept: application/json"}; for (auto& e : extra) h.push_back(e); return h; }
  HttpResponse Get(const std::string& path, int timeoutMs = 20000) { HttpRequest rq; rq.url = base + path; rq.headers = H(); rq.timeoutMs = timeoutMs; rq.pinSha256 = pin; return Http(rq); }
  json Info(std::string& err) { auto r = Get(""); return Parse(r, err); }
  json Changes(int64_t since, int waitS, std::string& err) { auto r = Get("/changes?since=" + std::to_string(since) + "&wait=" + std::to_string(waitS), (waitS + 30) * 1000); return Parse(r, err); }
  // Download to a .part file (resumed from its current size), then the caller renames it.
  bool Download(const std::string& path, const std::wstring& partFile, uint64_t have, std::string& err, int* status = nullptr) {
    HttpRequest rq; rq.url = base + "/files/" + EncPath(path); rq.headers = H(have ? std::vector<std::wstring>{L"Range: bytes=" + std::to_wstring(have) + L"-"} : std::vector<std::wstring>{}); rq.outFile = partFile; rq.outAppend = have; rq.timeoutMs = 120000; rq.pinSha256 = pin;
    auto r = Http(rq); if (status) *status = r.status;
    if (have && r.status == 200) { r.error = "server ignored the range"; }   // the file was written from 0 by outAppend anyway: restart
    if (!r.ok()) { err = r.status ? "http " + std::to_string(r.status) + ": " + r.message() : r.error; return false; }
    return true;
  }
  json PutSmall(const std::string& path, const std::wstring& file, uint64_t size, const std::string& sha, int64_t mtime, int64_t baseRev, std::string& err, int* status = nullptr) {
    HttpRequest rq; rq.method = L"PUT"; rq.url = base + "/files/" + EncPath(path); rq.bodyFile = file; rq.bodyOffset = 0; rq.bodyLength = size; rq.timeoutMs = 120000; rq.pinSha256 = pin;
    rq.headers = H({L"Content-Type: application/octet-stream", L"X-Drive-Base-Rev: " + std::to_wstring(baseRev), L"X-Drive-Mtime: " + std::to_wstring(mtime), L"X-Drive-Sha256: " + Wide(sha)});
    auto r = Http(rq); if (status) *status = r.status; return Parse(r, err);
  }
  json UploadStart(const std::string& path, uint64_t size, const std::string& sha, int64_t mtime, int64_t baseRev, std::string& err, int* status = nullptr) {
    HttpRequest rq; rq.method = L"POST"; rq.url = base + "/uploads"; rq.headers = H({L"Content-Type: application/json"}); rq.body = json({{"path", path}, {"size", size}, {"sha256", sha}, {"mtime", mtime}, {"base_rev", baseRev}}).dump(); rq.pinSha256 = pin;
    auto r = Http(rq); if (status) *status = r.status; return Parse(r, err);
  }
  uint64_t UploadOffset(const std::string& upload, std::string& err) {
    HttpRequest rq; rq.method = L"HEAD"; rq.url = base + "/uploads/" + upload; rq.headers = H(); rq.pinSha256 = pin;
    auto r = Http(rq); if (!r.ok()) { err = r.status ? "http " + std::to_string(r.status) : r.error; return (uint64_t)-1; }
    return (uint64_t)_strtoui64(r.header("Upload-Offset").c_str(), nullptr, 10);
  }
  bool UploadChunk(const std::string& upload, const std::wstring& file, uint64_t offset, uint64_t len, std::string& err, uint64_t* realOffset = nullptr) {
    HttpRequest rq; rq.method = L"PATCH"; rq.url = base + "/uploads/" + upload; rq.bodyFile = file; rq.bodyOffset = offset; rq.bodyLength = len; rq.timeoutMs = 300000; rq.pinSha256 = pin;
    rq.headers = H({L"Content-Type: application/offset+octet-stream", L"Upload-Offset: " + std::to_wstring(offset)});
    auto r = Http(rq);
    if (r.status == 409) { if (realOffset) *realOffset = (uint64_t)_strtoui64(r.header("Upload-Offset").c_str(), nullptr, 10); err = "offset mismatch"; return false; }
    if (!r.ok()) { err = r.status ? "http " + std::to_string(r.status) + ": " + r.message() : r.error; return false; }
    return true;
  }
  json UploadCommit(const std::string& upload, int64_t baseRev, std::string& err, int* status = nullptr) {
    HttpRequest rq; rq.method = L"POST"; rq.url = base + "/uploads/" + upload + "/commit"; rq.headers = H({L"Content-Type: application/json"}); rq.body = json({{"base_rev", baseRev}}).dump(); rq.timeoutMs = 300000; rq.pinSha256 = pin;
    auto r = Http(rq); if (status) *status = r.status; return Parse(r, err);
  }
  json Delete(const std::string& path, int64_t baseRev, std::string& err, int* status = nullptr) {
    HttpRequest rq; rq.method = L"DELETE"; rq.url = base + "/files/" + EncPath(path); rq.headers = H({L"X-Drive-Base-Rev: " + std::to_wstring(baseRev)}); rq.pinSha256 = pin;
    auto r = Http(rq); if (status) *status = r.status; return Parse(r, err);
  }
  json Mkdir(const std::string& path, std::string& err) {
    HttpRequest rq; rq.method = L"POST"; rq.url = base + "/mkdir"; rq.headers = H({L"Content-Type: application/json"}); rq.body = json({{"path", path}}).dump(); rq.pinSha256 = pin;
    auto r = Http(rq); return Parse(r, err);
  }
 private:
  static json Parse(const HttpResponse& r, std::string& err) {
    if (!r.ok()) { err = r.status ? "http " + std::to_string(r.status) + ": " + r.message() : r.error; if (r.status == 412) { try { return json::parse(r.body); } catch (...) {} } return nullptr; }
    try { return r.body.empty() ? json::object() : json::parse(r.body); } catch (...) { err = "bad json"; return nullptr; }
  }
};

} // namespace cd

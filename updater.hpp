// updater.hpp — ChronoDrive keeps itself current from the Miniserver (the same mechanism as ChronoRemote/csLink):
//   GET <server>/chronodrive/windows.json   {version, size, sha256, notes, published}   (public, tools\publish_setup.ps1)
//   GET <server>/api/cdrive/download        the signed setup, with the paired device's user token
// The setup is checked twice (the SHA-256 the server announced, and ChronoScan's Authenticode signature) and started
// silently with /UPDATEFROM=app: it closes this app, installs over it and starts it again in the tray (--updated).
// No administrator rights: the setup installs per user. Any failure is just "no update this time" and is logged.
#pragma once
#include "msclient.hpp"
#include <softpub.h>
#include <wintrust.h>
#pragma comment(lib, "wintrust.lib")

namespace cd { namespace upd {

constexpr const char* kThumbprint = "996426bc57433e3b1f201324d6072bc437341237";   // CHRONOSCAN CAPTURE S.L. (DigiCert, until 2027-08-20)

struct Info { bool available = false; std::string version, notes, sha256, published, error; uint64_t size = 0; };

// "1.10.0" > "1.9.2"; missing parts count as 0.
inline int CompareVersions(const std::string& a, const std::string& b) {
  size_t i = 0, j = 0;
  while (i < a.size() || j < b.size()) {
    long x = 0, y = 0;
    while (i < a.size() && isdigit((unsigned char)a[i])) x = x * 10 + (a[i++] - '0');
    while (j < b.size() && isdigit((unsigned char)b[j])) y = y * 10 + (b[j++] - '0');
    if (x != y) return x < y ? -1 : 1;
    if (i < a.size()) i++;
    if (j < b.size()) j++;
  }
  return 0;
}

inline Info Check(Miniserver& ms, const std::string& current) {
  Info r; std::string err;
  HttpResponse h = ms.Call(L"GET", "/chronodrive/windows.json", nullptr, false, err);
  if (!h.ok()) { r.error = h.status ? "windows.json: http " + std::to_string(h.status) : (err.empty() ? h.error : err); return r; }
  try {
    json j = json::parse(h.body);
    r.version = j.value("version", ""); r.notes = j.value("notes", ""); r.sha256 = Lower(j.value("sha256", "")); r.published = j.value("published", ""); r.size = j.value("size", (uint64_t)0);
  } catch (...) { r.error = "windows.json is not JSON"; return r; }
  if (r.version.empty()) { r.error = "no version in windows.json"; return r; }
  r.available = CompareVersions(r.version, current) > 0;
  return r;
}

inline bool SignedByChronoScan(const std::wstring& path, std::string& why) {
  WINTRUST_FILE_INFO fi{ sizeof(fi) }; fi.pcwszFilePath = path.c_str();
  WINTRUST_DATA wd{ sizeof(wd) }; wd.dwUIChoice = WTD_UI_NONE; wd.fdwRevocationChecks = WTD_REVOKE_NONE; wd.dwUnionChoice = WTD_CHOICE_FILE; wd.pFile = &fi;
  wd.dwStateAction = WTD_STATEACTION_VERIFY; wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  LONG st = WinVerifyTrust(nullptr, &action, &wd);
  wd.dwStateAction = WTD_STATEACTION_CLOSE; WinVerifyTrust(nullptr, &action, &wd);
  if (st != ERROR_SUCCESS) { char b[64]; sprintf_s(b, "the signature does not verify (0x%08lx)", (unsigned long)st); why = b; return false; }
  HCERTSTORE store = nullptr; HCRYPTMSG msg = nullptr; DWORD enc = 0, ct = 0, ft = 0;
  if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(), CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED, CERT_QUERY_FORMAT_FLAG_BINARY, 0, &enc, &ct, &ft, &store, &msg, nullptr)) { why = "no signature found"; return false; }
  bool ours = false; DWORD n = 0;
  if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &n)) {
    std::vector<BYTE> buf(n);
    if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, buf.data(), &n)) {
      CMSG_SIGNER_INFO* si = (CMSG_SIGNER_INFO*)buf.data();
      CERT_INFO ci{}; ci.Issuer = si->Issuer; ci.SerialNumber = si->SerialNumber;
      if (PCCERT_CONTEXT cert = CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT, &ci, nullptr)) {
        BYTE sha1[20]; DWORD len = 20;
        if (CertGetCertificateContextProperty(cert, CERT_SHA1_HASH_PROP_ID, sha1, &len)) { ours = Lower(HexOf(sha1, 20)) == kThumbprint; if (!ours) why = "signed by someone else"; }
        CertFreeCertificateContext(cert);
      }
    }
  }
  if (msg) CryptMsgClose(msg);
  if (store) CertCloseStore(store, 0);
  if (!ours && why.empty()) why = "the signer could not be read";
  return ours;
}

// Downloads the setup with the device's token into %LOCALAPPDATA%\ChronoDrive\updates and verifies it. "" = ok.
inline std::string Download(Miniserver& ms, const Info& info, std::wstring& setupOut) {
  std::wstring dir = JoinPath(DataDir(), L"updates"); EnsureDir(dir);
  std::wstring setup = JoinPath(dir, L"ChronoDrive_Setup_" + Wide(info.version) + L".exe");
  std::string tok, err;
  if (!ms.Token(tok, err)) return "token: " + err;
  HttpRequest rq; rq.url = ms.Base() + "/api/cdrive/download"; rq.headers = {L"Authorization: " + Wide(tok)}; rq.outFile = setup; rq.timeoutMs = 120000;
  HttpResponse r = Http(rq);
  if (!r.ok()) { DeleteFileW(setup.c_str()); return r.status ? "download: http " + std::to_string(r.status) : "download: " + r.error; }
  std::string sha; uint64_t size = 0;
  if (!Sha256File(setup, sha, &size) || (!info.sha256.empty() && sha != info.sha256)) { DeleteFileW(setup.c_str()); return "the download does not match what the server announced"; }
  std::string why;
  if (!SignedByChronoScan(setup, why)) { DeleteFileW(setup.c_str()); return "the setup is not signed by ChronoScan: " + why; }
  setupOut = setup;
  return "";
}

// Starts the downloaded setup silently. "" = it is running: the caller exits (the setup closes what it replaces).
inline std::string Install(const std::wstring& setup) {
  SHELLEXECUTEINFOW se{ sizeof(se) };
  se.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI; se.lpVerb = L"open"; se.lpFile = setup.c_str();
  std::wstring params = L"/VERYSILENT /SUPPRESSMSGBOXES /CLOSEAPPLICATIONS /NORESTART /UPDATEFROM=app";
  se.lpParameters = params.c_str(); se.nShow = SW_HIDE;
  if (!ShellExecuteExW(&se)) return "the setup could not start (error " + std::to_string(GetLastError()) + ")";
  if (se.hProcess) CloseHandle(se.hProcess);
  Log("update: started " + Utf8(setup));
  return "";
}

// Old downloads go (the one just installed is kept until the next start).
inline void CleanOld(const std::string& running) {
  std::wstring dir = JoinPath(DataDir(), L"updates");
  WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(JoinPath(dir, L"ChronoDrive_Setup_*.exe").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do { std::wstring n = fd.cFileName; if (n.find(Wide(running)) == std::wstring::npos) DeleteFileW(JoinPath(dir, n).c_str()); } while (FindNextFileW(h, &fd));
  FindClose(h);
}

} } // namespace cd::upd

// main.cpp — ChronoDrive.exe: the tray app. Keeps one local folder in sync with a drive on the Miniserver (sync.hpp);
// a ChronoUI window for the setup (pair, pick or create a drive, pick the folder) and the status (recent changes, log).
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <deque>
#include <memory>
#include "sync.hpp"
#include "VirtualWidget.hpp"
#include "VirtualChat.hpp"
#include "VControls.hpp"
#include "VNavigation.hpp"
#include "VActions.hpp"
#include "VIndicators.hpp"
#include "VCollections.hpp"
#include "VDraw.hpp"
#include "version.h"
using namespace ChronoUI;
using cd::json;

constexpr UINT WM_APP_RESULT = WM_APP + 1;
constexpr UINT WM_APP_TRAY = WM_APP + 3;
constexpr UINT kTrayId = 1;
enum { kMenuOpen = 101, kMenuFolder = 102, kMenuPause = 103, kMenuExit = 109, kMenuRecent = 200 };

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static std::wstring ExePathW() { wchar_t b[MAX_PATH * 2]; GetModuleFileNameW(nullptr, b, MAX_PATH * 2); return b; }
static bool AutostartEnabled() { HKEY k; if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false; wchar_t v[1024]; DWORD n = sizeof v, t = 0; bool on = RegQueryValueExW(k, L"ChronoDrive", nullptr, &t, (BYTE*)v, &n) == ERROR_SUCCESS && t == REG_SZ && n > 2; RegCloseKey(k); return on; }
static void SetAutostart(bool on) { HKEY k; if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return; if (on) { std::wstring v = L"\"" + ExePathW() + L"\" --tray"; RegSetValueExW(k, L"ChronoDrive", 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t))); } else RegDeleteValueW(k, L"ChronoDrive"); RegCloseKey(k); }

struct Tray {
  NOTIFYICONDATAW nid{}; bool added = false;
  void Add(HWND hwnd, HINSTANCE inst) {
    nid.cbSize = sizeof nid; nid.hWnd = hwnd; nid.uID = kTrayId; nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1)); if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"ChronoDrive"); added = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    nid.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &nid);
  }
  void Tip(const std::wstring& t) { if (!added) return; nid.uFlags = NIF_TIP; wcsncpy_s(nid.szTip, t.c_str(), _TRUNCATE); Shell_NotifyIconW(NIM_MODIFY, &nid); }
  void Remove() { if (added) { Shell_NotifyIconW(NIM_DELETE, &nid); added = false; } }
};

// async calls to the Miniserver (never block the UI thread)
struct AsyncResult { json r; std::string err; std::function<void(const json&, const std::string&)> cb; };
static HWND g_hwnd = nullptr; static std::mutex g_amx; static std::deque<std::unique_ptr<AsyncResult>> g_aq;
static void Async(std::function<json(std::string&)> work, std::function<void(const json&, const std::string&)> cb) {
  std::thread([work, cb] { auto r = std::make_unique<AsyncResult>(); r->cb = cb; try { r->r = work(r->err); } catch (std::exception& e) { r->err = e.what(); } { std::lock_guard<std::mutex> lk(g_amx); g_aq.push_back(std::move(r)); } PostMessageW(g_hwnd, WM_APP_RESULT, 0, 0); }).detach();
}
static void lockDpiAwareness() { typedef BOOL(WINAPI* SetCtx)(HANDLE); if (HMODULE u = GetModuleHandleW(L"user32.dll")) if (auto f = (SetCtx)GetProcAddress(u, "SetProcessDpiAwarenessContext")) { f((HANDLE)-1); return; } }
static std::wstring PickFolder(HWND owner, const std::wstring& start, const wchar_t* title) {
  std::wstring out; IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
  DWORD opts = 0; dlg->GetOptions(&opts); dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST); dlg->SetTitle(title);
  if (!start.empty() && cd::DirExists(start)) { IShellItem* si = nullptr; if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&si)))) { dlg->SetFolder(si); si->Release(); } }
  if (SUCCEEDED(dlg->Show(owner))) { IShellItem* item = nullptr; if (SUCCEEDED(dlg->GetResult(&item))) { PWSTR p = nullptr; if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = p; CoTaskMemFree(p); } item->Release(); } }
  dlg->Release(); return out;
}
static std::wstring W(const std::string& s) { return cd::Wide(s); }
static std::string U(const std::wstring& s) { return cd::Utf8(s); }

// a scrolling block of mono lines (recent changes, log)
class VLines : public VirtualWidgetImpl {
  std::vector<std::wstring> m_lines; float m_scroll = 0;
 public:
  const char* GetTypeName() const override { return "VLines"; }
  void Lines(std::vector<std::wstring> v) { m_lines = std::move(v); }
  void OnDraw(ID2D1RenderTarget* rt) override {
    vd::Fill(rt, m_bounds, vctl::Surface(), 6.0f); vd::Stroke(rt, m_bounds, vctl::Border(), 6.0f);
    const float lh = 17.0f; D2D1_RECT_F in = vd::Inset(m_bounds, 10.0f, 8.0f);
    float total = lh * (float)m_lines.size(), view = in.bottom - in.top, maxScroll = (std::max)(0.0f, total - view); if (m_scroll > maxScroll) m_scroll = maxScroll; if (m_scroll < 0) m_scroll = 0;
    rt->PushAxisAlignedClip(in, D2D1_ANTIALIAS_MODE_ALIASED);
    if (m_lines.empty()) vd::Text(rt, L"Nothing yet", in, vctl::Muted(), vd::Style().Size(11.5f));
    for (size_t i = (size_t)(m_scroll / lh); i < m_lines.size(); ++i) {
      float y = in.top + (float)i * lh - m_scroll; if (y > in.bottom) break;
      bool warn = m_lines[i].find(L"failed") != std::wstring::npos || m_lines[i].find(L"conflict") != std::wstring::npos || m_lines[i].find(L"cannot") != std::wstring::npos;
      vd::Text(rt, m_lines[i], vd::Rect(in.left, y, in.right - in.left, lh), warn ? D2D1::ColorF(0.80f, 0.25f, 0.20f) : vctl::Ink(), vd::Style().Size(11.5f).Mono());
    }
    rt->PopAxisAlignedClip();
  }
  VInputResult OnMouseWheel(float delta, float, float) override { m_scroll -= (delta > 0 ? 1.0f : -1.0f) * 3 * 17.0f; return VInputResult::Handled; }
};

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
  lockDpiAwareness();
  bool trayStart = false;
  { int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc); for (int i = 1; argv && i < argc; ++i) if (!wcscmp(argv[i], L"--tray")) trayStart = true; if (argv) LocalFree(argv); }
  const UINT WM_SHOWME = RegisterWindowMessageW(L"ChronoDrive.Show");
  HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\ChronoDrive.App");
  if (GetLastError() == ERROR_ALREADY_EXISTS) { PostMessageW(HWND_BROADCAST, WM_SHOWME, 0, 0); return 0; }
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ChronoControllerImpl::Instance();
  try {
  VirtualWindow win;
  if (!win.Create(hInstance, L"ChronoDrive " CD_VERSION_WSTR, 860, 720, false, 36.0f, trayStart)) return 1;
  win.SetBackground(vtheme::Current().window);
  g_hwnd = win.GetHWND();
  auto repaint = [] { InvalidateRect(g_hwnd, nullptr, FALSE); };
  Tray tray; tray.Add(g_hwnd, hInstance);
  auto showWindow = [&] { ShowWindow(g_hwnd, SW_SHOW); if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE); SetForegroundWindow(g_hwnd); };
  auto hideToTray = [&] { ShowWindow(g_hwnd, SW_HIDE); };

  // ---- state
  cd::Config cfg = cd::LoadConfig(); cd::Secrets sec; sec.Load();
  std::unique_ptr<cd::Engine> eng;
  json drives = json::array(), places = json::array();
  std::string linkCode, linkSecret, linkUrl; bool busy = false; float tick = 0.9f, tick10 = 0;
  std::function<void()> relayout;

  auto* msgDlg = win.AddChrome<VDialog>(); msgDlg->Attach(&win); msgDlg->Primary(L"OK");
  auto showMsg = [&](const std::wstring& title, const std::wstring& body) { msgDlg->Title(title).Body(body); msgDlg->Open(); repaint(); };
  auto* confirmDlg = win.AddChrome<VDialog>(); confirmDlg->Attach(&win); confirmDlg->Primary(L"Unlink").CloseText(L"Cancel").Accent(D2D1::ColorF(0.80f, 0.25f, 0.20f));

  auto lbl = [&](const wchar_t* t) { auto* l = win.Add<VLabel>(); l->Text(t).FontSize(12.0f).Muted(true); return l; };
  auto input = [&](const wchar_t* ph) { auto* b = win.Add<VChatInput>(); b->SetSingleLine(true).SetPlaceholder(ph).SetFontSize(12.5f); return b; };

  // ---- header: state
  auto* title = win.Add<VLabel>(); title->Text(L"ChronoDrive").FontSize(18.0f);
  auto* stateLbl = win.Add<VLabel>(); stateLbl->Text(L"").FontSize(13.0f);
  auto* detailLbl = win.Add<VLabel>(); detailLbl->Text(L"").FontSize(11.5f).Muted(true);
  auto* pauseBtn = win.Add<VButton>(); pauseBtn->Text(L"Pause");
  auto* openBtn = win.Add<VButton>(); openBtn->Text(L"Open folder");
  // ---- account
  auto* lAccount = lbl(L"Account");
  auto* accountLbl = win.Add<VLabel>(); accountLbl->Text(L"").FontSize(12.5f);
  auto* urlBox = input(L"https://cs-miniserver.chronoscan.org"); urlBox->SetText(W(cfg.url));
  auto* pairBtn = win.Add<VButton>(); pairBtn->Text(L"Pair this PC");
  auto* unlinkBtn = win.Add<VButton>(); unlinkBtn->Text(L"Unlink");
  // ---- drive
  auto* lDrive = lbl(L"Drive");
  auto* driveCombo = win.Add<VCombo>(); driveCombo->Items({L"(no drives yet)"}).Select(0).FontSize(12.5f);
  auto* refreshBtn = win.Add<VButton>(); refreshBtn->Text(L"Refresh");
  auto* lNew = lbl(L"New drive");
  auto* newName = input(L"Documents");
  auto* newSize = win.Add<VStepper>(); newSize->Range(1, 100000).Set(10);
  auto* lSize = lbl(L"GB");
  auto* nodeCombo = win.Add<VCombo>(); nodeCombo->Items({L"(no place yet)"}).Select(0).FontSize(12.5f);
  auto* backupChk = win.Add<VCheck>(L"Daily backup on the node");
  auto* createBtn = win.Add<VButton>(); createBtn->Text(L"Create drive");
  // ---- folder
  auto* lFolder = lbl(L"Local folder");
  auto* folderBox = input(L"C:\\Users\\you\\ChronoDrive\\Documents"); folderBox->SetText(cfg.folder);
  auto* browseBtn = win.Add<VButton>(); browseBtn->Text(L"Browse\x2026");
  auto* startBtn = win.Add<VButton>(); startBtn->Text(L"Start syncing");
  auto* autostartChk = win.Add<VCheck>(L"Start with Windows (tray icon)"); autostartChk->Set(AutostartEnabled());
  // ---- recent and log
  auto* lRecent = lbl(L"Recent changes");
  auto* recent = win.Add<VLines>();
  auto* lLog = lbl(L"Log");
  auto* logView = win.Add<VLines>();
  auto* footer = win.Add<VLabel>(); footer->Text(L"").FontSize(11.0f).Muted(true);

  auto linked = [&] { return !cfg.device_id.empty() && !sec.Get("device_secret").empty(); };
  auto driveIds = std::make_shared<std::vector<std::string>>(); auto placeKeys = std::make_shared<std::vector<std::string>>();
  auto fillAccount = [&] {
    accountLbl->Text(linked() ? W("Linked to " + cfg.email + " as " + (cfg.device_name.empty() ? cd::ComputerName() : cfg.device_name)) : L"Not paired. Enter the Miniserver address and press \"Pair this PC\": a code and a QR page open, approve it with your account.");
    pairBtn->Enabled(!linked() && linkCode.empty()); unlinkBtn->Enabled(linked());
  };
  auto fillDrives = [&] {
    std::vector<std::wstring> items; driveIds->clear(); int sel = 0;
    for (auto& d : drives) { if (d.value("deleting", false)) continue; driveIds->push_back(d.value("id", "")); if (d.value("id", "") == cfg.drive_id) sel = (int)driveIds->size() - 1;
      items.push_back(W(d.value("name", "") + "  \xc2\xb7  " + std::to_string(d.value("size_gb", 0)) + " GB on " + d.value("node", "") + (d.value("online", false) ? "" : "  (node offline)"))); }
    if (items.empty()) items.push_back(L"(no drives yet: create one below)");
    driveCombo->Items(items).Select(sel);
    std::vector<std::wstring> ps; placeKeys->clear();
    for (auto& p : places) { placeKeys->push_back(p.value("node", "") + "|" + p.value("fs_uuid", "")); ps.push_back(W(p.value("node", "") + " (" + p.value("kind", "") + ")  " + std::to_string(p.value("free_gb", 0)) + " GB free")); }
    if (ps.empty()) ps.push_back(L"(no node with room for a drive)");
    nodeCombo->Items(ps).Select(0);
    createBtn->Enabled(!placeKeys->empty());
  };
  auto loadDrives = [&] {
    if (!linked()) return;
    cd::Config c = cfg; std::string secret = sec.Get("device_secret");
    Async([c, secret](std::string& err) { cd::Miniserver ms(c.url, c.standby, c.device_id, secret); json d = ms.Json(L"GET", "/api/ms/drive/drives", nullptr, err); if (d.is_null()) return json(); json n = ms.Json(L"GET", "/api/ms/drive/nodes", nullptr, err); return json({{"drives", d["drives"]}, {"places", n.is_null() ? json::array() : n["nodes"]}}); },
      [&](const json& r, const std::string& err) { if (!err.empty()) { footer->Text(W("Miniserver: " + err)); repaint(); return; } drives = r["drives"]; places = r["places"]; fillDrives(); repaint(); });
  };
  auto startEngine = [&] {
    if (eng) { eng->Stop(); eng.reset(); }
    if (!linked() || cfg.drive_id.empty() || cfg.folder.empty()) return;
    eng = std::make_unique<cd::Engine>(cfg, sec.Get("device_secret")); eng->Pause(cfg.paused); eng->Start();
  };
  auto applyStatus = [&] {
    if (!eng) { stateLbl->Text(linked() ? L"Not syncing yet: pick a drive and a folder, then \"Start syncing\"." : L"Not paired."); detailLbl->Text(L""); tray.Tip(L"ChronoDrive: not set up"); return; }
    auto st = eng->Status();
    std::wstring s = st.state == "idle" ? L"Up to date" : st.state == "syncing" ? L"Syncing" + (st.pending ? L" (" + std::to_wstring(st.pending) + L" left" + (st.bytesTotal ? L", " + W(cd::Human(st.bytesDone)) + L" of " + W(cd::Human(st.bytesTotal)) : L"") + L")" : L"") : st.state == "paused" ? L"Paused" : st.state == "offline" ? L"Waiting: drive not reachable" : st.state == "connecting" ? L"Connecting" : st.state == "error" ? L"Stopped" : L"Setup";
    stateLbl->Text(s); detailLbl->Text(W(st.detail) + (st.nodeUrl.empty() ? L"" : L"   \xc2\xb7   " + W(st.nodeUrl)) + (st.lastSync ? L"   \xc2\xb7   last sync " + W(cd::TimeLocal(st.lastSync)) : L""));
    tray.Tip(L"ChronoDrive \xe2\x80\x94 " + s);
    pauseBtn->Text(st.state == "paused" ? L"Resume" : L"Pause");
    std::vector<std::wstring> lines; for (auto& r : st.recent) lines.push_back(W(cd::TimeLocal(r.at) + "  " + r.what + "  " + r.path + (r.by.empty() ? "" : "  (" + r.by + ")")));
    recent->Lines(lines);
  };
  auto refreshLog = [&] { std::vector<std::wstring> l; for (auto& s : cd::LogTail(200)) l.push_back(W(s)); logView->Lines(l); };

  pairBtn->OnClick([&] {
    std::string url = U(urlBox->GetText()); while (!url.empty() && url.back() == '/') url.pop_back(); if (url.empty()) url = cd::kDefaultServer;
    linkUrl = url; pairBtn->Enabled(false); accountLbl->Text(L"Asking the Miniserver for a pairing code\x2026"); repaint();
    Async([url](std::string& err) { return cd::Miniserver::LinkStart(url, cd::ComputerName(), err); }, [&](const json& r, const std::string& err) {
      if (!err.empty()) { accountLbl->Text(W("Cannot start pairing: " + err)); pairBtn->Enabled(true); repaint(); return; }
      linkCode = r.value("code", ""); linkSecret = r.value("secret", "");
      ShellExecuteW(nullptr, L"open", W(r.value("qr_page", "")).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      accountLbl->Text(W("Pairing code " + linkCode + ": scan the QR or approve it at " + r.value("link_url", "") + " \xe2\x80\xa6")); repaint();
    });
  });
  confirmDlg->OnResult([&](int r) { if (r != 0) return; if (eng) { eng->Stop(); eng.reset(); } sec.Clear(); sec.Save(); cfg.email.clear(); cfg.device_id.clear(); cfg.drive_id.clear(); cd::SaveConfig(cfg); DeleteFileW(cd::IndexPath().c_str()); drives = json::array(); places = json::array(); fillAccount(); fillDrives(); applyStatus(); repaint(); });
  unlinkBtn->OnClick([&] { confirmDlg->Title(L"Unlink this PC?").Body(L"The account's credentials are forgotten on this PC and syncing stops. The files in the local folder and on the drive stay as they are."); confirmDlg->Open(); repaint(); });
  refreshBtn->OnClick([&] { loadDrives(); });
  createBtn->OnClick([&] {
    int s = (int)nodeCombo->SelectedIndex(); if (s < 0 || s >= (int)placeKeys->size()) return;
    std::string key = (*placeKeys)[(size_t)s], node = key.substr(0, key.find('|')), fs = key.substr(key.find('|') + 1), name = U(newName->GetText());
    if (name.empty()) { showMsg(L"New drive", L"Give the drive a name."); return; }
    json body = {{"name", name}, {"node", node}, {"fs_uuid", fs}, {"size_gb", newSize->Value()}, {"backup", backupChk->Value()}};
    cd::Config c = cfg; std::string secret = sec.Get("device_secret"); createBtn->Enabled(false);
    Async([c, secret, body](std::string& err) { cd::Miniserver ms(c.url, c.standby, c.device_id, secret); return ms.Json(L"POST", "/api/ms/drive/drives", body, err); }, [&](const json& r, const std::string& err) {
      createBtn->Enabled(true); if (!err.empty()) { showMsg(L"Cannot create the drive", W(err)); return; }
      cfg.drive_id = r.value("id", ""); cfg.drive_name = r.value("name", ""); cfg.drive_node = r.value("node", ""); cfg.drive_node_kind = r.value("node_kind", ""); cd::SaveConfig(cfg); loadDrives();
    });
  });
  browseBtn->OnClick([&] { std::wstring p = PickFolder(g_hwnd, folderBox->GetText(), L"Folder that mirrors the drive on this PC"); if (!p.empty()) { folderBox->SetText(p); repaint(); } });
  startBtn->OnClick([&] {
    int s = (int)driveCombo->SelectedIndex(); if (s < 0 || s >= (int)driveIds->size()) { showMsg(L"Drive", L"Pick a drive (or create one)."); return; }
    std::wstring folder = folderBox->GetText(); if (folder.empty()) { showMsg(L"Folder", L"Pick the local folder."); return; }
    std::string id = (*driveIds)[(size_t)s];
    for (auto& d : drives) if (d.value("id", "") == id) { cfg.drive_name = d.value("name", ""); cfg.drive_node = d.value("node", ""); cfg.drive_node_kind = d.value("node_kind", ""); }
    if (cfg.drive_id != id || cfg.folder != folder) DeleteFileW(cd::IndexPath().c_str());
    cfg.drive_id = id; cfg.folder = folder; cfg.paused = false; cd::SaveConfig(cfg); cd::EnsureDir(folder);
    startEngine(); applyStatus(); repaint();
  });
  pauseBtn->OnClick([&] { if (!eng) return; cfg.paused = !eng->Paused(); eng->Pause(cfg.paused); cd::SaveConfig(cfg); applyStatus(); repaint(); });
  openBtn->OnClick([&] { if (!cfg.folder.empty()) ShellExecuteW(nullptr, L"open", cfg.folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
  autostartChk->OnChange([&](bool on) { SetAutostart(on); });

  auto layout = [&] {
    RECT rc; GetClientRect(g_hwnd, &rc); float Wd = (float)rc.right, H = (float)rc.bottom, pad = 16.0f, cw = Wd - 2 * pad, y = pad;
    title->SetBounds(vd::Rect(pad, y, 300, 26)); pauseBtn->SetBounds(vd::Rect(Wd - pad - 230, y, 100, 30)); openBtn->SetBounds(vd::Rect(Wd - pad - 120, y, 120, 30)); y += 30;
    stateLbl->SetBounds(vd::Rect(pad, y, cw, 22)); y += 22; detailLbl->SetBounds(vd::Rect(pad, y, cw, 18)); y += 28;
    lAccount->SetBounds(vd::Rect(pad, y, cw, 18)); y += 20; accountLbl->SetBounds(vd::Rect(pad, y, cw, 20)); y += 24;
    urlBox->SetBounds(vd::Rect(pad, y, (std::min)(cw - 280, 420.0f), 32)); pairBtn->SetBounds(vd::Rect(pad + (std::min)(cw - 280, 420.0f) + 10, y, 130, 32)); unlinkBtn->SetBounds(vd::Rect(pad + (std::min)(cw - 280, 420.0f) + 150, y, 100, 32)); y += 44;
    lDrive->SetBounds(vd::Rect(pad, y, cw, 18)); y += 20; driveCombo->SetBounds(vd::Rect(pad, y, cw - 110, 32)); refreshBtn->SetBounds(vd::Rect(Wd - pad - 100, y, 100, 32)); y += 40;
    lNew->SetBounds(vd::Rect(pad, y, cw, 18)); y += 20;
    newName->SetBounds(vd::Rect(pad, y, 200, 32)); newSize->SetBounds(vd::Rect(pad + 210, y, 90, 32)); lSize->SetBounds(vd::Rect(pad + 305, y + 7, 30, 18)); nodeCombo->SetBounds(vd::Rect(pad + 340, y, cw - 340 - 370, 32)); backupChk->SetBounds(vd::Rect(Wd - pad - 360, y + 4, 230, 24)); createBtn->SetBounds(vd::Rect(Wd - pad - 120, y, 120, 32)); y += 44;
    lFolder->SetBounds(vd::Rect(pad, y, cw, 18)); y += 20; folderBox->SetBounds(vd::Rect(pad, y, cw - 260, 32)); browseBtn->SetBounds(vd::Rect(Wd - pad - 250, y, 100, 32)); startBtn->SetBounds(vd::Rect(Wd - pad - 140, y, 140, 32)); y += 38;
    autostartChk->SetBounds(vd::Rect(pad, y, cw, 24)); y += 32;
    float rest = H - y - 30, rh = (std::max)(80.0f, rest * 0.45f), lh2 = (std::max)(60.0f, rest - rh - 48);
    lRecent->SetBounds(vd::Rect(pad, y, cw, 18)); y += 20; recent->SetBounds(vd::Rect(pad, y, cw, rh)); y += rh + 8;
    lLog->SetBounds(vd::Rect(pad, y, cw, 18)); y += 20; logView->SetBounds(vd::Rect(pad, y, cw, lh2));
    footer->SetBounds(vd::Rect(pad, H - 24, cw, 18));
    msgDlg->Cover(Wd, H); confirmDlg->Cover(Wd, H); repaint();
  };
  relayout = layout; win.OnResize(layout);
  fillAccount(); fillDrives(); layout(); loadDrives(); startEngine(); applyStatus(); refreshLog();

  win.OnMessage([&](UINT msg, WPARAM, LPARAM lp) -> bool {
    if (msg == WM_SHOWME) { showWindow(); return true; }
    if (msg == WM_CLOSE) { hideToTray(); return true; }
    if (msg == WM_APP_TRAY) {
      UINT ev = LOWORD(lp);
      if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK || ev == NIN_SELECT || ev == NIN_KEYSELECT) { showWindow(); return true; }
      if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
        HMENU m = CreatePopupMenu(); std::wstring st = stateLbl->GetText();
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, st.empty() ? L"ChronoDrive" : st.c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, kMenuOpen, L"Open ChronoDrive");
        AppendMenuW(m, MF_STRING | (cfg.folder.empty() ? MF_GRAYED : 0), kMenuFolder, L"Open folder");
        AppendMenuW(m, MF_STRING | (eng ? 0 : MF_GRAYED), kMenuPause, eng && eng->Paused() ? L"Resume sync" : L"Pause sync");
        if (eng) { auto s = eng->Status(); if (!s.recent.empty()) { HMENU r = CreatePopupMenu(); int i = 0; for (auto& c : s.recent) AppendMenuW(r, MF_STRING | MF_GRAYED, kMenuRecent + i++, W(cd::TimeLocal(c.at) + "  " + c.what + "  " + c.path).c_str()); AppendMenuW(m, MF_POPUP, (UINT_PTR)r, L"Recent changes"); } }
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, kMenuExit, L"Exit (syncing stops)");
        POINT pt; GetCursorPos(&pt); SetForegroundWindow(g_hwnd);
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, nullptr);
        PostMessageW(g_hwnd, WM_NULL, 0, 0); DestroyMenu(m);
        if (cmd == kMenuOpen) showWindow();
        else if (cmd == kMenuFolder) ShellExecuteW(nullptr, L"open", cfg.folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else if (cmd == kMenuPause && eng) { cfg.paused = !eng->Paused(); eng->Pause(cfg.paused); cd::SaveConfig(cfg); }
        else if (cmd == kMenuExit) { tray.Remove(); PostQuitMessage(0); }
      }
      return true;
    }
    if (msg == WM_APP_RESULT) {
      for (;;) { std::unique_ptr<AsyncResult> r; { std::lock_guard<std::mutex> lk(g_amx); if (g_aq.empty()) break; r = std::move(g_aq.front()); g_aq.pop_front(); } try { if (r->cb) r->cb(r->r, r->err); } catch (std::exception& e) { showMsg(L"Unexpected error", W(e.what())); } }
      return true;
    }
    return false;
  });
  win.OnTick([&](float dt) {
    tick += dt; tick10 += dt;
    if (tick >= 1.0f) {
      tick = 0; applyStatus(); repaint();
      if (!linkCode.empty() && !busy) {
        busy = true; std::string url = linkUrl, code = linkCode, secret = linkSecret;
        Async([url, code, secret](std::string& err) { std::string st; json p = cd::Miniserver::LinkPoll(url, code, secret, st, err); if (p.is_null()) p = json::object(); p["_status"] = st; return p; }, [&](const json& p, const std::string& err) {
          busy = false; std::string st = p.value("_status", "");
          if (st == "pending") return;
          linkCode.clear();
          if (st != "approved") { accountLbl->Text(W("Pairing " + st + ": " + err)); pairBtn->Enabled(true); repaint(); return; }
          cfg.url = linkUrl; cfg.email = p.value("email", ""); cfg.device_id = p.value("device_id", ""); cfg.device_name = cd::ComputerName();
          sec.Set("device_secret", p.value("device_secret", "")); sec.Save(); cd::SaveConfig(cfg);
          fillAccount(); loadDrives(); repaint();
        });
      }
    }
    if (tick10 >= 10.0f) { tick10 = 0; refreshLog(); }
  });
  int rc = win.RunMessageLoop();
  if (eng) eng->Stop();
  tray.Remove();
  if (single) CloseHandle(single);
  CoUninitialize();
  return rc;
  } catch (std::exception& e) {
    std::string m = std::string("ChronoDrive stopped: ") + e.what(); cd::Log(m);
    MessageBoxA(nullptr, m.c_str(), "ChronoDrive", MB_ICONERROR); return 1;
  }
}

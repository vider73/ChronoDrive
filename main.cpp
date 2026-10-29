// main.cpp — ChronoDrive.exe: the tray app. Shows each of the account's drives as a card; "Show on this PC" puts a
// drive on this PC as a drive letter (Z:) or in a folder, and keeps it in sync (drives.hpp, sync.hpp).
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <deque>
#include <memory>
#include "drives.hpp"
#include "updater.hpp"
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
constexpr int kMaxCards = 6;
enum { kMenuOpen = 101, kMenuPause = 103, kMenuPage = 104, kMenuUpdate = 105, kMenuExit = 109, kMenuDrive = 200 };

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static std::wstring ExePathW() { wchar_t b[MAX_PATH * 2]; GetModuleFileNameW(nullptr, b, MAX_PATH * 2); return b; }
static bool AutostartEnabled() { HKEY k; if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false; wchar_t v[1024]; DWORD n = sizeof v, t = 0; bool on = RegQueryValueExW(k, L"ChronoDrive", nullptr, &t, (BYTE*)v, &n) == ERROR_SUCCESS && t == REG_SZ && n > 2; RegCloseKey(k); return on; }
static void SetAutostart(bool on) { HKEY k; if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return; if (on) { std::wstring v = L"\"" + ExePathW() + L"\" --tray"; RegSetValueExW(k, L"ChronoDrive", 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t))); } else RegDeleteValueW(k, L"ChronoDrive"); RegCloseKey(k); }

struct Tray {
  NOTIFYICONDATAW nid{}; bool added = false;
  void Add(HWND hwnd, HINSTANCE inst) {
    nid.cbSize = sizeof nid; nid.hWnd = hwnd; nid.uID = kTrayId; nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"ChronoDrive"); added = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    nid.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &nid);
  }
  void Tip(const std::wstring& t) { if (!added) return; nid.uFlags = NIF_TIP; wcsncpy_s(nid.szTip, t.c_str(), _TRUNCATE); Shell_NotifyIconW(NIM_MODIFY, &nid); }
  void Remove() { if (added) { Shell_NotifyIconW(NIM_DELETE, &nid); added = false; } }
};

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
static void OpenPath(const std::wstring& p) { if (!p.empty()) ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
static D2D1_COLOR_F Indigo() { return vtheme::IsDark() ? D2D1::ColorF(0xA5B4FC) : D2D1::ColorF(0x4338CA); }
static D2D1_COLOR_F StateColor(const std::string& st) {
  if (st == "idle") return D2D1::ColorF(0x059669);
  if (st == "syncing" || st == "connecting") return D2D1::ColorF(0x2563EB);
  if (st == "offline" || st == "paused") return D2D1::ColorF(0xD97706);
  if (st == "error") return D2D1::ColorF(0xDC2626);
  return vctl::Muted();
}

// ------------------------------------------------------------------ a drive card: a disk glyph, the name, where it lives, its state on this PC
struct CardData { std::wstring name, where, state, detail, share; std::string stateKey; bool onPc = false, selected = false, panel = false, shareWarn = false; };
class VDriveCard : public VirtualWidgetImpl {
 public:
  CardData d;
  const char* GetTypeName() const override { return "VDriveCard"; }
  void OnDraw(ID2D1RenderTarget* rt) override {
    vd::Fill(rt, m_bounds, vctl::Surface(), 10.0f);
    vd::Stroke(rt, m_bounds, d.selected ? Indigo() : vctl::Border(), 10.0f);
    if (d.panel) return;
    // the disk glyph: a rounded drive with its slot and its light
    D2D1_RECT_F g = vd::Rect(m_bounds.left + 18, m_bounds.top + 26, 44, 32);
    vd::Fill(rt, g, d.onPc ? Indigo() : vctl::Track(), 7.0f);
    vd::Fill(rt, vd::Rect(g.left + 8, g.top + 21, 20, 4), d.onPc ? D2D1::ColorF(1, 1, 1, 0.55f) : vctl::Border(), 2.0f);
    vd::Fill(rt, vd::Rect(g.right - 12, g.top + 20, 6, 6), d.onPc ? StateColor(d.stateKey) : vctl::Muted(), 3.0f);
    float x = m_bounds.left + 80, w = (std::max)(120.0f, m_bounds.right - x - 440);
    vd::Text(rt, d.name, vd::Rect(x, m_bounds.top + 12, w, 24), vctl::Ink(), vd::Style().Size(17).Bold());
    vd::Text(rt, d.where, vd::Rect(x, m_bounds.top + 38, w, 20), vctl::Muted(), vd::Style().Size(13.5f));
    std::wstring line = d.state + d.detail;
    vd::Text(rt, line, vd::Rect(x, m_bounds.top + 60, m_bounds.right - x - 16, 20), d.onPc ? StateColor(d.stateKey) : vctl::Muted(), vd::Style().Size(14).Bold());
    if (!d.share.empty()) vd::Text(rt, d.share, vd::Rect(x, m_bounds.top + 84, m_bounds.right - x - 16, 20), d.shareWarn ? D2D1::ColorF(0xD97706) : Indigo(), vd::Style().Size(13.5f));
  }
};

// a short list of lines (recent changes)
class VLines : public VirtualWidgetImpl {
  std::vector<std::wstring> m_lines;
 public:
  const char* GetTypeName() const override { return "VLines"; }
  void Lines(std::vector<std::wstring> v) { m_lines = std::move(v); }
  void OnDraw(ID2D1RenderTarget* rt) override {
    const float lh = 22.0f; D2D1_RECT_F in = m_bounds;
    rt->PushAxisAlignedClip(in, D2D1_ANTIALIAS_MODE_ALIASED);
    if (m_lines.empty()) vd::Text(rt, L"Nothing has changed yet.", vd::Rect(in.left, in.top, in.right - in.left, lh), vctl::Muted(), vd::Style().Size(13.5f));
    for (size_t i = 0; i < m_lines.size(); ++i) {
      float y = in.top + (float)i * lh; if (y + lh > in.bottom) break;
      bool warn = m_lines[i].find(L"conflict") != std::wstring::npos;
      vd::Text(rt, m_lines[i], vd::Rect(in.left, y, in.right - in.left, lh), warn ? D2D1::ColorF(0xD97706) : vctl::Ink(), vd::Style().Size(13.5f));
    }
    rt->PopAxisAlignedClip();
  }
};

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
  lockDpiAwareness();
  bool trayStart = false, justUpdated = false; std::string showId;   // --show <drive id>: open that drive's "show on this PC" panel; --updated: the setup restarted us
  { int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) { if (!wcscmp(argv[i], L"--tray")) trayStart = true; else if (!wcscmp(argv[i], L"--show") && i + 1 < argc) showId = cd::Utf8(argv[++i]); else if (!wcscmp(argv[i], L"--updated")) justUpdated = true; }
    if (argv) LocalFree(argv); }
  // One instance per data folder: CHRONODRIVE_DATA (tests, a second account) gets its own lock and its own "show" message.
  std::wstring inst = cd::EnvW(L"CHRONODRIVE_DATA"); for (auto& c : inst) if (c == L'\\' || c == L':' || c == L'/') c = L'_';
  const UINT WM_SHOWME = RegisterWindowMessageW((L"ChronoDrive.Show" + inst).c_str());
  HANDLE single = CreateMutexW(nullptr, TRUE, (L"Local\\ChronoDrive.App" + inst).c_str());
  if (GetLastError() == ERROR_ALREADY_EXISTS) { PostMessageW(HWND_BROADCAST, WM_SHOWME, 0, 0); return 0; }
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ChronoControllerImpl::Instance();
  try {
  VirtualWindow win;
  if (!win.Create(hInstance, L"ChronoDrive " CD_VERSION_WSTR, 840, 720, false, 36.0f, trayStart)) return 1;
  win.SetBackground(vtheme::Current().window);
  g_hwnd = win.GetHWND();
  auto repaint = [] { InvalidateRect(g_hwnd, nullptr, FALSE); };
  Tray tray; tray.Add(g_hwnd, hInstance);
  auto showWindow = [&] { ShowWindow(g_hwnd, SW_SHOW); if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE); SetForegroundWindow(g_hwnd); };

  // ---- state
  cd::Config cfg = cd::LoadConfig(); cd::Secrets sec; sec.Load();
  cd::DriveSet set(cfg, sec.Get("device_secret"));
  json drives = json::array();                    // the account's drives (from the Miniserver)
  std::string linkCode, linkSecret, linkUrl, mapId, serverNote; bool busy = false, loaded = false; float tick = 0.9f, tick30 = 0;
  // updates (updater.hpp): checked 1 minute after start and every 6 hours; a newer setup is downloaded and verified in
  // the background, and installed when every drive is up to date (the setup closes this app and starts it again)
  std::string updNote = justUpdated ? std::string("Updated to ") + CD_VERSION_STR : ""; std::wstring updSetup; std::string updVersion; bool updBusy = false; float updTick = 6 * 3600 - 60;
  auto linked = [&] { return !cfg.device_id.empty() && !sec.Get("device_secret").empty(); };

  auto* msgDlg = win.AddChrome<VDialog>(); msgDlg->Attach(&win); msgDlg->Primary(L"OK");
  auto showMsg = [&](const std::wstring& t, const std::wstring& b) { msgDlg->Title(t).Body(b); msgDlg->Open(); repaint(); };
  auto* confirmDlg = win.AddChrome<VDialog>(); confirmDlg->Attach(&win); confirmDlg->Primary(L"Remove").CloseText(L"Cancel").Accent(D2D1::ColorF(0xDC2626));
  std::function<void()> onConfirm;
  confirmDlg->OnResult([&](int r) { auto f = onConfirm; onConfirm = nullptr; if (r == 0 && f) f(); });
  auto ghost = [](VButton* b) { b->Face(vctl::Subtle()).FaceHover(vctl::Track()).FacePress(vctl::Border()).TextColor(vctl::Ink()); return b; };
  auto primary = [](VButton* b) { b->Face(Indigo()).FaceHover(D2D1::ColorF(0x3730A3)).FacePress(D2D1::ColorF(0x312E81)).TextColor(D2D1::ColorF(1, 1, 1)); return b; };

  // ---- header
  auto* title = win.Add<VLabel>(); title->Text(L"ChronoDrive").FontSize(22.0f);
  auto* accountLbl = win.Add<VLabel>(); accountLbl->Text(L"").FontSize(13.5f).Muted(true);
  auto* unlinkBtn = ghost(win.Add<VButton>()); unlinkBtn->Text(L"Unlink");
  // ---- pairing (only while not paired)
  auto* pairTitle = win.Add<VLabel>(); pairTitle->Text(L"Connect this PC to your Miniserver").FontSize(18.0f);
  auto* pairText = win.Add<VLabel>(); pairText->Text(L"A code opens in the browser: approve it with your account.").FontSize(14.0f).Muted(true);
  auto* urlBox = win.Add<VChatInput>(); urlBox->SetSingleLine(true).SetPlaceholder(L"https://cs-miniserver.chronoscan.org").SetFontSize(14.0f); urlBox->SetText(W(cfg.url));
  auto* pairBtn = primary(win.Add<VButton>()); pairBtn->Text(L"Pair this PC");
  // ---- drives
  auto* drivesTitle = win.Add<VLabel>(); drivesTitle->Text(L"Your drives").FontSize(18.0f);
  auto* newBtn = ghost(win.Add<VButton>()); newBtn->Text(L"New drive\x2026");
  auto* refreshBtn = ghost(win.Add<VButton>()); refreshBtn->Text(L"Refresh");
  auto* emptyLbl = win.Add<VLabel>(); emptyLbl->Text(L"").FontSize(14.0f).Muted(true);
  VDriveCard* cards[kMaxCards]; VButton* mainBtn[kMaxCards]; VButton* pauseBtn[kMaxCards]; VButton* removeBtn[kMaxCards]; VButton* shareBtn[kMaxCards];
  for (int i = 0; i < kMaxCards; ++i) {
    cards[i] = win.Add<VDriveCard>();
    shareBtn[i] = ghost(win.Add<VButton>()); shareBtn[i]->Text(L"Share");
    mainBtn[i] = primary(win.Add<VButton>());
    pauseBtn[i] = ghost(win.Add<VButton>()); pauseBtn[i]->Text(L"Pause");
    removeBtn[i] = ghost(win.Add<VButton>()); removeBtn[i]->Text(L"Remove from PC");
  }
  // ---- the "show on this PC" panel (under the chosen card)
  auto* mapPanel = win.Add<VDriveCard>(); mapPanel->d.panel = true; mapPanel->d.selected = true;
  auto* mapTitle = win.Add<VLabel>(); mapTitle->Text(L"Show it on this PC as").FontSize(14.5f);
  auto* mapMode = win.Add<VSegment>(std::vector<std::wstring>{L"A drive letter", L"A folder"});
  auto* letterCombo = win.Add<VCombo>(); letterCombo->Items({L"Z:"}).Select(0).FontSize(14.0f);
  auto* folderBox = win.Add<VChatInput>(); folderBox->SetSingleLine(true).SetPlaceholder(L"D:\\Drive").SetFontSize(14.0f);
  auto* browseBtn = ghost(win.Add<VButton>()); browseBtn->Text(L"Browse\x2026");
  auto* mapHint = win.Add<VLabel>(); mapHint->Text(L"").FontSize(13.0f).Muted(true);
  auto* mapOk = primary(win.Add<VButton>()); mapOk->Text(L"Show it");
  auto* mapCancel = ghost(win.Add<VButton>()); mapCancel->Text(L"Cancel");
  std::vector<std::wstring> letterChoices; int mapModeSel = 0; std::string openMapLater;
  // ---- the "share" panel (under the chosen card; the owner's drives): an email, Can edit / Read only, who has it
  constexpr int kMaxMembers = 5;
  auto* sharePanel = win.Add<VDriveCard>(); sharePanel->d.panel = true; sharePanel->d.selected = true;
  auto* shareTitle = win.Add<VLabel>(); shareTitle->Text(L"Share with").FontSize(14.5f);
  auto* shareEmail = win.Add<VChatInput>(); shareEmail->SetSingleLine(true).SetPlaceholder(L"their email").SetFontSize(14.0f);
  auto* shareRole = win.Add<VSegment>(std::vector<std::wstring>{L"Can edit", L"Read only"});
  auto* shareOk = primary(win.Add<VButton>()); shareOk->Text(L"Share");
  auto* shareHint = win.Add<VLabel>(); shareHint->Text(L"").FontSize(13.0f).Muted(true);
  VLabel* memberLbl[kMaxMembers]; VButton* memberDel[kMaxMembers];
  for (int i = 0; i < kMaxMembers; ++i) { memberLbl[i] = win.Add<VLabel>(); memberLbl[i]->Text(L"").FontSize(14.0f); memberDel[i] = ghost(win.Add<VButton>()); memberDel[i]->Text(L"Remove"); }
  auto* shareWeb = ghost(win.Add<VButton>()); shareWeb->Text(L"Change on the web");
  auto* shareClose = ghost(win.Add<VButton>()); shareClose->Text(L"Close");
  std::string shareId; int shareRoleSel = 0; bool shareBusy = false; std::string shareNote; bool shareNoteBad = false;
  // ---- bottom
  auto* recentTitle = win.Add<VLabel>(); recentTitle->Text(L"Recent changes").FontSize(15.0f);
  auto* recent = win.Add<VLines>();
  auto* autostartChk = win.Add<VCheck>(L"Start with Windows"); autostartChk->Set(AutostartEnabled());
  auto* logBtn = ghost(win.Add<VButton>()); logBtn->Text(L"Log");
  auto* footer = win.Add<VLabel>(); footer->Text(L"").FontSize(12.5f).Muted(true);

  std::function<void()> relayout;
  auto visibleDrives = [&] { std::vector<json> v; for (auto& d : drives) if (!d.value("deleting", false)) v.push_back(d); return v; };
  auto driveName = [&](const std::string& id) { for (auto& d : drives) if (d.value("id", "") == id) return d.value("name", std::string()); return std::string(); };
  auto driveById = [&](const std::string& id) { for (auto& d : drives) if (d.value("id", "") == id) return d; return json(); };
  auto membersOf = [&](const std::string& id) { json d = driveById(id); return d.is_object() ? d.value("members", json::array()) : json::array(); };
  auto cleanErr = [](std::string e) { if (e.rfind("http ", 0) == 0) { size_t c = e.find(": "); if (c != std::string::npos) e = e.substr(c + 2); } return e; };
  auto stateText = [&](const cd::Mapping& m, std::string& key, std::wstring& detail) -> std::wstring {
    auto st = set.Status(m.id); key = st.state; detail.clear();
    std::wstring at = cd::ShownAt(m);
    if (!set.Running(m.id)) { key = "error"; return at + L"   \x00b7   not syncing"; }
    if (st.state == "idle" && st.readOnly) return at + L"   \x00b7   Up to date, read only" + (st.localOnly ? L"   \x00b7   " + std::to_wstring(st.localOnly) + (st.localOnly == 1 ? L" change of yours stays on this PC" : L" changes of yours stay on this PC") : L"");
    if (st.state == "idle") return at + L"   \x00b7   Up to date";
    if (st.state == "syncing") return at + L"   \x00b7   Syncing" + (st.pending ? L", " + std::to_wstring(st.pending) + L" left" : L"") + (st.bytesTotal ? L" (" + W(cd::Human(st.bytesDone)) + L" of " + W(cd::Human(st.bytesTotal)) + L")" : L"");
    if (st.state == "paused") return at + L"   \x00b7   Paused";
    if (st.state == "offline") { detail = st.detail.empty() ? L"" : L"   " + W(st.detail); return at + L"   \x00b7   Waiting"; }
    if (st.state == "error") { detail = L"   " + W(st.detail); return at + L"   \x00b7   Stopped"; }
    return at + L"   \x00b7   Connecting\x2026";
  };
  auto fill = [&] {
    bool paired = linked();
    accountLbl->Text(paired ? W(cfg.email.empty() ? "Paired" : cfg.email) : L"");
    unlinkBtn->SetVisible(paired);
    pairTitle->SetVisible(!paired); pairText->SetVisible(!paired); urlBox->SetVisible(!paired); pairBtn->SetVisible(!paired);
    pairBtn->Enabled(linkCode.empty());
    drivesTitle->SetVisible(paired); newBtn->SetVisible(paired); refreshBtn->SetVisible(paired); recentTitle->SetVisible(paired); recent->SetVisible(paired);
    auto vis = visibleDrives();
    emptyLbl->Text(!paired ? L"" : !loaded ? L"Loading your drives\x2026" : vis.empty() ? L"You have no drive yet. \"New drive\x2026\" creates one on the web." : L"");
    emptyLbl->SetVisible(paired && !emptyLbl->GetText().empty());
    for (int i = 0; i < kMaxCards; ++i) {
      bool on = paired && i < (int)vis.size();
      cards[i]->SetVisible(on); mainBtn[i]->SetVisible(on); pauseBtn[i]->SetVisible(false); removeBtn[i]->SetVisible(false); shareBtn[i]->SetVisible(on);
      if (!on) continue;
      const json& d = vis[(size_t)i]; std::string id = d.value("id", "");
      CardData c; c.name = W(d.value("name", ""));
      std::string node = d.value("node", ""); bool home = d.value("node_kind", "") == "home";
      c.where = std::to_wstring(d.value("size_gb", 0)) + L" GB   \x00b7   " + W(cd::Human(d.value("used", (uint64_t)0))) + L" used   \x00b7   "
              + (home ? L"at home (" + W(node) + L")" : node == "office" ? std::wstring(L"in the office") : L"node " + W(node));
      c.selected = id == mapId || id == shareId;
      // sharing (API.md section 17): the owner sees who has it; a member sees whose it is and what it may do
      if (d.value("owner", true)) {
        json ms = d.value("members", json::array()); shareBtn[i]->Text(L"Share");
        if (!ms.empty()) { std::wstring who; for (size_t k = 0; k < ms.size() && k < 2; ++k) who += (k ? L", " : L"") + W(ms[k].value("email", "")); if (ms.size() > 2) who += L" and " + std::to_wstring(ms.size() - 2) + L" more"; c.share = L"Shared with " + who; }
      } else {
        bool ro = d.value("role", "") == "read"; shareBtn[i]->Text(L"Leave");
        c.share = L"Shared with you by " + W(d.value("owner_email", "")) + (ro ? L"   \x00b7   read only: your changes stay on this PC" : L"   \x00b7   you can edit"); c.shareWarn = ro;
      }
      if (auto* m = cfg.Find(id)) {
        c.onPc = true; c.state = stateText(*m, c.stateKey, c.detail);
        mainBtn[i]->Text(L"Open"); pauseBtn[i]->SetVisible(true); removeBtn[i]->SetVisible(true);
        pauseBtn[i]->Text(m->paused ? L"Resume" : L"Pause");
      } else {
        c.state = L"Not on this PC"; c.stateKey = "setup";
        mainBtn[i]->Text(L"Show on this PC");
      }
      cards[i]->d = c;
    }
    bool mapping = paired && !mapId.empty();
    mapPanel->SetVisible(mapping); mapTitle->SetVisible(mapping); mapMode->SetVisible(mapping); mapHint->SetVisible(mapping); mapOk->SetVisible(mapping); mapCancel->SetVisible(mapping);
    bool letter = mapModeSel == 0;
    letterCombo->SetVisible(mapping && letter); folderBox->SetVisible(mapping && !letter); browseBtn->SetVisible(mapping && !letter);
    bool sharing = paired && !shareId.empty();
    json sms = sharing ? membersOf(shareId) : json::array();
    for (auto* x : std::initializer_list<VirtualWidgetImpl*>{sharePanel, shareTitle, shareEmail, shareRole, shareOk, shareHint, shareWeb, shareClose}) x->SetVisible(sharing);
    for (int k = 0; k < kMaxMembers; ++k) {
      bool row = sharing && (k < (int)sms.size() || (k == 0 && sms.empty()));
      memberLbl[k]->SetVisible(row); memberDel[k]->SetVisible(row && k < (int)sms.size());
      if (!row) continue;
      if (sms.empty()) { memberLbl[k]->Text(L"Not shared with anybody yet.").Muted(true); continue; }
      if (k == kMaxMembers - 1 && sms.size() > (size_t)kMaxMembers) { memberLbl[k]->Text(L"and " + std::to_wstring(sms.size() - kMaxMembers + 1) + L" more (see them on the web)").Muted(true); memberDel[k]->SetVisible(false); continue; }
      const json& mm = sms[(size_t)k];
      memberLbl[k]->Text(W(mm.value("email", "")) + L"   \x00b7   " + (mm.value("role", "") == "read" ? L"read only" : L"can edit")).Muted(false);
    }
    shareOk->Enabled(!shareBusy);
    if (sharing) {
      bool home = driveById(shareId).value("node_kind", "") == "home";
      shareHint->Text(!shareNote.empty() ? W(shareNote) : home ? L"They need a Miniserver account. This drive is on your home node: they reach it only on your home network."
                                                               : L"They need a Miniserver account. It shows up in their ChronoDrive at once, and they get an email.");
      shareHint->Muted(!shareNoteBad);
    }
    mapHint->Text(letter ? L"Its own disk in Explorer. The files are kept in " + cd::BackingFolder(driveName(mapId)) + L"."
                         : L"The folder gets the drive's files and stays in step with it.");
    // tray and recent changes
    std::string all = set.Overall();
    tray.Tip(cfg.maps.empty() ? L"ChronoDrive" : all == "idle" ? L"ChronoDrive \x2014 up to date" : all == "syncing" ? L"ChronoDrive \x2014 syncing" : all == "offline" ? L"ChronoDrive \x2014 waiting for a drive" : L"ChronoDrive \x2014 " + W(all));
    std::vector<std::pair<time_t, std::wstring>> rs;
    for (auto& m : cfg.maps) for (auto& r : set.Status(m.id).recent) rs.push_back({r.at, W(cd::TimeLocal(r.at)) + L"    " + W(m.name) + L"    " + W(r.what) + L"   " + W(r.path)});
    std::sort(rs.begin(), rs.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<std::wstring> lines; for (size_t i = 0; i < rs.size() && i < 8; ++i) lines.push_back(rs[i].second);
    recent->Lines(lines);
    footer->Text(!serverNote.empty() ? W(serverNote) : !updNote.empty() ? W(updNote) : L"ChronoDrive " CD_VERSION_WSTR);
    if (relayout) relayout();
  };

  auto loadDrives = [&] {
    if (!linked()) return;
    cd::Config c = cfg; std::string secret = sec.Get("device_secret");
    Async([c, secret](std::string& err) { cd::Miniserver ms(c.url, c.standby, c.device_id, secret); json d = ms.Json(L"GET", "/api/ms/drive/drives", nullptr, err); return d.is_null() ? json() : d["drives"]; },
      [&](const json& r, const std::string& err) {
        if (!err.empty()) { serverNote = err == "revoked" ? "This PC was unlinked on the Miniserver: pair it again." : "Miniserver: " + err; fill(); repaint(); return; }
        serverNote.clear(); drives = r; loaded = true;
        if (!showId.empty()) { std::string id = showId; showId.clear(); for (auto& d : drives) if (d.value("id", "") == id && !cfg.Find(id)) openMapLater = id; }
        for (auto& m : cfg.maps) for (auto& d : drives) if (d.value("id", "") == m.id) { m.name = d.value("name", m.name); m.node = d.value("node", m.node); m.node_kind = d.value("node_kind", m.node_kind); }
        cd::SaveConfig(cfg); fill(); repaint();
      });
  };

  std::function<void(bool)> checkUpdate = [&](bool manual) {
    if (updBusy || !updSetup.empty()) return;
    updBusy = true; if (manual) { updNote = "Checking for updates\xe2\x80\xa6"; fill(); repaint(); }
    cd::Config c = cfg; std::string secret = sec.Get("device_secret"); bool paired = linked();
    Async([c, secret, paired](std::string& err) -> json {
      cd::Miniserver ms(c.url, c.standby, c.device_id, secret);
      cd::upd::Info i = cd::upd::Check(ms, CD_VERSION_STR);
      if (!i.error.empty()) { err = i.error; return json(); }
      json out = {{"available", i.available}, {"version", i.version}};
      if (i.available && paired) {
        std::wstring setup; std::string e = cd::upd::Download(ms, i, setup);
        if (!e.empty()) { err = e; return out; }
        out["setup"] = cd::Utf8(setup);
      }
      return out; },
      [&, manual](const json& r, const std::string& err) {
        updBusy = false;
        if (!err.empty()) { cd::Log("update: " + err); if (manual) updNote = "Update check failed: " + err; fill(); repaint(); return; }
        if (!r.value("available", false)) { if (manual) updNote = "ChronoDrive " CD_VERSION_STR " is up to date"; fill(); repaint(); return; }
        updVersion = r.value("version", "");
        if (r.contains("setup")) { updSetup = cd::Wide(r.value("setup", "")); updNote = "Version " + updVersion + " is ready: it installs when every drive is up to date"; cd::Log("update: " + updVersion + " downloaded and verified"); }
        else updNote = "Version " + updVersion + " is available on the ChronoDrive page";
        fill(); repaint();
      });
  };
  auto installIfReady = [&] {
    if (updSetup.empty()) return;
    std::string all = set.Overall();
    if (all != "idle" && all != "setup" && all != "offline" && all != "paused") return;   // never in the middle of a sync
    std::string e = cd::upd::Install(updSetup);
    if (!e.empty()) { cd::Log("update: " + e); updNote = "Update " + updVersion + " failed: " + e; updSetup.clear(); fill(); repaint(); return; }
    tray.Remove(); ExitProcess(0);   // the setup replaces us now (see updater.hpp)
  };

  auto openMap = [&](const std::string& id) {
    mapId = id; shareId.clear();
    cd::Mapping* m = cfg.Find(id);
    letterChoices = cd::FreeLetters(m ? m->letter : L"");
    std::vector<std::wstring> items; for (auto& L : letterChoices) items.push_back(L + L":");
    if (items.empty()) items.push_back(L"(none free)");
    letterCombo->Items(items).Select(0);
    folderBox->SetText(cd::JoinPath(cd::JoinPath(cd::EnvW(L"USERPROFILE"), L"ChronoDrive"), cd::SafeName(driveName(id))));
    mapModeSel = 0; mapMode->Set(0);
    fill(); repaint();
  };
  auto idAt = [&](int i) { auto vis = visibleDrives(); return i < (int)vis.size() ? vis[(size_t)i].value("id", std::string()) : std::string(); };
  for (int i = 0; i < kMaxCards; ++i) {
    mainBtn[i]->OnClick([&, i] {
      std::string id = idAt(i); if (id.empty()) return;
      if (auto* m = cfg.Find(id)) OpenPath(cd::ShownAt(*m) + (m->mode == "letter" ? L"\\" : L""));
      else openMap(id);
    });
    pauseBtn[i]->OnClick([&, i] { std::string id = idAt(i); if (auto* m = cfg.Find(id)) { set.Pause(id, !m->paused); cd::SaveConfig(cfg); fill(); repaint(); } });
    removeBtn[i]->OnClick([&, i] {
      std::string id = idAt(i); auto* m = cfg.Find(id); if (!m) return;
      confirmDlg->Title(L"Remove \"" + W(m->name) + L"\" from this PC?").Body(L"It stops syncing here" + std::wstring(m->mode == "letter" ? L" and its drive letter goes" : L"") + L". The files stay in " + m->folder + L". The drive on the Miniserver is not touched.").Primary(L"Remove");
      onConfirm = [&, id] { set.Remove(id); cd::SaveConfig(cfg); fill(); repaint(); };
      confirmDlg->Open(); repaint();
    });
  }
  auto doShare = [&](std::wstring emailW) {
    std::string email = U(emailW); while (!email.empty() && email.back() == ' ') email.pop_back(); while (!email.empty() && email.front() == ' ') email.erase(0, 1);
    if (email.empty() || shareId.empty() || shareBusy) { if (email.empty()) { shareNote = "Type their email."; shareNoteBad = true; fill(); repaint(); } return; }
    shareBusy = true; shareNote = "Sharing\xe2\x80\xa6"; shareNoteBad = false; fill(); repaint();
    cd::Config c = cfg; std::string secret = sec.Get("device_secret"), id = shareId, role = shareRoleSel == 1 ? "read" : "write";
    Async([c, secret, id, email, role](std::string& err) { cd::Miniserver ms(c.url, c.standby, c.device_id, secret); return ms.Json(L"POST", "/api/ms/drive/drives/" + id + "/members", json({{"email", email}, {"role", role}}), err); },
      [&, email, role](const json& r, const std::string& err) {
        shareBusy = false;
        if (!err.empty() || r.is_null()) { shareNote = cleanErr(err); shareNoteBad = true; shareEmail->SetText(cd::Wide(email)); fill(); repaint(); return; }
        shareNote = "Shared with " + email + (role == "read" ? " (read only)" : " (can edit)") + ". They got an email."; shareNoteBad = false;
        shareEmail->SetText(L""); loadDrives(); fill(); repaint();
      });
  };
  shareOk->OnClick([&] { doShare(shareEmail->GetText()); });
  shareEmail->OnSubmit([&](const std::wstring& t) { doShare(t); });
  shareRole->OnChange([&](int s) { shareRoleSel = s; });
  shareClose->OnClick([&] { shareId.clear(); shareNote.clear(); fill(); repaint(); });
  shareWeb->OnClick([&] { OpenPath(W(cfg.url + "/chronodrive/#mine")); });
  for (int k = 0; k < kMaxMembers; ++k) memberDel[k]->OnClick([&, k] {
    json ms = membersOf(shareId); if (k >= (int)ms.size()) return;
    std::string id = shareId, user = ms[(size_t)k].value("user", ""), email = ms[(size_t)k].value("email", "");
    confirmDlg->Title(L"Stop sharing \"" + W(driveName(id)) + L"\" with " + W(email) + L"?").Body(L"Their PCs stop syncing it and keep the files they have.").Primary(L"Remove");
    onConfirm = [&, id, user, email] {
      cd::Config c = cfg; std::string secret = sec.Get("device_secret");
      Async([c, secret, id, user](std::string& err) { cd::Miniserver ms(c.url, c.standby, c.device_id, secret); return ms.Json(L"DELETE", "/api/ms/drive/drives/" + id + "/members/" + user, nullptr, err); },
        [&, email](const json&, const std::string& err) { shareNote = err.empty() ? "No longer shared with " + email + "." : cleanErr(err); shareNoteBad = !err.empty(); loadDrives(); fill(); repaint(); });
    };
    confirmDlg->Open(); repaint();
  });
  for (int i = 0; i < kMaxCards; ++i) shareBtn[i]->OnClick([&, i] {
    std::string id = idAt(i); if (id.empty()) return;
    json d = driveById(id);
    if (d.value("owner", true)) { mapId.clear(); shareId = shareId == id ? "" : id; shareNote.clear(); shareNoteBad = false; shareEmail->SetText(L""); shareRoleSel = 0; shareRole->Set(0); fill(); repaint(); return; }
    std::string name = d.value("name", ""), owner = d.value("owner_email", "");
    confirmDlg->Title(L"Leave \"" + W(name) + L"\"?").Body(L"It stops syncing on this PC (the files here stay) and goes from your drives. " + W(owner) + L" can share it with you again.").Primary(L"Leave");
    onConfirm = [&, id, name] {
      cd::Config c = cfg; std::string secret = sec.Get("device_secret");
      Async([c, secret, id](std::string& err) { cd::Miniserver ms(c.url, c.standby, c.device_id, secret); return ms.Json(L"DELETE", "/api/ms/drive/drives/" + id + "/members/me", nullptr, err); },
        [&, id, name](const json&, const std::string& err) {
          if (!err.empty()) { showMsg(L"Cannot leave", W(cleanErr(err))); return; }
          if (cfg.Find(id)) { set.Remove(id); cd::SaveConfig(cfg); }
          for (size_t k = 0; k < drives.size(); ++k) if (drives[k].value("id", "") == id) { drives.erase(k); break; }
          serverNote = "You left \"" + name + "\"."; loadDrives(); fill(); repaint();
        });
    };
    confirmDlg->Open(); repaint();
  });
  mapMode->OnChange([&](int s) { mapModeSel = s; fill(); repaint(); });
  browseBtn->OnClick([&] { std::wstring p = PickFolder(g_hwnd, folderBox->GetText(), L"Folder for this drive"); if (!p.empty()) { folderBox->SetText(p); repaint(); } });
  mapCancel->OnClick([&] { mapId.clear(); fill(); repaint(); });
  mapOk->OnClick([&] {
    json d; for (auto& x : drives) if (x.value("id", "") == mapId) d = x;
    if (d.is_null()) { mapId.clear(); fill(); return; }
    cd::Mapping m; m.id = mapId; m.name = d.value("name", ""); m.node = d.value("node", ""); m.node_kind = d.value("node_kind", "");
    if (mapModeSel == 0) {
      size_t s = letterCombo->SelectedIndex(); if (s >= letterChoices.size()) { showMsg(L"No free letter", L"Every drive letter is taken on this PC. Use a folder instead."); return; }
      m.mode = "letter"; m.letter = letterChoices[s]; m.folder = cd::BackingFolder(m.name);
    } else {
      std::wstring f = folderBox->GetText(); if (f.empty()) { showMsg(L"Folder", L"Pick a folder."); return; }
      m.mode = "folder"; m.folder = f;
    }
    for (auto& o : cfg.maps) if (o.id != m.id && cd::LowerW(o.folder) == cd::LowerW(m.folder)) { showMsg(L"Folder in use", L"Another drive already uses " + m.folder + L"."); return; }
    set.Remove(m.id);
    cfg.maps.push_back(m);
    std::string err = set.Start(cfg.maps.back());
    if (!err.empty()) { set.Remove(m.id); showMsg(L"Cannot show it", W(err)); fill(); return; }
    cd::SaveConfig(cfg); mapId.clear(); fill(); repaint();
    if (m.mode == "letter") OpenPath(cd::LetterRoot(m.letter) + L"\\");
  });
  pairBtn->OnClick([&] {
    std::string url = U(urlBox->GetText()); while (!url.empty() && url.back() == '/') url.pop_back(); if (url.empty()) url = cd::kDefaultServer;
    linkUrl = url; pairBtn->Enabled(false); pairText->Text(L"Asking for a code\x2026"); repaint();
    Async([url](std::string& err) { return cd::Miniserver::LinkStart(url, cd::ComputerName(), err); }, [&](const json& r, const std::string& err) {
      if (!err.empty()) { pairText->Text(W("Cannot start: " + err)); pairBtn->Enabled(true); repaint(); return; }
      linkCode = r.value("code", ""); linkSecret = r.value("secret", "");
      OpenPath(W(r.value("qr_page", "")));
      pairText->Text(W("Code " + linkCode + ": approve it in the browser.")); repaint();
    });
  });
  unlinkBtn->OnClick([&] {
    confirmDlg->Title(L"Unlink this PC?").Body(L"Every drive stops syncing here and the drive letters go. The files stay on this PC and on the Miniserver.").Primary(L"Unlink");
    onConfirm = [&] { std::vector<std::string> ids; for (auto& m : cfg.maps) ids.push_back(m.id); for (auto& id : ids) set.Remove(id); sec.Clear(); sec.Save(); cfg.email.clear(); cfg.device_id.clear(); cd::SaveConfig(cfg); drives = json::array(); loaded = false; fill(); repaint(); };
    confirmDlg->Open(); repaint();
  });
  refreshBtn->OnClick([&] { loadDrives(); set.KickAll(); });
  newBtn->OnClick([&] { OpenPath(W(cfg.url + "/chronodrive/#mine")); });
  logBtn->OnClick([&] { OpenPath(cd::JoinPath(cd::LogDir(), L"chronodrive.log")); });
  autostartChk->OnChange([&](bool on) { SetAutostart(on); });

  auto layout = [&] {
    RECT rc; GetClientRect(g_hwnd, &rc); float Wd = (float)rc.right, H = (float)rc.bottom, pad = 22.0f, cw = Wd - 2 * pad, y = pad;
    title->SetBounds(vd::Rect(pad, y, 260, 30)); unlinkBtn->SetBounds(vd::Rect(Wd - pad - 90, y + 2, 90, 30));
    { float aw = vd::TextWidth(accountLbl->GetText(), vd::Style().Size(13.5f)) + 8; accountLbl->SetBounds(vd::Rect(Wd - pad - 102 - aw, y + 8, aw, 20)); }
    y += 52;
    if (!linked()) {
      pairTitle->SetBounds(vd::Rect(pad, y, cw, 26)); y += 32; pairText->SetBounds(vd::Rect(pad, y, cw, 22)); y += 34;
      float ub = (std::min)(cw - 170, 440.0f);
      urlBox->SetBounds(vd::Rect(pad, y, ub, 36)); pairBtn->SetBounds(vd::Rect(pad + ub + 12, y, 150, 36)); y += 56;
    }
    drivesTitle->SetBounds(vd::Rect(pad, y, 200, 26)); newBtn->SetBounds(vd::Rect(Wd - pad - 120, y - 2, 120, 32)); refreshBtn->SetBounds(vd::Rect(Wd - pad - 232, y - 2, 100, 32)); y += 40;
    if (emptyLbl->IsVisible()) { emptyLbl->SetBounds(vd::Rect(pad, y, cw, 22)); y += 34; }
    auto vis = visibleDrives();
    for (int i = 0; i < kMaxCards; ++i) {
      if (!cards[i]->IsVisible()) continue;
      const float ch = cards[i]->d.share.empty() ? 90.0f : 112.0f;
      cards[i]->SetBounds(vd::Rect(pad, y, cw, ch));
      float bx = Wd - pad - 16;
      if (removeBtn[i]->IsVisible()) {
        removeBtn[i]->SetBounds(vd::Rect(bx - 132, y + 16, 132, 32)); bx -= 140;
        pauseBtn[i]->SetBounds(vd::Rect(bx - 84, y + 16, 84, 32)); bx -= 92;
        mainBtn[i]->SetBounds(vd::Rect(bx - 84, y + 16, 84, 32)); bx -= 92;
      } else { mainBtn[i]->SetBounds(vd::Rect(bx - 150, y + 16, 150, 32)); bx -= 158; }
      shareBtn[i]->SetBounds(vd::Rect(bx - 76, y + 16, 76, 32));
      y += ch + 10;
      if (i < (int)vis.size() && vis[(size_t)i].value("id", "") == mapId && mapPanel->IsVisible()) {
        const float ph = 156.0f; float px = pad + 30, py = y;
        mapPanel->SetBounds(vd::Rect(pad + 14, py, cw - 14, ph));
        mapTitle->SetBounds(vd::Rect(px, py + 18, 190, 22)); mapMode->SetBounds(vd::Rect(px + 190, py + 13, 300, 32));
        letterCombo->SetBounds(vd::Rect(px, py + 58, 120, 36));
        folderBox->SetBounds(vd::Rect(px, py + 58, cw - 220, 36)); browseBtn->SetBounds(vd::Rect(px + cw - 210, py + 60, 110, 32));
        mapHint->SetBounds(vd::Rect(px, py + 104, cw - 320, 40));
        mapCancel->SetBounds(vd::Rect(Wd - pad - 16 - 100, py + ph - 48, 100, 34)); mapOk->SetBounds(vd::Rect(Wd - pad - 16 - 220, py + ph - 48, 110, 34));
        y += ph + 12;
      }
      if (i < (int)vis.size() && vis[(size_t)i].value("id", "") == shareId && sharePanel->IsVisible()) {
        int rows = 0; for (int k = 0; k < kMaxMembers; ++k) if (memberLbl[k]->IsVisible()) ++rows;
        float ph = 100.0f + rows * 34.0f + 52.0f, px = pad + 30, py = y, inner = cw - 14 - 32;
        sharePanel->SetBounds(vd::Rect(pad + 14, py, cw - 14, ph));
        float ew = (std::max)(160.0f, inner - 100 - 210 - 90 - 30);
        shareTitle->SetBounds(vd::Rect(px, py + 20, 100, 22)); shareEmail->SetBounds(vd::Rect(px + 100, py + 14, ew, 36));
        shareRole->SetBounds(vd::Rect(px + 110 + ew, py + 16, 210, 32)); shareOk->SetBounds(vd::Rect(px + 130 + ew + 210, py + 15, 90, 34));
        shareHint->SetBounds(vd::Rect(px, py + 62, inner, 22));
        float ry = py + 96;
        for (int k = 0; k < kMaxMembers; ++k) { if (!memberLbl[k]->IsVisible()) continue; memberLbl[k]->SetBounds(vd::Rect(px, ry + 5, inner - 110, 22)); memberDel[k]->SetBounds(vd::Rect(px + inner - 96, ry, 96, 30)); ry += 34; }
        shareWeb->SetBounds(vd::Rect(px, py + ph - 46, 170, 34)); shareClose->SetBounds(vd::Rect(Wd - pad - 16 - 100, py + ph - 46, 100, 34));
        y += ph + 12;
      }
    }
    float by = (std::max)(y + 10, H - 250.0f);
    recentTitle->SetBounds(vd::Rect(pad, by, cw, 22)); recent->SetBounds(vd::Rect(pad, by + 30, cw, (std::max)(22.0f, H - by - 30 - 60)));
    autostartChk->SetBounds(vd::Rect(pad, H - 44, 220, 26)); logBtn->SetBounds(vd::Rect(Wd - pad - 70, H - 46, 70, 30));
    footer->SetBounds(vd::Rect(pad + 230, H - 40, cw - 320, 20));
    msgDlg->Cover(Wd, H); confirmDlg->Cover(Wd, H);
  };
  relayout = [&] { layout(); };
  win.OnResize([&] { layout(); repaint(); });
  if (linked()) { auto notes = set.StartAll(); cd::SaveConfig(cfg); if (!notes.empty()) serverNote = notes[0]; }
  fill(); loadDrives();
  cd::upd::CleanOld(CD_VERSION_STR);

  win.OnMessage([&](UINT msg, WPARAM wp, LPARAM lp) -> bool {
    if (msg == WM_SHOWME) { showWindow(); return true; }
    if (msg == WM_CLOSE) { ShowWindow(g_hwnd, SW_HIDE); return true; }
    // Windows signing out, or a setup's Restart Manager closing us to replace the files (WM_QUERYENDSESSION is left to
    // the default answer, yes): end at once
    if (msg == WM_ENDSESSION) { if (wp) { tray.Remove(); ExitProcess(0); } return true; }
    if (msg == WM_APP_TRAY) {
      UINT ev = LOWORD(lp);
      if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK || ev == NIN_SELECT || ev == NIN_KEYSELECT) { showWindow(); return true; }
      if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, kMenuOpen, L"Open ChronoDrive");
        if (!cfg.maps.empty()) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        for (size_t i = 0; i < cfg.maps.size() && i < 20; ++i) AppendMenuW(m, MF_STRING, kMenuDrive + (UINT)i, (W(cfg.maps[i].name) + L"   (" + cd::ShownAt(cfg.maps[i]) + L")").c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        bool anyRunning = false; for (auto& x : cfg.maps) if (!x.paused) anyRunning = true;
        AppendMenuW(m, MF_STRING | (cfg.maps.empty() ? MF_GRAYED : 0), kMenuPause, anyRunning ? L"Pause all" : L"Resume all");
        AppendMenuW(m, MF_STRING, kMenuPage, L"Your drives on the Miniserver");
        AppendMenuW(m, MF_STRING | (updBusy ? MF_GRAYED : 0), kMenuUpdate, updSetup.empty() ? L"Check for updates" : (L"Install version " + W(updVersion) + L" now").c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, kMenuExit, L"Exit (syncing stops)");
        POINT pt; GetCursorPos(&pt); SetForegroundWindow(g_hwnd);
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, nullptr);
        PostMessageW(g_hwnd, WM_NULL, 0, 0); DestroyMenu(m);
        if (cmd == kMenuOpen) showWindow();
        else if (cmd >= kMenuDrive && cmd < kMenuDrive + 20 && (size_t)(cmd - kMenuDrive) < cfg.maps.size()) { auto& x = cfg.maps[(size_t)(cmd - kMenuDrive)]; OpenPath(cd::ShownAt(x) + (x.mode == "letter" ? L"\\" : L"")); }
        else if (cmd == kMenuPause) { for (auto& x : cfg.maps) set.Pause(x.id, anyRunning); cd::SaveConfig(cfg); fill(); }
        else if (cmd == kMenuPage) OpenPath(W(cfg.url + "/chronodrive/#mine"));
        else if (cmd == kMenuUpdate) { if (updSetup.empty()) checkUpdate(true); else { std::string e = cd::upd::Install(updSetup); if (e.empty()) { tray.Remove(); ExitProcess(0); } else { updNote = "Update failed: " + e; fill(); } } }
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
    tick += dt; tick30 += dt;
    if (tick >= 1.0f) {
      tick = 0; fill(); repaint();
      if (!linkCode.empty() && !busy) {
        busy = true; std::string url = linkUrl, code = linkCode, secret = linkSecret;
        Async([url, code, secret](std::string& err) { std::string st; json p = cd::Miniserver::LinkPoll(url, code, secret, st, err); if (p.is_null()) p = json::object(); p["_status"] = st;
          if (st == "approved") {   // the email: PocketBase hides it in the pairing answer, the owner sees it on auth-refresh
            cd::Miniserver ms(url, "", p.value("device_id", ""), p.value("device_secret", "")); std::string e2; json me = ms.Json(L"POST", "/api/collections/users/auth-refresh", json::object(), e2);
            if (me.is_object()) p["_email"] = me.value("record", json::object()).value("email", "");
          }
          return p; }, [&](const json& p, const std::string& err) {
          busy = false; std::string st = p.value("_status", "");
          if (st == "pending") return;
          linkCode.clear();
          if (st != "approved") { pairText->Text(W("Pairing " + st + ": " + err)); pairBtn->Enabled(true); fill(); repaint(); return; }
          cfg.url = linkUrl; cfg.email = p.value("_email", ""); cfg.device_id = p.value("device_id", ""); cfg.device_name = cd::ComputerName();
          sec.Set("device_secret", p.value("device_secret", "")); sec.Save(); cd::SaveConfig(cfg); set.SetSecret(sec.Get("device_secret"));
          fill(); loadDrives(); repaint();
        });
      }
    }
    if (tick30 >= 30.0f) { tick30 = 0; loadDrives(); installIfReady(); }
    updTick += dt; if (updTick >= 6 * 3600) { updTick = 0; checkUpdate(false); }
    if (!openMapLater.empty()) { std::string id = openMapLater; openMapLater.clear(); openMap(id); }
  });
  int rc = win.RunMessageLoop();
  tray.Remove();
  if (single) CloseHandle(single);
  ExitProcess((UINT)rc);   // not set.StopAll(): a sync thread may be in a 30 s long poll; the indexes are already saved
  return rc;
  } catch (std::exception& e) {
    std::string m = std::string("ChronoDrive stopped: ") + e.what(); cd::Log(m);
    MessageBoxA(nullptr, m.c_str(), "ChronoDrive", MB_ICONERROR); return 1;
  }
}

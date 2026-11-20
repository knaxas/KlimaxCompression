#include "ui.h"
#include "install.h"

#define WM_CK_PROGRESS (WM_APP + 1)
#define WM_CK_LOG      (WM_APP + 2)
#define WM_CK_STATUS   (WM_APP + 3)
#define WM_CK_DONE     (WM_APP + 4)
#define WM_CK_ASKPW    (WM_APP + 5)
#define WM_CK_SHOWPW   (WM_APP + 6)
enum { ID_CANCEL = 1001, ID_INSTALL, ID_UNINSTALL, ID_HISTORY, ID_MODE0, ID_MODE1, ID_MODE2, ID_SECRET };
static const ULONG_PTR CK_COPYDATA_ID = 0xC0C0;
static const wchar_t* MAIN_CLASS = L"KlimaxCompressionWnd";

enum LogKind { LOG_INFO, LOG_OK, LOG_WARN, LOG_ERROR };
struct LogEntry { LogKind kind = LOG_INFO; std::wstring title, detail, time; fs::path reveal; std::wstring secret; };
struct StatusMsg { std::wstring name, detail; };
struct PwMsg { std::wstring name, pw; bool wrong = false, ok = false; };

static HWND gWnd, gBtnInstall, gBtnUninstall, gBtnCancel, gHistory;
static std::vector<LogEntry> gLog;
static std::wstring gStatusName, gStatusDetail = L"Bereit";
static int gProgress = 0;
static bool gUiBusy = false, gZoneHot = false, gDragOver = false, gTrackingMouse = false;

static HWND gBtnMode[3];
static std::atomic<int> gMode{MODE_FAST};
static const wchar_t* MODE_NAME[3] = {L"Schnell", L"Ausgewogen", L"Maximal"};
static const wchar_t* MODE_INFO[3] = {
  L"Blitzschnell, kleiner als ZIP",
  L"Kaum langsamer, noch kleiner",
  L"Am kleinsten, dauert Minuten"};

static HWND gBtnSecret;
static std::atomic<bool> gSecret{false};
static bool gModal = false;

static int loadMode() {
  DWORD v = MODE_FAST, sz = sizeof(v);
  if (RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, L"Modus", RRF_RT_REG_DWORD, NULL, &v, &sz) != ERROR_SUCCESS &&
      RegGetValueW(HKEY_CURRENT_USER, L"Software\\Compression King", L"Modus", RRF_RT_REG_DWORD, NULL, &v, &sz) != ERROR_SUCCESS) v = MODE_FAST;
  if (v > 2) v = MODE_FAST;
  return (int)v;
}
static void saveMode(int m) {
  HKEY k;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) return;
  DWORD v = (DWORD)m; RegSetValueExW(k, L"Modus", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
  RegCloseKey(k);
}
static RECT rModeText();
static void setMode(int m) {
  gMode = m;
  for (int i = 0; i < 3; i++) if (gBtnMode[i]) setButtonStyle(gBtnMode[i], i == m ? BTN_PRIMARY : BTN_GHOST);
  if (gWnd) { RECT t = rModeText(); InvalidateRect(gWnd, &t, FALSE); }
}

static std::mutex gQM; static std::deque<std::pair<int, std::wstring>> gQueue; static bool gBusy = false;
static std::atomic<bool> gCancel{false};
static std::thread gWorker;
static std::atomic<bool> gWorkerRunning{false};

static const int WIN_W = 720, WIN_H = 652;
static RECT rZone()       { return {S(24), S(148), S(696), S(328)}; }
static RECT rStatus()     { return {S(24), S(340), gUiBusy ? S(584) : S(696), S(372)}; }
static RECT rBar()        { return {S(24), S(384), S(696), S(384) + std::max(2, S(3))}; }
static RECT rStatusArea() { return {S(24), S(336), S(696), S(392)}; }
static RECT rModeText()   { return {S(380), S(92), S(592), S(124)}; }
static RECT rHistory()    { return {S(24), S(436), S(696), S(628)}; }

static std::wstring nowHHMM() {
  SYSTEMTIME t; GetLocalTime(&t);
  wchar_t b[16]; swprintf(b, 16, L"%02d:%02d", t.wHour, t.wMinute); return b;
}
static void addLog(LogEntry e) {
  e.time = nowHHMM();
  gLog.push_back(std::move(e));
  SendMessageW(gHistory, LB_INSERTSTRING, 0, (LPARAM)(gLog.size() - 1));
  SendMessageW(gHistory, LB_SETTOPINDEX, 0, 0);
}

static void drawHistoryItem(const DRAWITEMSTRUCT* d) {
  RECT r = d->rcItem; int w = r.right - r.left, h = r.bottom - r.top;
  if (w <= 0 || h <= 0) return;
  HDC dc = CreateCompatibleDC(d->hDC);
  HBITMAP bmp = CreateCompatibleBitmap(d->hDC, w, h), ob = (HBITMAP)SelectObject(dc, bmp);
  RECT b = {0, 0, w, h};
  fillRect(dc, b, (d->itemState & ODS_SELECTED) ? C_SURFACE_HI : C_SURFACE);
  if (d->itemID != (UINT)-1 && d->itemData < gLog.size()) {
    const LogEntry& e = gLog[d->itemData];
    COLORREF dot = e.kind == LOG_OK ? C_GOLD : e.kind == LOG_WARN ? C_WARN : e.kind == LOG_ERROR ? C_ERROR : C_FAINT;
    RECT rail = {S(14), S(12), S(14) + std::max(2, S(2)), S(42)};
    fillRect(dc, rail, dot);
    RECT rt = {S(30), S(8), w - S(64), S(28)};
    drawText(dc, e.title, rt, gF.bodyStrong, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT rm = {w - S(64), S(8), w - S(14), S(28)};
    drawText(dc, e.time, rm, gF.caption, C_FAINT, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    RECT rd = {S(30), S(28), w - S(14), S(46)};
    drawText(dc, e.detail, rd, gF.caption, C_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    hLine(dc, S(14), w - S(14), h - std::max(1, S(1)), C_LINE);
  }
  BitBlt(d->hDC, r.left, r.top, w, h, dc, 0, 0, SRCCOPY);
  SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc);
}

static void postStatus(const std::wstring& name, const std::wstring& detail) {
  PostMessageW(gWnd, WM_CK_STATUS, 0, (LPARAM)new StatusMsg{name, detail});
}
static void postLog(LogKind kind, const std::wstring& title, const std::wstring& detail, const fs::path& reveal = fs::path()) {
  LogEntry* e = new LogEntry; e->kind = kind; e->title = title; e->detail = detail; e->reveal = reveal;
  PostMessageW(gWnd, WM_CK_LOG, 0, (LPARAM)e);
}
static const wchar_t* SEP = L"   ·   ";

struct TempFile { fs::path p; ~TempFile() { std::error_code ec; if (!p.empty()) fs::remove(p, ec); } };

static void unlockArchive(const fs::path& p, const std::wstring& name, const fs::path& plain, Progress& prog) {
  bool wrong = false;
  for (;;) {
    postStatus(name, L"Wartet auf das Passwort …");
    PwMsg a; a.name = name; a.wrong = wrong;
    SendMessageW(gWnd, WM_CK_ASKPW, 0, (LPARAM)&a);
    if (!a.ok) throw CKCancelled();
    try { decryptFile(p, plain, asciiOnly(a.pw), prog); return; }
    catch (const CKWrongPassword&) { wrong = true; }
  }
}

static void processItem(const fs::path& p, int mode) {
  auto t0 = std::chrono::steady_clock::now();
  Progress prog; prog.cancel = &gCancel;
  std::wstring name = p.filename().wstring(); if (name.empty()) name = p.wstring();
  std::string lastPhase; auto phaseStart = t0;
  prog.cb = [&](const std::string& phase, U64 done, U64 total) {
    PostMessageW(gWnd, WM_CK_PROGRESS, total ? (WPARAM)(done * 1000 / total) : 0, 0);
    if (phase != lastPhase) { lastPhase = phase; phaseStart = std::chrono::steady_clock::now(); }
    double pel = std::chrono::duration<double>(std::chrono::steady_clock::now() - phaseStart).count();
    std::wstring d = W(phase) + L"  " + W(fmtSize(done)) + L" von " + W(fmtSize(total));
    if (done > 0 && pel > 1.0) {
      double speed = done / pel;
      d += SEP + W(fmtSize((U64)speed)) + L"/s" + SEP + L"noch ca. " + W(fmtTime((total - done) / std::max(1.0, speed)));
    }
    postStatus(name, d);
  };
  postStatus(name, L"Wird vorbereitet …");
  try {
    std::error_code ec;
    bool isCk = mode == 2 || (mode == 0 && fs::is_regular_file(p, ec) && isArchiveExt(lowerExt(p)));
    if (isCk) {
      TempFile plain; fs::path src = p;
      if (isEncryptedArchive(p)) { plain.p = secretTempFile(); unlockArchive(p, name, plain.p, prog); src = plain.p; }
      ExtractResult r = extractArchive(src, p.parent_path(), prog);
      double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      std::wstring d = std::to_wstring(r.files) + L" Dateien" + SEP + W(fmtSize(r.bytes)) + SEP + W(fmtTime(el));
      if (r.crcErrors) d += SEP + std::to_wstring(r.crcErrors) + L" Datei(en) mit CRC-Fehler – Archiv beschädigt";
      postLog(r.crcErrors ? LOG_WARN : LOG_OK, L"Entpackt:  " + name + L"  →  " + r.root.filename().wstring(), d, r.root);
    } else {
      fs::path pn = fs::absolute(p).lexically_normal(); if (!pn.has_filename()) pn = pn.parent_path();
      if (!pn.has_filename() || pn == pn.root_path()) throw CKError("Ganze Laufwerke koennen nicht komprimiert werden - bitte einen Ordner waehlen.");
      fs::path out = uniquePath(pn.parent_path() / (pn.filename().wstring() + L".kc"));
      int m = gMode.load();
      bool secret = gSecret.load();
      TempFile plain; if (secret) { fs::path t = out; t += L".tmp"; plain.p = uniquePath(t); }
      CompressResult r = compressPath(pn, secret ? plain.p : out, prog, 0, (CkMode)m);
      std::wstring pw;
      if (secret) {
        std::string p8 = makePassword();
        encryptFile(plain.p, out, p8, prog);
        pw = W(p8);
        r.out = out; r.outBytes = fs::file_size(out, ec);
      }
      double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      std::wstring d = W(fmtSize(r.inBytes)) + L"  →  " + W(fmtSize(r.outBytes)) + SEP + W(fmtPercent(r.outBytes, r.inBytes)) + SEP + W(fmtTime(el)) +
                       SEP + MODE_NAME[m];
      if (secret) d += std::wstring(SEP) + L"geheim (AES-256), Doppelklick zeigt das Passwort";
      if (r.dups) d += SEP + std::to_wstring(r.dups) + L" Duplikate";
      if (r.stored) d += SEP + std::to_wstring(r.stored) + L" bereits komprimiert";
      if (!secret) { postLog(LOG_OK, name + L"  →  " + r.out.filename().wstring(), d, r.out); return; }
      LogEntry* e = new LogEntry; e->kind = LOG_OK; e->title = name + L"  →  " + r.out.filename().wstring();
      e->detail = d; e->reveal = r.out; e->secret = pw;
      SendMessageW(gWnd, WM_CK_LOG, 0, (LPARAM)e);
      PwMsg s; s.name = r.out.filename().wstring(); s.pw = pw;
      SendMessageW(gWnd, WM_CK_SHOWPW, 0, (LPARAM)&s);
    }
  } catch (const CKCancelled&) {
    postLog(LOG_INFO, L"Abgebrochen:  " + name, L"Der Vorgang wurde abgebrochen.");
  } catch (const std::exception& ex) {
    postLog(LOG_ERROR, L"Fehler bei  " + name, W(ex.what()));
  }
}

static void workerMain() {
  for (;;) {
    std::pair<int, std::wstring> item;
    {
      std::lock_guard<std::mutex> lk(gQM);
      if (gQueue.empty() || gCancel.load()) { gQueue.clear(); gBusy = false; PostMessageW(gWnd, WM_CK_DONE, 0, 0); gWorkerRunning = false; return; }
      item = gQueue.front(); gQueue.pop_front();
    }
    processItem(fs::path(item.second), item.first);
  }
}

static void enqueue(const JobList& items) {
  bool start = false;
  {
    std::lock_guard<std::mutex> lk(gQM);
    for (auto& s : items) gQueue.push_back(s);
    if (!gBusy && !gQueue.empty()) { gBusy = true; start = true; }
  }
  if (start) {
    if (gWorker.joinable()) gWorker.join();
    gCancel = false;
    gUiBusy = true;
    gWorkerRunning = true;
    gWorker = std::thread(workerMain);
    EnableWindow(gBtnCancel, TRUE); ShowWindow(gBtnCancel, SW_SHOW);
    RECT z = rZone(), s = rStatusArea();
    InvalidateRect(gWnd, &z, FALSE); InvalidateRect(gWnd, &s, FALSE);
  }
}

static void setDragOver(bool on) {
  if (gDragOver == on) return;
  gDragOver = on; RECT z = rZone(); InvalidateRect(gWnd, &z, FALSE);
}

class DropTarget : public IDropTarget {
  LONG ref = 1; bool accept = false; IDropTargetHelper* helper = nullptr;
  static FORMATETC fmt() { FORMATETC f = {CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; return f; }
 public:
  DropTarget() { CoCreateInstance(CLSID_DragDropHelper, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper)); }
  virtual ~DropTarget() { if (helper) helper->Release(); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (riid == IID_IUnknown || riid == IID_IDropTarget) { *ppv = static_cast<IDropTarget*>(this); AddRef(); return S_OK; }
    *ppv = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
  ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* d, DWORD, POINTL pt, DWORD* eff) override {
    FORMATETC f = fmt(); accept = d->QueryGetData(&f) == S_OK;
    *eff = accept ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    if (helper) { POINT p = {pt.x, pt.y}; helper->DragEnter(gWnd, d, &p, *eff); }
    setDragOver(accept);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* eff) override {
    *eff = accept ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    if (helper) { POINT p = {pt.x, pt.y}; helper->DragOver(&p, *eff); }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragLeave() override {
    if (helper) helper->DragLeave();
    setDragOver(false);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Drop(IDataObject* d, DWORD, POINTL pt, DWORD* eff) override {
    *eff = accept ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    if (helper) { POINT p = {pt.x, pt.y}; helper->Drop(d, &p, *eff); }
    setDragOver(false);
    if (!accept) return S_OK;
    FORMATETC f = fmt(); STGMEDIUM sm = {};
    if (SUCCEEDED(d->GetData(&f, &sm))) {
      HDROP hd = (HDROP)sm.hGlobal; JobList items;
      UINT n = DragQueryFileW(hd, 0xFFFFFFFF, NULL, 0);
      for (UINT i = 0; i < n; i++) {
        UINT len = DragQueryFileW(hd, i, NULL, 0); std::wstring s(len + 1, 0);
        DragQueryFileW(hd, i, &s[0], len + 1); s.resize(len); items.push_back({0, s});
      }
      ReleaseStgMedium(&sm);
      enqueue(items);
    }
    return S_OK;
  }
};

static void updateInstallButtons() {
  bool inst = isInstalled();
  SetWindowTextW(gBtnInstall, !inst ? L"Installieren" : runningInstalled() ? L"Reparieren" : L"Aktualisieren");
  setButtonStyle(gBtnInstall, inst ? BTN_GHOST : BTN_PRIMARY);
  EnableWindow(gBtnUninstall, inst);
  InvalidateRect(gBtnUninstall, NULL, FALSE);
}

static void onInstall(HWND hwnd) {
  try {
    bool was = isInstalled();
    doInstall();
    updateInstallButtons();
    LogEntry e; e.kind = LOG_OK; e.title = was ? L"Installation aktualisiert" : L"Installiert";
    e.detail = installDir().wstring(); e.reveal = installDir() / L"KlimaxCompression.exe";
    addLog(e);
    if (!was)
      ckDialog(hwnd, DLG_INFO, L"Klimax Compression ist installiert",
        L"•  .kc-Dateien (und alte .ck) tragen jetzt das Kronen-Symbol und öffnen sich per Doppelklick.\n"
        L"•  Rechtsklick auf Dateien oder Ordner → „Mit Klimax Compression komprimieren“\n"
        L"    (unter Windows 11 bei „Weitere Optionen anzeigen“).\n"
        L"•  Rechtsklick auf eine .kc-Datei → „Hier entpacken“.\n"
        L"•  Im Startmenü unter „Klimax Compression“.");
  } catch (const std::exception& ex) {
    ckDialog(hwnd, DLG_ERROR, L"Installation fehlgeschlagen", W(ex.what()));
  }
}

static void onUninstall(HWND hwnd) {
  if (!ckDialog(hwnd, DLG_ASK, L"Klimax Compression deinstallieren?",
                L"Entfernt werden das installierte Programm, die Dateizuordnung für .kc, die Einträge im "
                L"Rechtsklick-Menü und die Startmenü-Verknüpfung.\n\nDeine Archive bleiben erhalten."))
    return;
  try {
    bool self = runningInstalled();
    doUninstall();
    if (self) {
      ckDialog(hwnd, DLG_INFO, L"Klimax Compression wurde deinstalliert", L"Das Programm wird jetzt geschlossen.");
      PostMessageW(hwnd, WM_CLOSE, 0, 0);
    } else {
      updateInstallButtons();
      LogEntry e; e.kind = LOG_INFO; e.title = L"Deinstalliert";
      e.detail = L"Dateizuordnung, Rechtsklick-Menü und Startmenü-Eintrag wurden entfernt.";
      addLog(e);
    }
  } catch (const std::exception& ex) {
    ckDialog(hwnd, DLG_ERROR, L"Deinstallation fehlgeschlagen", W(ex.what()));
  }
}

static void paintMain(HWND hwnd) {
  BufferedPaint bp(hwnd); HDC dc = bp.dc; RECT rc = bp.rc;
  fillRect(dc, rc, C_BG);

  if (gIconHeader) DrawIconEx(dc, S(24), S(21), gIconHeader, S(30), S(30), 0, NULL, DI_NORMAL);
  RECT rt = {S(66), S(14), S(420), S(58)};
  drawText(dc, L"Klimax Compression", rt, gF.brand, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  hLine(dc, 0, rc.right, S(72), C_LINE);

  drawText(dc, MODE_INFO[gMode.load()], rModeText(), gF.body, C_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

  RECT z = rZone();
  fillRect(dc, z, gDragOver ? RGB(38, 35, 28) : gZoneHot ? RGB(34, 32, 29) : C_SURFACE);
  frameRect(dc, z, gDragOver ? C_GOLD : gZoneHot ? C_LINE_HI : C_LINE);
  RECT ri = {z.left, z.top + S(22), z.right, z.top + S(64)};
  drawText(dc, L"", ri, gF.icon, gDragOver ? C_GOLD_HI : C_GOLD, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  RECT r1 = {z.left, z.top + S(74), z.right, z.top + S(98)};
  drawText(dc, gDragOver ? L"Loslassen zum Hinzufügen" : L"Dateien oder Ordner hierher ziehen", r1, gF.title, C_TEXT,
           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  RECT r2 = {z.left, z.top + S(100), z.right, z.top + S(120)};
  drawText(dc, gUiBusy ? L"Weitere Elemente werden der Reihe nach bearbeitet" : L"oder klicken, um Dateien auszuwählen", r2,
           gF.body, C_DIM, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  RECT r3 = {z.left, z.bottom - S(36), z.right, z.bottom - S(16)};
  drawText(dc, L"Name  →  Name.kc        .kc-Archive werden entpackt", r3, gF.caption, C_FAINT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

  RECT s = rStatus(); int nameW = 0;
  if (!gStatusName.empty()) {
    nameW = std::min(textWidth(dc, gStatusName, gF.bodyStrong), S(260));
    RECT rn = s; rn.right = s.left + nameW;
    drawText(dc, gStatusName, rn, gF.bodyStrong, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    nameW += S(14);
  }
  RECT rd = s; rd.left += nameW;
  drawText(dc, gStatusDetail, rd, gF.body, C_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  drawProgressBar(dc, rBar(), gProgress);

  RECT lab = {S(24), S(408), S(400), S(426)};
  drawLabel(dc, L"VERLAUF", lab, C_FAINT, DT_LEFT | DT_VCENTER);
  RECT ver = {S(400), S(408), S(696), S(426)};
  drawText(dc, L"Version " CK_APP_VERSION, ver, gF.caption, C_FAINT, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
  frameRect(dc, rHistory(), C_LINE);
}

static JobList parseCopyData(const COPYDATASTRUCT* cds) {
  JobList items;
  if (!cds || cds->dwData != CK_COPYDATA_ID || !cds->lpData) return items;
  std::wstring all((const wchar_t*)cds->lpData, cds->cbData / sizeof(wchar_t));
  size_t st = 0;
  while (st < all.size()) {
    size_t e = all.find(L'\n', st); if (e == std::wstring::npos) e = all.size();
    std::wstring line = all.substr(st, e - st);
    if (line.size() > 2 && line[1] == L'|') items.push_back({line[0] - L'0', line.substr(2)});
    st = e + 1;
  }
  return items;
}

static bool inZone(LPARAM lp) { RECT z = rZone(); POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)}; return PtInRect(&z, p) != 0; }

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: {
      gWnd = hwnd;
      windowOpened(); styleWindow(hwnd);
      gBtnInstall = makeButton(hwnd, ID_INSTALL, L"Installieren", BTN_PRIMARY);
      gBtnUninstall = makeButton(hwnd, ID_UNINSTALL, L"Deinstallieren", BTN_GHOST);
      gBtnCancel = makeButton(hwnd, ID_CANCEL, L"Abbrechen", BTN_GHOST);
      MoveWindow(gBtnInstall, S(432), S(20), S(128), S(32), FALSE);
      MoveWindow(gBtnUninstall, S(568), S(20), S(128), S(32), FALSE);
      MoveWindow(gBtnCancel, S(596), S(340), S(100), S(32), FALSE);
      {
        const int x[3] = {24, 128, 252}, w[3] = {96, 116, 108};
        for (int i = 0; i < 3; i++) {
          gBtnMode[i] = makeButton(hwnd, ID_MODE0 + i, MODE_NAME[i], BTN_GHOST);
          MoveWindow(gBtnMode[i], S(x[i]), S(92), S(w[i]), S(32), FALSE);
        }
        setMode(loadMode());
        gBtnSecret = makeButton(hwnd, ID_SECRET, L"Geheim", BTN_GHOST);
        MoveWindow(gBtnSecret, S(604), S(92), S(92), S(32), FALSE);
      }
      ShowWindow(gBtnCancel, SW_HIDE);
      RECT h = rHistory(); int b = std::max(1, S(1));
      gHistory = CreateWindowExW(0, L"LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                                 h.left + b, h.top + b, h.right - h.left - 2 * b, h.bottom - h.top - 2 * b, hwnd, (HMENU)(INT_PTR)ID_HISTORY, gHInst, NULL);
      SetWindowTheme(gHistory, L"DarkMode_Explorer", NULL);
      if (!isInstalled()) {
        LogEntry t; t.title = L"Tipp: Klimax Compression installieren";
        t.detail = L"Dann öffnen sich .kc-Dateien per Doppelklick, und im Rechtsklick-Menü erscheint ein eigener Eintrag.";
        addLog(t);
      }
      LogEntry wlc; wlc.title = L"Willkommen";
      wlc.detail = L"Neben jedem Element entsteht eine .kc-Datei. Doppelklick auf einen Eintrag zeigt das Ergebnis im Explorer.";
      addLog(wlc);
      updateInstallButtons();
      DropTarget* dt = new DropTarget;
      RegisterDragDrop(hwnd, dt);
      dt->Release();
      return 0;
    }
    case WM_PAINT: paintMain(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLORLISTBOX: SetBkColor((HDC)wp, C_SURFACE); SetTextColor((HDC)wp, C_TEXT); return (LRESULT)gBrSurface;
    case WM_MEASUREITEM: {
      MEASUREITEMSTRUCT* m = (MEASUREITEMSTRUCT*)lp;
      if (m->CtlType == ODT_LISTBOX) { m->itemHeight = S(54); return TRUE; }
      break;
    }
    case WM_DRAWITEM: {
      const DRAWITEMSTRUCT* d = (const DRAWITEMSTRUCT*)lp;
      if (d->CtlType == ODT_LISTBOX) drawHistoryItem(d); else drawButton(d);
      return TRUE;
    }
    case WM_MOUSEMOVE: {
      bool in = inZone(lp);
      if (in != gZoneHot) { gZoneHot = in; RECT z = rZone(); InvalidateRect(hwnd, &z, FALSE); }
      if (!gTrackingMouse) { TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&t); gTrackingMouse = true; }
      return 0;
    }
    case WM_MOUSELEAVE:
      gTrackingMouse = false;
      if (gZoneHot) { gZoneHot = false; RECT z = rZone(); InvalidateRect(hwnd, &z, FALSE); }
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT && (HWND)wp == hwnd) {
        POINT p; GetCursorPos(&p); ScreenToClient(hwnd, &p); RECT z = rZone();
        if (PtInRect(&z, p)) { SetCursor(LoadCursor(NULL, IDC_HAND)); return TRUE; }
      }
      break;
    case WM_LBUTTONUP:
      if (inZone(lp)) {
        JobList items;
        for (auto& f : pickFiles(hwnd)) items.push_back({0, f});
        if (!items.empty()) enqueue(items);
      }
      return 0;
    case WM_COPYDATA: {
      auto items = parseCopyData((const COPYDATASTRUCT*)lp);
      if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
      SetForegroundWindow(hwnd);
      if (!items.empty()) enqueue(items);
      return TRUE;
    }
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case ID_CANCEL: {
          gCancel = true; EnableWindow(gBtnCancel, FALSE);
          gStatusDetail = L"Wird abgebrochen …";
          RECT s = rStatusArea(); InvalidateRect(hwnd, &s, FALSE);
          break;
        }
        case ID_INSTALL: onInstall(hwnd); break;
        case ID_UNINSTALL: onUninstall(hwnd); break;
        case ID_MODE0: case ID_MODE1: case ID_MODE2:
          setMode(LOWORD(wp) - ID_MODE0); saveMode(gMode.load());
          break;
        case ID_SECRET: {
          gSecret = !gSecret.load();
          setButtonStyle(gBtnSecret, gSecret ? BTN_PRIMARY : BTN_GHOST);
          LogEntry e; e.kind = LOG_INFO;
          if (gSecret) {
            e.title = L"Geheim ist an";
            e.detail = L"Neue Archive werden mit AES-256 verschlüsselt. Danach bekommst du ein zufälliges Passwort, ohne das nichts mehr geht.";
          } else {
            e.title = L"Geheim ist aus";
            e.detail = L"Neue Archive werden wieder normal gepackt.";
          }
          addLog(e);
          break;
        }
        case ID_HISTORY:
          if (HIWORD(wp) == LBN_DBLCLK) {
            LRESULT i = SendMessageW(gHistory, LB_GETCURSEL, 0, 0);
            if (i != LB_ERR) {
              size_t k = (size_t)SendMessageW(gHistory, LB_GETITEMDATA, (WPARAM)i, 0);
              if (k < gLog.size()) {
                if (!gLog[k].secret.empty()) {
                  gModal = true; showPassword(hwnd, gLog[k].reveal.filename().wstring(), gLog[k].secret); gModal = false;
                } else revealInExplorer(gLog[k].reveal);
              }
            }
          }
          break;
      }
      return 0;
    case WM_CK_PROGRESS: {
      if (gCancel) return 0;
      gProgress = (int)wp;
      RECT b = rBar(); InvalidateRect(hwnd, &b, FALSE);
      return 0;
    }
    case WM_CK_LOG: { LogEntry* e = (LogEntry*)lp; addLog(*e); delete e; return 0; }
    case WM_CK_STATUS: {
      StatusMsg* s = (StatusMsg*)lp;
      if (!gCancel) { gStatusName = s->name; gStatusDetail = s->detail; RECT a = rStatusArea(); InvalidateRect(hwnd, &a, FALSE); }
      delete s;
      return 0;
    }
    case WM_CK_DONE: {
      gUiBusy = false; gProgress = 0; gStatusName.clear(); gStatusDetail = L"Bereit";
      ShowWindow(gBtnCancel, SW_HIDE);
      RECT z = rZone(), s = rStatusArea();
      InvalidateRect(hwnd, &z, FALSE); InvalidateRect(hwnd, &s, FALSE);
      if (GetForegroundWindow() != hwnd) {
        FLASHWINFO fi = {sizeof(fi), hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0}; FlashWindowEx(&fi);
      }
      return 0;
    }
    case WM_CK_ASKPW: {
      PwMsg* a = (PwMsg*)lp;
      if (gCancel || gModal) { a->ok = false; return 0; }
      gModal = true;
      a->ok = askPassword(hwnd, a->name, a->wrong, a->pw);
      gModal = false;
      return 0;
    }
    case WM_CK_SHOWPW: {
      PwMsg* s = (PwMsg*)lp;
      bool was = gModal; gModal = true;
      showPassword(hwnd, s->name, s->pw);
      gModal = was;
      return 0;
    }
    case WM_CLOSE:
      if (gModal) return 0;
      gCancel = true;
      while (gWorkerRunning) { MSG m; PeekMessageW(&m, NULL, 0, 0, PM_NOREMOVE); Sleep(5); }
      if (gWorker.joinable()) gWorker.join();
      DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
      RevokeDragDrop(hwnd);
      gWnd = NULL; windowClosed(); return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

bool forwardToRunningInstance(const JobList& jobs) {
  for (int t = 0; t < 60; t++) {
    HWND w = FindWindowW(MAIN_CLASS, NULL);
    if (w) {
      if (jobs.empty()) { if (IsIconic(w)) ShowWindow(w, SW_RESTORE); SetForegroundWindow(w); return true; }
      std::wstring all;
      for (auto& j : jobs) { all += (wchar_t)(L'0' + j.first); all += L'|'; all += j.second; all += L'\n'; }
      COPYDATASTRUCT cds; cds.dwData = CK_COPYDATA_ID; cds.cbData = (DWORD)(all.size() * sizeof(wchar_t)); cds.lpData = (void*)all.data();
      AllowSetForegroundWindow(ASFW_ANY);
      SendMessageW(w, WM_COPYDATA, 0, (LPARAM)&cds);
      return true;
    }
    Sleep(100);
  }
  return false;
}

void createMainWindow(int nShow, const JobList& jobs) {
  RECT r = {0, 0, S(WIN_W), S(WIN_H)};
  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
  AdjustWindowRectEx(&r, style, FALSE, 0);
  HWND h = CreateWindowExW(0, MAIN_CLASS, L"Klimax Compression", style, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, NULL, NULL, gHInst, NULL);
  ShowWindow(h, nShow); UpdateWindow(h);
  if (!jobs.empty()) enqueue(jobs);
}

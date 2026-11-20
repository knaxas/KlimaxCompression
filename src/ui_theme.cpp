#include "ui.h"

HINSTANCE gHInst;
double gScale = 1.0;
Fonts gF;
HBRUSH gBrBg, gBrSurface;
HICON gIconHeader;
static int gWindowCount = 0;

std::wstring W(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
  std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n); return w;
}

static HFONT makeFont(int px, int weight, const wchar_t* face) {
  return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                     CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

static void enableDarkMenus() {
  typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
  RtlGetVersionFn rgv = (RtlGetVersionFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
  OSVERSIONINFOW vi = {}; vi.dwOSVersionInfoSize = sizeof(vi);
  if (!rgv || rgv(&vi) != 0 || vi.dwMajorVersion < 10 || vi.dwBuildNumber < 18362) return;
  HMODULE ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!ux) return;
  typedef int(WINAPI * SetPreferredAppModeFn)(int);
  typedef void(WINAPI * FlushMenuThemesFn)();
  SetPreferredAppModeFn setMode = (SetPreferredAppModeFn)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(135));
  FlushMenuThemesFn flush = (FlushMenuThemesFn)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(136));
  if (setMode) setMode(2  );
  if (flush) flush();
}

void uiInit(HINSTANCE h) {
  gHInst = h;
  enableDarkMenus();
  HDC sdc = GetDC(NULL); gScale = GetDeviceCaps(sdc, LOGPIXELSX) / 96.0; ReleaseDC(NULL, sdc);
  gBrBg = CreateSolidBrush(C_BG); gBrSurface = CreateSolidBrush(C_SURFACE);
  gF.brand      = makeFont(25, FW_NORMAL, L"Georgia");
  gF.crumb      = makeFont(18, FW_NORMAL, L"Georgia");
  gF.title      = makeFont(17, FW_SEMIBOLD, L"Segoe UI");
  gF.body       = makeFont(14, FW_NORMAL, L"Segoe UI");
  gF.bodyStrong = makeFont(14, FW_SEMIBOLD, L"Segoe UI");
  gF.caption    = makeFont(12, FW_NORMAL, L"Segoe UI");
  gF.label      = makeFont(11, FW_SEMIBOLD, L"Segoe UI");
  gF.button     = makeFont(14, FW_SEMIBOLD, L"Segoe UI");
  gF.icon       = makeFont(30, FW_NORMAL, L"Segoe MDL2 Assets");
  gF.iconSmall  = makeFont(14, FW_NORMAL, L"Segoe MDL2 Assets");
  gIconHeader = (HICON)LoadImageW(h, MAKEINTRESOURCEW(1), IMAGE_ICON, S(30), S(30), 0);
}

void styleWindow(HWND h) {
  BOOL on = TRUE;            DwmSetWindowAttribute(h, 20  , &on, sizeof(on));
  COLORREF cap = C_BG;       DwmSetWindowAttribute(h, 35  , &cap, sizeof(cap));
  COLORREF border = C_LINE;  DwmSetWindowAttribute(h, 34  , &border, sizeof(border));
  COLORREF txt = C_TEXT;     DwmSetWindowAttribute(h, 36  , &txt, sizeof(txt));
}

void windowOpened() { gWindowCount++; }
void windowClosed() { if (--gWindowCount <= 0) PostQuitMessage(0); }

int messageLoop() {
  MSG m;
  while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
  return 0;
}

BufferedPaint::BufferedPaint(HWND h) : hwnd(h) {
  win = BeginPaint(h, &ps);
  GetClientRect(h, &rc);
  dc = CreateCompatibleDC(win);
  bmp = CreateCompatibleBitmap(win, std::max(1, (int)rc.right), std::max(1, (int)rc.bottom));
  old = (HBITMAP)SelectObject(dc, bmp);
  SetBkMode(dc, TRANSPARENT);
}
BufferedPaint::~BufferedPaint() {
  BitBlt(win, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
  SelectObject(dc, old); DeleteObject(bmp); DeleteDC(dc);
  EndPaint(hwnd, &ps);
}

void fillRect(HDC dc, const RECT& r, COLORREF c) {
  SetDCBrushColor(dc, c); FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}
void frameRect(HDC dc, const RECT& r, COLORREF c) {
  SetDCBrushColor(dc, c); FrameRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}
void hLine(HDC dc, int x1, int x2, int y, COLORREF c) {
  RECT r = {x1, y, x2, y + std::max(1, S(1))}; fillRect(dc, r, c);
}
void drawText(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT flags) {
  HGDIOBJ o = SelectObject(dc, f); SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT);
  DrawTextW(dc, s.c_str(), (int)s.size(), &r, flags | DT_NOPREFIX);
  SelectObject(dc, o);
}
int textWidth(HDC dc, const std::wstring& s, HFONT f) {
  HGDIOBJ o = SelectObject(dc, f); SIZE sz = {0, 0};
  GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
  SelectObject(dc, o); return sz.cx;
}
void drawLabel(HDC dc, const wchar_t* s, RECT r, COLORREF c, UINT flags) {
  int old = SetTextCharacterExtra(dc, std::max(1, S(1)));
  drawText(dc, s, r, gF.label, c, flags | DT_SINGLELINE);
  SetTextCharacterExtra(dc, old);
}
void drawProgressBar(HDC dc, const RECT& r, int permille) {
  fillRect(dc, r, C_SURFACE_HI);
  if (permille > 0) {
    RECT f = r; f.right = r.left + (int)((r.right - r.left) * (long long)std::min(permille, 1000) / 1000);
    fillRect(dc, f, C_GOLD);
  }
}

static LRESULT CALLBACK ButtonSubclass(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
  LONG_PTR ud = GetWindowLongPtrW(h, GWLP_USERDATA);
  switch (m) {
    case WM_MOUSEMOVE:
      if (!(ud & 0x100)) {
        SetWindowLongPtrW(h, GWLP_USERDATA, ud | 0x100);
        TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, h, 0}; TrackMouseEvent(&t);
        InvalidateRect(h, NULL, FALSE);
      }
      break;
    case WM_MOUSELEAVE:
      SetWindowLongPtrW(h, GWLP_USERDATA, ud & ~(LONG_PTR)0x100);
      InvalidateRect(h, NULL, FALSE);
      break;
    case WM_SETCURSOR:
      if (IsWindowEnabled(h)) { SetCursor(LoadCursor(NULL, IDC_HAND)); return TRUE; }
      break;
    case WM_LBUTTONDBLCLK:
      m = WM_LBUTTONDOWN;
      break;
    case WM_NCDESTROY:
      RemoveWindowSubclass(h, ButtonSubclass, 1);
      break;
  }
  return DefSubclassProc(h, m, wp, lp);
}

HWND makeButton(HWND parent, int id, const wchar_t* text, BtnStyle style) {
  HWND b = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                           parent, (HMENU)(INT_PTR)id, gHInst, NULL);
  SetWindowLongPtrW(b, GWLP_USERDATA, style);
  SetWindowSubclass(b, ButtonSubclass, 1, 0);
  return b;
}

void setButtonStyle(HWND btn, BtnStyle style) {
  LONG_PTR ud = GetWindowLongPtrW(btn, GWLP_USERDATA);
  SetWindowLongPtrW(btn, GWLP_USERDATA, (ud & ~(LONG_PTR)0xff) | style);
  InvalidateRect(btn, NULL, FALSE);
}

void drawButton(const DRAWITEMSTRUCT* d) {
  RECT r = d->rcItem;
  int w = r.right - r.left, h = r.bottom - r.top;
  HDC dc = CreateCompatibleDC(d->hDC);
  HBITMAP bmp = CreateCompatibleBitmap(d->hDC, w, h), ob = (HBITMAP)SelectObject(dc, bmp);
  RECT b = {0, 0, w, h};
  fillRect(dc, b, C_BG);

  LONG_PTR ud = GetWindowLongPtrW(d->hwndItem, GWLP_USERDATA);
  bool primary = (ud & 0xff) == BTN_PRIMARY, hot = (ud & 0x100) != 0;
  bool dis = (d->itemState & ODS_DISABLED) != 0, down = (d->itemState & ODS_SELECTED) != 0;
  bool focus = (d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT);

  COLORREF fill, border, text;
  if (primary) {
    fill = dis ? C_SURFACE_HI : down ? C_GOLD_DOWN : hot ? C_GOLD_HI : C_GOLD;
    border = fill; text = dis ? C_FAINT : C_ON_GOLD;
  } else {
    fill = dis ? C_BG : down ? C_SURFACE_HI : hot ? C_SURFACE : C_BG;
    border = dis ? C_SURFACE_HI : (hot || focus) ? C_LINE_HI : C_LINE;
    text = dis ? C_FAINT : C_TEXT;
  }
  if (focus) border = C_GOLD;
  HBRUSH br = CreateSolidBrush(fill); HPEN pen = CreatePen(PS_SOLID, std::max(1, S(1)), border);
  HGDIOBJ obr = SelectObject(dc, br), open = SelectObject(dc, pen);
  RoundRect(dc, 0, 0, w, h, S(6), S(6));
  SelectObject(dc, obr); SelectObject(dc, open); DeleteObject(br); DeleteObject(pen);

  wchar_t txt[128]; GetWindowTextW(d->hwndItem, txt, 128);
  drawText(dc, txt, b, (ud & 0xff) == BTN_ICON ? gF.iconSmall : gF.button, text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

  BitBlt(d->hDC, r.left, r.top, w, h, dc, 0, 0, SRCCOPY);
  SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc);
}

bool ckDialog(HWND owner, DlgKind kind, const std::wstring& heading, const std::wstring& text) {
  typedef HRESULT(WINAPI * TaskDialogFn)(HWND, HINSTANCE, PCWSTR, PCWSTR, PCWSTR, TASKDIALOG_COMMON_BUTTON_FLAGS, PCWSTR, int*);
  static TaskDialogFn td = (TaskDialogFn)(void*)GetProcAddress(GetModuleHandleW(L"comctl32.dll"), "TaskDialog");
  if (td) {
    PCWSTR icon = kind == DLG_WARN ? TD_WARNING_ICON : kind == DLG_ERROR ? TD_ERROR_ICON : MAKEINTRESOURCEW(1);
    HINSTANCE inst = (kind == DLG_INFO || kind == DLG_ASK) ? gHInst : NULL;
    int res = 0;
    if (SUCCEEDED(td(owner, inst, L"Klimax Compression", heading.c_str(), text.c_str(),
                     kind == DLG_ASK ? (TDCBF_YES_BUTTON | TDCBF_NO_BUTTON) : TDCBF_OK_BUTTON, icon, &res)))
      return kind == DLG_ASK ? res == IDYES : true;
  }
  UINT flags = kind == DLG_ASK ? (MB_YESNO | MB_ICONQUESTION) : kind == DLG_WARN ? MB_ICONWARNING : kind == DLG_ERROR ? MB_ICONERROR : MB_ICONINFORMATION;
  std::wstring all = heading + (text.empty() ? L"" : L"\n\n" + text);
  int r = MessageBoxW(owner, all.c_str(), L"Klimax Compression", flags);
  return kind == DLG_ASK ? r == IDYES : true;
}

bool pickFolder(HWND owner, const wchar_t* title, fs::path& out) {
  IFileOpenDialog* d = nullptr; bool ok = false;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d))) || !d) return false;
  DWORD o = 0; d->GetOptions(&o); d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  d->SetTitle(title);
  if (SUCCEEDED(d->Show(owner))) {
    IShellItem* it = nullptr;
    if (SUCCEEDED(d->GetResult(&it)) && it) {
      PWSTR p = nullptr;
      if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out = p; CoTaskMemFree(p); ok = true; }
      it->Release();
    }
  }
  d->Release();
  return ok;
}

std::vector<std::wstring> pickFiles(HWND owner) {
  std::vector<std::wstring> res;
  IFileOpenDialog* d = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d))) || !d) return res;
  DWORD o = 0; d->GetOptions(&o); d->SetOptions(o | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
  d->SetTitle(L"Dateien zum Komprimieren oder Entpacken wählen");
  if (SUCCEEDED(d->Show(owner))) {
    IShellItemArray* arr = nullptr;
    if (SUCCEEDED(d->GetResults(&arr)) && arr) {
      DWORD n = 0; arr->GetCount(&n);
      for (DWORD i = 0; i < n; i++) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(arr->GetItemAt(i, &it)) && it) {
          PWSTR p = nullptr;
          if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { res.push_back(p); CoTaskMemFree(p); }
          it->Release();
        }
      }
      arr->Release();
    }
  }
  d->Release();
  return res;
}

void revealInExplorer(const fs::path& p) {
  std::error_code ec;
  if (p.empty() || !fs::exists(p, ec)) return;
  std::wstring args = L"/select,\"" + p.wstring() + L"\"";
  ShellExecuteW(NULL, L"open", L"explorer.exe", args.c_str(), NULL, SW_SHOWNORMAL);
}

void registerWindowClasses() {
  HICON appIcon = LoadIconW(gHInst, MAKEINTRESOURCEW(1));
  if (!appIcon) appIcon = LoadIcon(NULL, IDI_APPLICATION);
  WNDCLASSEXW wc = {}; wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = MainProc; wc.hInstance = gHInst; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = gBrBg; wc.lpszClassName = L"KlimaxCompressionWnd"; wc.hIcon = appIcon; wc.hIconSm = appIcon;
  RegisterClassExW(&wc);
  WNDCLASSEXW vc = wc; vc.lpfnWndProc = ViewProc; vc.lpszClassName = L"KlimaxCompressionViewer";
  RegisterClassExW(&vc);
  WNDCLASSEXW sc = wc; sc.lpfnWndProc = SecretProc; sc.lpszClassName = L"KlimaxCompressionSecret";
  RegisterClassExW(&sc);
}

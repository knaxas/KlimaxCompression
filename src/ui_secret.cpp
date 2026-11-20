#include "ui.h"

enum { ID_S_COPY = 4001 };
enum SecretKind { SK_ASK, SK_SHOW };

struct SecretDlg {
  SecretKind kind = SK_ASK;
  std::wstring name, pw;
  bool wrong = false, done = false, result = false;
  HWND edit = NULL, copy = NULL;
  HFONT mono = NULL;
};

static const int ASK_W = 440, ASK_H = 206, SHOW_W = 460, SHOW_H = 252;

void setClipboardText(HWND owner, const std::wstring& text) {
  if (!OpenClipboard(owner)) return;
  EmptyClipboard();
  size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
    memcpy(GlobalLock(g), text.c_str(), bytes); GlobalUnlock(g);
    if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
  }
  CloseClipboard();
}

std::string asciiOnly(const std::wstring& s) {
  std::string r; for (wchar_t c : s) if (c > 0 && c < 128) r += (char)c; return r;
}

fs::path secretTempFile() {
  wchar_t tmp[MAX_PATH + 1] = {0}; GetTempPathW(MAX_PATH, tmp);
  fs::path dir = fs::path(tmp) / L"KlimaxCompression";
  std::error_code ec; fs::create_directories(dir, ec);
  return dir / (L"g" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()) + L".kc");
}

static void paintSecret(HWND hwnd, SecretDlg* d) {
  BufferedPaint bp(hwnd); HDC dc = bp.dc; RECT rc = bp.rc;
  fillRect(dc, rc, C_BG);
  int w = rc.right;
  if (d->kind == SK_ASK) {
    RECT h = {S(24), S(18), w - S(24), S(42)};
    drawText(dc, L"Passwort eingeben", h, gF.title, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT t = {S(24), S(46), w - S(24), S(68)};
    if (d->wrong) drawText(dc, L"Falsches Passwort. Bitte nochmal versuchen.", t, gF.body, C_ERROR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    else drawText(dc, L"„" + d->name + L"“ ist verschlüsselt.", t, gF.body, C_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    RECT f = {S(24), S(82), w - S(24), S(122)};
    fillRect(dc, f, C_SURFACE); frameRect(dc, f, GetFocus() == d->edit ? C_GOLD : C_LINE);
  } else {
    RECT h = {S(24), S(18), w - S(24), S(42)};
    drawText(dc, L"Dein Passwort", h, gF.title, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT t = {S(24), S(46), w - S(24), S(88)};
    drawText(dc, L"Ohne dieses Passwort lässt sich „" + d->name + L"“ nie wieder öffnen, auch nicht von dir. "
                 L"Es gibt keinen Weg zurück, also gut aufbewahren.", t, gF.body, C_DIM, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
    RECT f = {S(24), S(100), w - S(24), S(150)};
    fillRect(dc, f, C_SURFACE); frameRect(dc, f, C_GOLD);
    drawText(dc, d->pw, f, d->mono, C_GOLD_HI, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT c = {S(24), S(156), w - S(24), S(176)};
    drawText(dc, L"Groß/klein und Striche sind beim Eintippen egal.", c, gF.caption, C_FAINT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  }
}

static LRESULT CALLBACK EditSubclass(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
  if (m == WM_SETFOCUS || m == WM_KILLFOCUS) InvalidateRect(GetParent(h), NULL, FALSE);
  if (m == WM_NCDESTROY) RemoveWindowSubclass(h, EditSubclass, 1);
  return DefSubclassProc(h, m, wp, lp);
}

LRESULT CALLBACK SecretProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  SecretDlg* d = (SecretDlg*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  switch (msg) {
    case WM_CREATE: {
      d = (SecretDlg*)((CREATESTRUCTW*)lp)->lpCreateParams;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)d);
      styleWindow(hwnd);
      d->mono = CreateFontW(-S(d->kind == SK_SHOW ? 22 : 17), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
      if (d->kind == SK_ASK) {
        d->edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                  S(36), S(92), S(ASK_W - 72), S(22), hwnd, (HMENU)(INT_PTR)100, gHInst, NULL);
        SendMessageW(d->edit, WM_SETFONT, (WPARAM)d->mono, TRUE);
        SendMessageW(d->edit, EM_LIMITTEXT, 100, 0);
        SetWindowSubclass(d->edit, EditSubclass, 1, 0);
        HWND cancel = makeButton(hwnd, IDCANCEL, L"Abbrechen", BTN_GHOST);
        HWND ok = makeButton(hwnd, IDOK, L"Öffnen", BTN_PRIMARY);
        MoveWindow(cancel, S(ASK_W - 264), S(152), S(116), S(32), FALSE);
        MoveWindow(ok, S(ASK_W - 140), S(152), S(116), S(32), FALSE);
      } else {
        d->copy = makeButton(hwnd, ID_S_COPY, L"Kopieren", BTN_GHOST);
        HWND ok = makeButton(hwnd, IDOK, L"Fertig", BTN_PRIMARY);
        MoveWindow(d->copy, S(SHOW_W - 264), S(198), S(116), S(32), FALSE);
        MoveWindow(ok, S(SHOW_W - 140), S(198), S(116), S(32), FALSE);
      }
      return 0;
    }
    case WM_PAINT: if (d) paintSecret(hwnd, d); else { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps); } return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLOREDIT: SetTextColor((HDC)wp, C_TEXT); SetBkColor((HDC)wp, C_SURFACE); return (LRESULT)gBrSurface;
    case WM_DRAWITEM: drawButton((const DRAWITEMSTRUCT*)lp); return TRUE;
    case WM_COMMAND:
      if (!d) break;
      switch (LOWORD(wp)) {
        case IDOK:
          if (d->kind == SK_ASK) {
            int n = GetWindowTextLengthW(d->edit); std::wstring s(n + 1, 0);
            GetWindowTextW(d->edit, &s[0], n + 1); s.resize(n);
            if (normalizePassword(asciiOnly(s)).empty()) { MessageBeep(MB_OK); SetFocus(d->edit); return 0; }
            d->pw = s;
          }
          d->result = true; d->done = true; return 0;
        case IDCANCEL: d->result = d->kind == SK_SHOW; d->done = true; return 0;
        case ID_S_COPY:
          setClipboardText(hwnd, d->pw);
          SetWindowTextW(d->copy, L"Kopiert"); InvalidateRect(d->copy, NULL, FALSE);
          return 0;
      }
      return 0;
    case WM_CLOSE: if (d) { d->result = d->kind == SK_SHOW; d->done = true; } return 0;
    case WM_DESTROY:
      if (d && d->mono) { DeleteObject(d->mono); d->mono = NULL; }
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool runSecret(SecretDlg& d, HWND owner, int w, int h) {
  DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, ex = WS_EX_DLGMODALFRAME;
  RECT r = {0, 0, S(w), S(h)}; AdjustWindowRectEx(&r, style, FALSE, ex);
  int W = r.right - r.left, H = r.bottom - r.top;
  RECT ref;
  if (owner && IsWindowVisible(owner) && !IsIconic(owner)) GetWindowRect(owner, &ref);
  else SystemParametersInfoW(SPI_GETWORKAREA, 0, &ref, 0);
  int x = ref.left + (ref.right - ref.left - W) / 2, y = ref.top + (ref.bottom - ref.top - H) / 2;
  HWND hw = CreateWindowExW(ex, L"KlimaxCompressionSecret", L"Klimax Compression", style, x, y, W, H, owner, NULL, gHInst, &d);
  if (!hw) return false;
  if (owner) EnableWindow(owner, FALSE);
  ShowWindow(hw, SW_SHOW); SetForegroundWindow(hw);
  if (d.edit) SetFocus(d.edit);
  MSG m; bool quit = false; WPARAM quitCode = 0;
  while (!d.done) {
    BOOL g = GetMessageW(&m, NULL, 0, 0);
    if (g <= 0) { quit = true; quitCode = m.wParam; break; }
    if (!IsDialogMessageW(hw, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
  }
  if (owner) EnableWindow(owner, TRUE);
  DestroyWindow(hw);
  if (owner) SetForegroundWindow(owner);
  if (quit) PostQuitMessage((int)quitCode);
  return d.result;
}

bool askPassword(HWND owner, const std::wstring& archiveName, bool wrongBefore, std::wstring& out) {
  SecretDlg d; d.kind = SK_ASK; d.name = archiveName; d.wrong = wrongBefore;
  bool ok = runSecret(d, owner, ASK_W, ASK_H);
  if (ok) out = d.pw;
  return ok;
}

void showPassword(HWND owner, const std::wstring& archiveName, const std::wstring& pw) {
  SecretDlg d; d.kind = SK_SHOW; d.name = archiveName; d.pw = pw;
  runSecret(d, owner, SHOW_W, SHOW_H);
}

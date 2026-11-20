#pragma once
#include "winapi.h"
#include "archive.h"
#include "crypto.h"

const COLORREF C_BG         = RGB(21, 20, 18);
const COLORREF C_SURFACE    = RGB(30, 29, 26);
const COLORREF C_SURFACE_HI = RGB(40, 38, 34);
const COLORREF C_LINE       = RGB(54, 51, 46);
const COLORREF C_LINE_HI    = RGB(86, 81, 72);
const COLORREF C_GOLD       = RGB(199, 164, 98);
const COLORREF C_GOLD_HI    = RGB(214, 181, 117);
const COLORREF C_GOLD_DOWN  = RGB(172, 140, 82);
const COLORREF C_ON_GOLD    = RGB(28, 24, 17);
const COLORREF C_TEXT       = RGB(233, 228, 218);
const COLORREF C_DIM        = RGB(150, 144, 132);
const COLORREF C_FAINT      = RGB(104, 99, 90);
const COLORREF C_ERROR      = RGB(201, 98, 84);
const COLORREF C_WARN       = RGB(214, 158, 72);

struct Fonts { HFONT brand, crumb, title, body, bodyStrong, caption, label, button, icon, iconSmall; };

extern HINSTANCE gHInst;
extern double gScale;
extern Fonts gF;
extern HBRUSH gBrBg, gBrSurface;
extern HICON gIconHeader;

inline int S(int v) { return (int)(v * gScale + 0.5); }
std::wstring W(const std::string& utf8);

void uiInit(HINSTANCE h);
void styleWindow(HWND h);
void windowOpened();
void windowClosed();
int  messageLoop();

struct BufferedPaint {
  PAINTSTRUCT ps; HWND hwnd; HDC win, dc; HBITMAP bmp, old; RECT rc;
  explicit BufferedPaint(HWND h);
  ~BufferedPaint();
};
void fillRect(HDC dc, const RECT& r, COLORREF c);
void frameRect(HDC dc, const RECT& r, COLORREF c);
void hLine(HDC dc, int x1, int x2, int y, COLORREF c);
void drawText(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT flags);
int  textWidth(HDC dc, const std::wstring& s, HFONT f);
void drawLabel(HDC dc, const wchar_t* s, RECT r, COLORREF c, UINT flags);
void drawProgressBar(HDC dc, const RECT& r, int permille);

enum BtnStyle { BTN_PRIMARY = 0, BTN_GHOST = 1, BTN_ICON = 2 };
HWND makeButton(HWND parent, int id, const wchar_t* text, BtnStyle style);
void setButtonStyle(HWND btn, BtnStyle style);
void drawButton(const DRAWITEMSTRUCT* d);

enum DlgKind { DLG_INFO, DLG_WARN, DLG_ERROR, DLG_ASK };
bool ckDialog(HWND owner, DlgKind kind, const std::wstring& heading, const std::wstring& text);
bool pickFolder(HWND owner, const wchar_t* title, fs::path& out);
std::vector<std::wstring> pickFiles(HWND owner);
void revealInExplorer(const fs::path& p);
void setClipboardText(HWND owner, const std::wstring& text);

bool askPassword(HWND owner, const std::wstring& archiveName, bool wrongBefore, std::wstring& out);
void showPassword(HWND owner, const std::wstring& archiveName, const std::wstring& pw);
std::string asciiOnly(const std::wstring& s);
fs::path secretTempFile();

typedef std::vector<std::pair<int, std::wstring>> JobList;
void registerWindowClasses();
bool forwardToRunningInstance(const JobList& jobs);
void createMainWindow(int nShow, const JobList& jobs);
bool openViewer(const fs::path& archive, int nShow);

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK ViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK SecretProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

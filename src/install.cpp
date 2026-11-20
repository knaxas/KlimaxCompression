#include "install.h"
#include "winapi.h"

static const wchar_t* PROGID = L"KlimaxCompression.Archive";
static const wchar_t* UNINST_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\KlimaxCompression";
static const wchar_t* VERB = L"KlimaxCompression";
static const wchar_t* CLASSES = L"Software\\Classes\\";

static const wchar_t* OLD_PROGID = L"CompressionKing.Archive";
static const wchar_t* OLD_UNINST_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\CompressionKing";
static const wchar_t* OLD_VERB = L"CompressionKing";
static const wchar_t* OLD_SETTINGS = L"Software\\Compression King";

static fs::path knownFolder(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr; fs::path r;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, NULL, &p)) && p) r = p;
  if (p) CoTaskMemFree(p);
  return r;
}
static fs::path selfExe() { std::wstring b(32768, 0); DWORD n = GetModuleFileNameW(NULL, &b[0], (DWORD)b.size()); b.resize(n); return fs::path(b); }
static fs::path installedExe() { return installDir() / L"KlimaxCompression.exe"; }
static fs::path startMenuLink() { return knownFolder(FOLDERID_Programs) / L"Klimax Compression.lnk"; }

fs::path installDir() { return knownFolder(FOLDERID_LocalAppData) / L"Programs" / L"Klimax Compression"; }

static bool regKeyExists(const std::wstring& sub) {
  HKEY k; if (RegOpenKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, KEY_READ, &k) == ERROR_SUCCESS) { RegCloseKey(k); return true; } return false;
}
bool isInstalled() { std::error_code ec; return regKeyExists(UNINST_KEY) && fs::exists(installedExe(), ec); }
bool runningInstalled() { std::error_code ec; return fs::equivalent(selfExe(), installedExe(), ec); }
bool legacyInstalled() { return regKeyExists(OLD_UNINST_KEY); }

static int verNum(const std::wstring& v) { int a = 0, b = 0; swscanf(v.c_str(), L"%d.%d", &a, &b); return a * 1000 + b; }
bool installedIsOlder() {
  wchar_t buf[64] = {}; DWORD n = sizeof(buf);
  if (RegGetValueW(HKEY_CURRENT_USER, UNINST_KEY, L"DisplayVersion", RRF_RT_REG_SZ, NULL, buf, &n) != ERROR_SUCCESS) return false;
  return verNum(buf) < verNum(L"" CK_APP_VERSION);
}

static void regStr(const std::wstring& sub, const wchar_t* name, const std::wstring& val) {
  HKEY k;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) throw CKError("Registry-Fehler");
  RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(k);
}
static void regDword(const std::wstring& sub, const wchar_t* name, DWORD v) {
  HKEY k;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) throw CKError("Registry-Fehler");
  RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
  RegCloseKey(k);
}
static void makeShortcut(const fs::path& lnk, const fs::path& target, const std::wstring& desc) {
  IShellLinkW* sl = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl)) || !sl) return;
  sl->SetPath(target.c_str()); sl->SetDescription(desc.c_str());
  sl->SetIconLocation(target.c_str(), 0); sl->SetWorkingDirectory(target.parent_path().c_str());
  IPersistFile* pf = nullptr;
  if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf)) && pf) { pf->Save(lnk.c_str(), TRUE); pf->Release(); }
  sl->Release();
}

static void removeLegacy() {
  std::error_code ec;
  const std::wstring C = CLASSES;
  DWORD mode = 0, sz = sizeof(mode);
  if (RegGetValueW(HKEY_CURRENT_USER, OLD_SETTINGS, L"Modus", RRF_RT_REG_DWORD, NULL, &mode, &sz) == ERROR_SUCCESS &&
      !regKeyExists(SETTINGS_KEY))
    regDword(SETTINGS_KEY, L"Modus", mode);
  RegDeleteTreeW(HKEY_CURRENT_USER, OLD_SETTINGS);
  RegDeleteTreeW(HKEY_CURRENT_USER, (C + OLD_PROGID).c_str());
  RegDeleteTreeW(HKEY_CURRENT_USER, (C + L"*\\shell\\" + OLD_VERB).c_str());
  RegDeleteTreeW(HKEY_CURRENT_USER, (C + L"Directory\\shell\\" + OLD_VERB).c_str());
  RegDeleteTreeW(HKEY_CURRENT_USER, OLD_UNINST_KEY);
  fs::remove(knownFolder(FOLDERID_Programs) / L"Compression King.lnk", ec);
  fs::remove_all(knownFolder(FOLDERID_LocalAppData) / L"Programs" / L"Compression King", ec);
}

void doInstall() {
  std::error_code ec;
  fs::path dir = installDir(), exe = installedExe();
  fs::create_directories(dir, ec);
  if (!runningInstalled())
    if (!CopyFileW(selfExe().c_str(), exe.c_str(), FALSE))
      throw CKError("Konnte das Programm nicht kopieren. Laeuft die installierte Version gerade? Dann bitte zuerst schliessen.");
  removeLegacy();
  std::wstring e = exe.wstring(), q = L"\"" + e + L"\"";
  const std::wstring C = CLASSES, P = C + PROGID;
  for (const wchar_t* ext : {L".kc", L".ck"}) {
    regStr(C + ext, NULL, PROGID);
    regStr(C + ext + L"\\OpenWithProgids", PROGID, L"");
  }
  regStr(P, NULL, L"Klimax Compression Archiv");
  regStr(P, L"FriendlyTypeName", L"Klimax Compression Archiv");
  regStr(P + L"\\DefaultIcon", NULL, e + L",-2");
  regStr(P + L"\\shell", NULL, L"open");
  regStr(P + L"\\shell\\open", NULL, L"Öffnen");
  regStr(P + L"\\shell\\open\\command", NULL, q + L" \"%1\"");
  regStr(P + L"\\shell\\extract", L"MUIVerb", L"Hier entpacken");
  regStr(P + L"\\shell\\extract", L"Icon", e);
  regStr(P + L"\\shell\\extract\\command", NULL, q + L" --extract \"%1\"");
  for (const wchar_t* base : {L"*", L"Directory"}) {
    std::wstring k = C + base + L"\\shell\\" + VERB;
    regStr(k, L"MUIVerb", L"Mit Klimax Compression komprimieren");
    regStr(k, L"Icon", e);
    regStr(k + L"\\command", NULL, q + L" --compress \"%1\"");
  }
  regStr(UNINST_KEY, L"DisplayName", L"Klimax Compression");
  regStr(UNINST_KEY, L"DisplayIcon", e);
  regStr(UNINST_KEY, L"DisplayVersion", L"" CK_APP_VERSION);
  regStr(UNINST_KEY, L"Publisher", L"Klimax Compression");
  regStr(UNINST_KEY, L"InstallLocation", dir.wstring());
  regStr(UNINST_KEY, L"UninstallString", q + L" --uninstall");
  regDword(UNINST_KEY, L"NoModify", 1);
  regDword(UNINST_KEY, L"NoRepair", 1);
  regDword(UNINST_KEY, L"EstimatedSize", (DWORD)(fs::file_size(exe, ec) / 1024));
  makeShortcut(startMenuLink(), exe, L"Dateien und Ordner komprimieren");
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
}

void doUninstall() {
  std::error_code ec;
  const std::wstring C = CLASSES;
  removeLegacy();
  RegDeleteTreeW(HKEY_CURRENT_USER, (C + PROGID).c_str());
  RegDeleteTreeW(HKEY_CURRENT_USER, (C + L".kc").c_str());
  RegDeleteTreeW(HKEY_CURRENT_USER, (C + L".ck").c_str());
  RegDeleteTreeW(HKEY_CURRENT_USER, (C + L"*\\shell\\" + VERB).c_str());
  RegDeleteTreeW(HKEY_CURRENT_USER, (C + L"Directory\\shell\\" + VERB).c_str());
  RegDeleteTreeW(HKEY_CURRENT_USER, UNINST_KEY);
  RegDeleteTreeW(HKEY_CURRENT_USER, SETTINGS_KEY);
  fs::remove(startMenuLink(), ec);
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
  fs::path dir = installDir();
  if (runningInstalled()) {
    wchar_t tmp[MAX_PATH + 1] = {0}; GetTempPathW(MAX_PATH, tmp);
    std::wstring cmd = L"cmd.exe /c ping 127.0.0.1 -n 3 > nul & rmdir /s /q \"" + dir.wstring() + L"\"";
    STARTUPINFOW si = {}; si.cb = sizeof(si); PROCESS_INFORMATION pi = {};
    if (CreateProcessW(NULL, &cmd[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, tmp, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
  } else {
    fs::remove_all(dir, ec);
  }
}

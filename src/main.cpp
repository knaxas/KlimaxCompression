#include "archive.h"

#ifdef _WIN32
#include "ui.h"
#include "install.h"

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#endif

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow) {
  ckInit();
  SetProcessDPIAware();
  OleInitialize(NULL);
  INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES}; InitCommonControlsEx(&icc);
  uiInit(hInst);
  registerWindowClasses();

  std::vector<std::wstring> args;
  int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv) { for (int i = 1; i < argc; i++) args.push_back(argv[i]); LocalFree(argv); }

  if (!args.empty() && args[0] == L"--uninstall") {
    if (ckDialog(NULL, DLG_ASK, L"Klimax Compression deinstallieren?", L"Deine Archive bleiben erhalten.")) {
      try { doUninstall(); ckDialog(NULL, DLG_INFO, L"Klimax Compression wurde deinstalliert", L""); }
      catch (const std::exception& e) { ckDialog(NULL, DLG_ERROR, L"Deinstallation fehlgeschlagen", W(e.what())); }
    }
    OleUninitialize();
    return 0;
  }

  if ((isInstalled() && !runningInstalled() && installedIsOlder()) || legacyInstalled()) {
    try { doInstall(); } catch (...) {}
  }

  std::error_code ec;
  if (args.size() == 1 && isArchiveExt(lowerExt(fs::path(args[0]))) && fs::is_regular_file(fs::path(args[0]), ec)) {
    int rc = openViewer(fs::path(args[0]), nShow) ? messageLoop() : 1;
    OleUninitialize();
    return rc;
  }

  JobList jobs; int mode = 0;
  for (auto& a : args) {
    if (a == L"--compress") mode = 1;
    else if (a == L"--extract") mode = 2;
    else jobs.push_back({mode, a});
  }

  HANDLE mtx = CreateMutexW(NULL, FALSE, L"Local\\KlimaxCompressionMainWindow");
  if (GetLastError() == ERROR_ALREADY_EXISTS && forwardToRunningInstance(jobs)) {
    if (mtx) CloseHandle(mtx);
    OleUninitialize();
    return 0;
  }

  createMainWindow(nShow, jobs);
  int rc = messageLoop();
  if (mtx) CloseHandle(mtx);
  OleUninitialize();
  return rc;
}

#else
#include "model.h"

int main(int argc, char** argv) {
  ckInit();
  if (getenv("CK_LR")) G_LR = atoi(getenv("CK_LR"));
  if (getenv("CK_LIM")) G_LIM = atoi(getenv("CK_LIM"));
  if (getenv("CK_LR2")) G_LR2 = atoi(getenv("CK_LR2"));
  if (getenv("CK_APMR")) G_APMR = atoi(getenv("CK_APMR"));
  if (getenv("CK_LRB")) G_LRB = atoi(getenv("CK_LRB"));
  if (getenv("CK_X1")) G_X1 = atoi(getenv("CK_X1"));
  if (getenv("CK_FM")) G_FM = atoi(getenv("CK_FM"));
  if (getenv("CK_SKIP")) G_SKIP = atoi(getenv("CK_SKIP"));
  if (argc < 3) {
    fprintf(stderr, "Klimax Compression\n  kc c <datei|ordner> [ziel.kc] [threads] [modus 0=schnell 1=ausgewogen 2=maximal]\n  kc d <archiv.kc> [zielordner]\n");
    return 1;
  }
  std::string mode = argv[1];
  Progress prog;
  auto t0 = std::chrono::steady_clock::now();
  prog.cb = [&](const std::string& ph, U64 d, U64 t) { fprintf(stderr, "\r%s %5.1f%%   ", ph.c_str(), t ? 100.0 * d / t : 100.0); };
  try {
    if (mode == "c") {
      fs::path in = argv[2];
      fs::path pn = fs::absolute(in).lexically_normal(); if (!pn.has_filename()) pn = pn.parent_path();
      fs::path out = argc > 3 ? fs::path(argv[3]) : uniquePath(pn.parent_path() / (pn.filename().string() + ".kc"));
      int threads = argc > 4 ? atoi(argv[4]) : 0;
      CkMode m = argc > 5 ? (CkMode)atoi(argv[5]) : MODE_MAX;
      CompressResult r = compressPath(in, out, prog, threads, m);
      double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      fprintf(stderr, "\r%s -> %s  %llu -> %llu  (%.2f%%, %.3f bpc)  %.1fs  %.2f MB/s  dups=%llu stored=%llu seg=%d\n", in.c_str(), out.c_str(),
              (unsigned long long)r.inBytes, (unsigned long long)r.outBytes, r.inBytes ? 100.0 * r.outBytes / r.inBytes : 0.0,
              r.inBytes ? 8.0 * r.outBytes / r.inBytes : 0.0, el, r.inBytes / 1048576.0 / el, (unsigned long long)r.dups, (unsigned long long)r.stored, r.segments);
    } else if (mode == "d") {
      fs::path a = argv[2]; fs::path od = argc > 3 ? fs::path(argv[3]) : fs::absolute(a).parent_path();
      ExtractResult r = extractArchive(a, od, prog);
      double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      fprintf(stderr, "\rEntpackt nach %s: %llu Dateien, %llu Bytes, %.1fs, CRC-Fehler: %d\n", r.root.c_str(), (unsigned long long)r.files,
              (unsigned long long)r.bytes, el, r.crcErrors);
      return r.crcErrors ? 2 : 0;
    } else if (mode == "l") {
      ArchiveInfo ai = readArchiveInfo(fs::path(argv[2]));
      for (auto& e : ai.ents) printf("%s %12llu  %s\n", e.dir ? "D" : (e.method == M_DUP ? "=" : e.method == M_STORED ? "S" : "F"), (unsigned long long)e.size, e.path.c_str());
    } else if (mode == "x" && argc >= 5) {
      ArchiveInfo ai = readArchiveInfo(fs::path(argv[2]));
      std::vector<char> want(ai.ents.size(), 0);
      for (int k = 5; k < argc; k++) for (size_t i = 0; i < ai.ents.size(); i++) {
        const std::string& p = ai.ents[i].path; std::string w = argv[k];
        if (p == w || p.compare(0, w.size() + 1, w + "/") == 0) want[i] = 1;
      }
      ExtractResult r = extractArchive(argv[2], argv[3], prog, &want, argv[4]);
      fprintf(stderr, "\rTeil-Entpackt: %llu Dateien, CRC-Fehler %d, erste: %s\n", (unsigned long long)r.files, r.crcErrors, r.firstFile.c_str());
      return r.crcErrors ? 2 : 0;
    } else { fprintf(stderr, "Unbekannter Modus\n"); return 1; }
  } catch (const std::exception& e) { fprintf(stderr, "\nFehler: %s\n", e.what()); return 1; }
  return 0;
}
#endif

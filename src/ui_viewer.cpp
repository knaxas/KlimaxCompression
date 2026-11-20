#include "ui.h"
#include <cwctype>

#define WM_V_PROGRESS (WM_APP + 10)
#define WM_V_STATUS   (WM_APP + 11)
#define WM_V_DONE     (WM_APP + 12)
enum { ID_V_UP = 2001, ID_V_ALL, ID_V_SEL, ID_V_LIST };

struct Viewer {
  HWND hwnd = NULL, list = NULL, bUp = NULL, bAll = NULL, bSel = NULL;
  fs::path archive; ArchiveInfo ai; std::string cur;
  fs::path data; bool encrypted = false;
  std::vector<int> rows;
  std::thread worker; std::atomic<bool> cancel{false}; bool busy = false;
  fs::path tempDir; int openCount = 0;
  std::wstring summary, status; int progress = 0;
  std::vector<U64> dirSize; std::vector<U32> dirFiles, dirDirs;
};
struct VDone { int kind = 0; bool ok = false; fs::path path; std::wstring msg; };
enum { CM_OPEN = 3001, CM_OPENWITH, CM_EXTRACT_TO, CM_EXTRACT_HERE, CM_EXTRACT_DESKTOP, CM_COPY_NAME, CM_COPY_PATH,
       CM_PROPERTIES, CM_EXTRACT_ALL, CM_EXTRACT_ALL_TO, CM_SELECT_ALL, CM_UP };

static const int TOOLBAR_H = 64, FOOTER_H = 44;
static RECT rToolbar(Viewer* v) { RECT rc; GetClientRect(v->hwnd, &rc); return {0, 0, rc.right, S(TOOLBAR_H)}; }
static RECT rFooter(Viewer* v)  { RECT rc; GetClientRect(v->hwnd, &rc); return {0, rc.bottom - S(FOOTER_H), rc.right, rc.bottom}; }

static std::unordered_map<std::wstring, std::pair<int, std::wstring>> gIconCache;
static std::pair<int, std::wstring> shellIcon(const std::wstring& name, bool dir) {
  std::wstring key = L"<dir>";
  if (!dir) {
    size_t d = name.find_last_of(L'.');
    key = d == std::wstring::npos ? L"" : name.substr(d);
    for (auto& ch : key) ch = (wchar_t)towlower(ch);
  }
  auto it = gIconCache.find(key);
  if (it != gIconCache.end()) return it->second;
  SHFILEINFOW sfi = {};
  std::wstring probe = dir ? L"Ordner" : L"datei" + key;
  SHGetFileInfoW(probe.c_str(), dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                 SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME);
  std::pair<int, std::wstring> r(sfi.iIcon, sfi.szTypeName);
  if (r.second.empty()) r.second = dir ? L"Dateiordner" : L"Datei";
  gIconCache[key] = r;
  return r;
}
static std::wstring fmtDate(U64 ft) {
  if (!ft) return L"";
  FILETIME f; f.dwLowDateTime = (DWORD)ft; f.dwHighDateTime = (DWORD)(ft >> 32);
  SYSTEMTIME u, l;
  if (!FileTimeToSystemTime(&f, &u) || !SystemTimeToTzSpecificLocalTime(NULL, &u, &l)) return L"";
  wchar_t b[32]; swprintf(b, 32, L"%02d.%02d.%04d  %02d:%02d", l.wDay, l.wMonth, l.wYear, l.wHour, l.wMinute);
  return b;
}
static void lvSetText(HWND lv, int item, int sub, const std::wstring& t) {
  LVITEMW it = {}; it.iSubItem = sub; it.pszText = const_cast<LPWSTR>(t.c_str());
  SendMessageW(lv, LVM_SETITEMTEXTW, (WPARAM)item, (LPARAM)&it);
}
static void invalidateFooter(Viewer* v) { RECT f = rFooter(v); InvalidateRect(v->hwnd, &f, FALSE); }

static void viewerRefresh(Viewer* v) {
  const auto& ents = v->ai.ents;
  std::vector<int> kids;
  for (int i = 0; i < (int)ents.size(); i++) {
    const std::string& p = ents[i].path;
    if (p.size() <= v->cur.size() || p.compare(0, v->cur.size(), v->cur) != 0) continue;
    if (p.find('/', v->cur.size()) != std::string::npos) continue;
    kids.push_back(i);
  }
  std::vector<std::wstring> names(ents.size());
  for (int i : kids) names[i] = W(ents[i].path.substr(v->cur.size()));
  std::sort(kids.begin(), kids.end(), [&](int a, int b) {
    if (ents[a].dir != ents[b].dir) return ents[a].dir;
    return lstrcmpiW(names[a].c_str(), names[b].c_str()) < 0;
  });
  v->rows.clear();
  if (!v->cur.empty()) v->rows.push_back(-1);
  v->rows.insert(v->rows.end(), kids.begin(), kids.end());

  SendMessageW(v->list, WM_SETREDRAW, FALSE, 0);
  SendMessageW(v->list, LVM_DELETEALLITEMS, 0, 0);
  for (int k = 0; k < (int)v->rows.size(); k++) {
    int i = v->rows[k];
    std::wstring name = i < 0 ? L".." : names[i];
    bool dir = i < 0 || ents[i].dir;
    auto ic = shellIcon(name, dir);
    LVITEMW it = {}; it.mask = LVIF_TEXT | LVIF_IMAGE; it.iItem = k; it.pszText = &name[0]; it.iImage = ic.first;
    SendMessageW(v->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
    if (i >= 0) {
      lvSetText(v->list, k, 1, W(fmtSize(dir ? v->dirSize[i] : ents[i].size)));
      lvSetText(v->list, k, 2, fmtDate(ents[i].mtime));
      lvSetText(v->list, k, 3, ic.second);
    }
  }
  SendMessageW(v->list, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(v->list, NULL, TRUE);
  RECT t = rToolbar(v); InvalidateRect(v->hwnd, &t, FALSE);
  EnableWindow(v->bUp, !v->cur.empty() && !v->busy);
}

static void viewerUp(Viewer* v) {
  if (v->cur.empty()) return;
  std::string c = v->cur.substr(0, v->cur.size() - 1);
  size_t sl = c.find_last_of('/');
  v->cur = sl == std::string::npos ? "" : c.substr(0, sl + 1);
  viewerRefresh(v);
}

static void viewerSetBusy(Viewer* v, bool busy) {
  v->busy = busy;
  EnableWindow(v->bAll, !busy); EnableWindow(v->bUp, !busy && !v->cur.empty());
  SetWindowTextW(v->bSel, busy ? L"Abbrechen" : L"Auswahl entpacken \u2026");
  InvalidateRect(v->bSel, NULL, FALSE);
  v->progress = 0; v->status = busy ? L"Wird vorbereitet \u2026" : L"";
  invalidateFooter(v);
}

static void viewerStart(Viewer* v, int kind, std::vector<char> want, const std::string& prefix, const fs::path& outDir) {
  if (v->busy) return;
  if (v->worker.joinable()) v->worker.join();
  v->cancel = false;
  viewerSetBusy(v, true);
  v->worker = std::thread([v, kind, want, prefix, outDir] {
    VDone* d = new VDone; d->kind = kind;
    Progress prog; prog.cancel = &v->cancel;
    HWND h = v->hwnd;
    prog.cb = [h](const std::string& ph, U64 done, U64 tot) {
      PostMessageW(h, WM_V_PROGRESS, tot ? (WPARAM)(done * 1000 / tot) : 0, 0);
      PostMessageW(h, WM_V_STATUS, 0, (LPARAM)new std::wstring(W(ph) + L"  " + W(fmtSize(done)) + L" von " + W(fmtSize(tot))));
    };
    try {
      ExtractResult r = extractArchive(v->data, outDir, prog, want.empty() ? nullptr : &want, prefix);
      d->ok = true;
      d->path = (kind == 0 || kind == 3) ? r.firstFile : r.root;
      if (r.crcErrors) d->msg = std::to_wstring(r.crcErrors) + L" Datei(en) haben einen CRC-Fehler \u2013 das Archiv ist besch\u00E4digt.";
    } catch (const CKCancelled&) {
    } catch (const std::exception& e) { d->msg = W(e.what()); }
    PostMessageW(h, WM_V_DONE, 0, (LPARAM)d);
  });
}

static std::vector<int> viewerSelection(Viewer* v) {
  std::vector<int> sel; int k = -1;
  while ((k = (int)SendMessageW(v->list, LVM_GETNEXTITEM, (WPARAM)k, LVNI_SELECTED)) >= 0)
    if (k < (int)v->rows.size() && v->rows[k] >= 0) sel.push_back(v->rows[k]);
  return sel;
}
static std::vector<char> viewerWant(Viewer* v, const std::vector<int>& sel) {
  const auto& ents = v->ai.ents;
  std::vector<char> want(ents.size(), 0);
  for (int i : sel) {
    want[i] = 1;
    if (ents[i].dir) {
      std::string pre = ents[i].path + "/";
      for (size_t j = 0; j < ents.size(); j++) if (ents[j].path.compare(0, pre.size(), pre) == 0) want[j] = 1;
    }
  }
  return want;
}

static void viewerActivate(Viewer* v, int row) {
  if (v->busy || row < 0 || row >= (int)v->rows.size()) return;
  int i = v->rows[row];
  if (i < 0) { viewerUp(v); return; }
  const Entry& e = v->ai.ents[i];
  if (e.dir) { v->cur = e.path + "/"; viewerRefresh(v); return; }
  std::error_code ec;
  fs::path out = v->tempDir / (L"o" + std::to_wstring(++v->openCount));
  fs::create_directories(out, ec);
  viewerStart(v, 0, viewerWant(v, {i}), v->cur, out);
}

static std::wstring winPath(const std::string& p) { std::wstring w = W(p); for (auto& c : w) if (c == L'/') c = L'\\'; return w; }
static std::wstring baseName(const std::string& p) { size_t s = p.find_last_of('/'); return W(s == std::string::npos ? p : p.substr(s + 1)); }

static std::wstring exactBytes(U64 b) {
  std::wstring s = std::to_wstring(b);
  for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, L".");
  return s + L" Bytes";
}

static fs::path desktopDir() {
  PWSTR p = nullptr; fs::path r;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, NULL, &p)) && p) r = p;
  if (p) CoTaskMemFree(p);
  return r;
}

static void showProperties(Viewer* v, const std::vector<int>& sel) {
  const auto& ents = v->ai.ents;
  std::wstring head, body;
  if (sel.empty()) {
    U64 files = 0, dirs = 0, stored = 0, dups = 0;
    for (auto& e : ents) { if (e.dir) dirs++; else { files++; if (e.method == M_STORED) stored++; if (e.method == M_DUP) dups++; } }
    head = v->archive.filename().wstring();
    body = L"Ort:  " + v->archive.parent_path().wstring() +
           L"\nInhalt:  " + std::to_wstring(files) + L" Dateien, " + std::to_wstring(dirs) + L" Ordner" +
           L"\nOriginalgröße:  " + W(fmtSize(v->ai.totalSize)) + L"  (" + exactBytes(v->ai.totalSize) + L")" +
           L"\nArchivgröße:  " + W(fmtSize(v->ai.archiveSize)) + L"  (" + exactBytes(v->ai.archiveSize) + L")" +
           L"\nVerhältnis:  " + W(fmtPercent(v->ai.archiveSize, v->ai.totalSize)) + L" der Originalgröße" +
           L"\nVerfahren:  " + W(codecName(v->ai)) + L", " + std::to_wstring(v->ai.nseg) + L" Segment(e)";
    if (v->encrypted) body += L"\nVerschlüsselung:  AES-256-GCM (geheim)";
    if (dups) body += L"\nDuplikate:  " + std::to_wstring(dups) + L" (nur einmal gespeichert)";
    if (stored) body += L"\nBereits komprimiert:  " + std::to_wstring(stored) + L" (unverändert übernommen)";
  } else if (sel.size() == 1) {
    const Entry& e = ents[sel[0]];
    head = baseName(e.path);
    body = L"Pfad im Archiv:  " + winPath(e.path) + L"\nTyp:  " + shellIcon(head, e.dir).second;
    if (e.dir) {
      body += L"\nInhalt:  " + std::to_wstring(v->dirFiles[sel[0]]) + L" Dateien, " + std::to_wstring(v->dirDirs[sel[0]]) + L" Ordner" +
              L"\nGröße:  " + W(fmtSize(v->dirSize[sel[0]])) + L"  (" + exactBytes(v->dirSize[sel[0]]) + L")";
    } else {
      body += L"\nGröße:  " + W(fmtSize(e.size)) + L"  (" + exactBytes(e.size) + L")";
      std::wstring how = e.method == M_STORED ? L"unverändert gespeichert (bereits komprimiert)"
                       : e.method == M_DUP    ? L"Duplikat von  " + winPath(ents[e.dupOf].path)
                       : e.filter == F_E8E9   ? L"komprimiert (Context Mixing, x86-Filter)" : L"komprimiert (Context Mixing)";
      body += L"\nSpeicherung:  " + how;
      wchar_t crc[16]; swprintf(crc, 16, L"%08X", e.method == M_DUP ? ents[e.dupOf].crc : e.crc);
      body += L"\nPrüfsumme (CRC32):  " + std::wstring(crc);
    }
    body += L"\nGeändert am:  " + fmtDate(e.mtime);
  } else {
    U64 files = 0, dirs = 0, size = 0;
    for (int i : sel) {
      if (ents[i].dir) { dirs++; files += v->dirFiles[i]; size += v->dirSize[i]; } else { files++; size += ents[i].size; }
    }
    head = std::to_wstring(sel.size()) + L" Elemente ausgewählt";
    body = L"Inhalt:  " + std::to_wstring(files) + L" Dateien" + (dirs ? L" in " + std::to_wstring(dirs) + L" ausgewählten Ordnern" : L"") +
           L"\nGröße:  " + W(fmtSize(size)) + L"  (" + exactBytes(size) + L")";
  }
  ckDialog(v->hwnd, DLG_INFO, head, body);
}

static void extractSelection(Viewer* v, const std::vector<int>& sel, const fs::path& outDir) {
  if (sel.empty() || outDir.empty()) return;
  viewerStart(v, 2, viewerWant(v, sel), v->cur, outDir);
}

static void viewerCommand(Viewer* v, int cmd) {
  std::vector<int> sel = viewerSelection(v);
  switch (cmd) {
    case CM_OPEN:
      if (sel.size() == 1) {
        for (int k = 0; k < (int)v->rows.size(); k++) if (v->rows[k] == sel[0]) { viewerActivate(v, k); break; }
      }
      break;
    case CM_OPENWITH:
      if (sel.size() == 1 && !v->ai.ents[sel[0]].dir && !v->busy) {
        std::error_code ec;
        fs::path out = v->tempDir / (L"o" + std::to_wstring(++v->openCount));
        fs::create_directories(out, ec);
        viewerStart(v, 3, viewerWant(v, sel), v->cur, out);
      }
      break;
    case CM_EXTRACT_TO: {
      fs::path out;
      if (!sel.empty() && pickFolder(v->hwnd, L"Wohin soll entpackt werden?", out)) extractSelection(v, sel, out);
      break;
    }
    case CM_EXTRACT_HERE:    extractSelection(v, sel, v->archive.parent_path()); break;
    case CM_EXTRACT_DESKTOP: extractSelection(v, sel, desktopDir()); break;
    case CM_EXTRACT_ALL:     viewerStart(v, 1, {}, "", v->archive.parent_path()); break;
    case CM_EXTRACT_ALL_TO: {
      fs::path out;
      if (pickFolder(v->hwnd, L"Wohin soll das Archiv entpackt werden?", out)) viewerStart(v, 1, {}, "", out);
      break;
    }
    case CM_COPY_NAME: case CM_COPY_PATH: {
      std::wstring t;
      for (int i : sel) { if (!t.empty()) t += L"\r\n"; t += cmd == CM_COPY_NAME ? baseName(v->ai.ents[i].path) : winPath(v->ai.ents[i].path); }
      if (!t.empty()) setClipboardText(v->hwnd, t);
      break;
    }
    case CM_SELECT_ALL: {
      LVITEMW it = {}; it.stateMask = LVIS_SELECTED; it.state = LVIS_SELECTED;
      SendMessageW(v->list, LVM_SETITEMSTATE, (WPARAM)-1, (LPARAM)&it);
      if (!v->rows.empty() && v->rows[0] < 0) {
        it.state = 0; SendMessageW(v->list, LVM_SETITEMSTATE, 0, (LPARAM)&it);
      }
      break;
    }
    case CM_UP: if (!v->busy) viewerUp(v); break;
    case CM_PROPERTIES: showProperties(v, sel); break;
  }
}

static void viewerContextMenu(Viewer* v, LPARAM lp) {
  POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
  if (lp == -1) {
    int k = (int)SendMessageW(v->list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_FOCUSED);
    RECT r = {}; r.left = LVIR_LABEL;
    if (k < 0 || !SendMessageW(v->list, LVM_GETITEMRECT, k, (LPARAM)&r)) { r.left = S(20); r.bottom = S(20); }
    pt = {r.left, r.bottom}; ClientToScreen(v->list, &pt);
  }
  std::vector<int> sel = viewerSelection(v);
  UINT busy = v->busy ? MF_GRAYED : 0;
  HMENU m = CreatePopupMenu();
  if (!sel.empty()) {
    if (sel.size() == 1) {
      bool dir = v->ai.ents[sel[0]].dir;
      AppendMenuW(m, MF_STRING | busy, CM_OPEN, L"Öffnen\tEnter");
      if (!dir) AppendMenuW(m, MF_STRING | busy, CM_OPENWITH, L"Öffnen mit …");
      SetMenuDefaultItem(m, CM_OPEN, FALSE);
      AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    }
    AppendMenuW(m, MF_STRING | busy, CM_EXTRACT_TO, L"Entpacken nach …");
    AppendMenuW(m, MF_STRING | busy, CM_EXTRACT_HERE, L"Neben das Archiv entpacken");
    AppendMenuW(m, MF_STRING | busy, CM_EXTRACT_DESKTOP, L"Auf den Desktop entpacken");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CM_COPY_NAME, sel.size() == 1 ? L"Namen kopieren\tStrg+C" : L"Namen kopieren\tStrg+C");
    AppendMenuW(m, MF_STRING, CM_COPY_PATH, L"Pfad im Archiv kopieren");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CM_PROPERTIES, L"Eigenschaften\tAlt+Enter");
  } else {
    AppendMenuW(m, MF_STRING | busy, CM_EXTRACT_ALL, L"Alles entpacken");
    AppendMenuW(m, MF_STRING | busy, CM_EXTRACT_ALL_TO, L"Alles entpacken nach …");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CM_SELECT_ALL, L"Alles auswählen\tStrg+A");
    if (!v->cur.empty()) AppendMenuW(m, MF_STRING | busy, CM_UP, L"Eine Ebene höher\tRücktaste");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CM_PROPERTIES, L"Eigenschaften des Archivs");
  }
  int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, v->hwnd, NULL);
  DestroyMenu(m);
  if (cmd) viewerCommand(v, cmd);
}

static LRESULT CALLBACK ListSubclass(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
  if (m == WM_NOTIFY) {
    NMHDR* nh = (NMHDR*)lp;
    HWND hdr = (HWND)SendMessageW(h, LVM_GETHEADER, 0, 0);
    if (nh->hwndFrom == hdr && nh->code == NM_CUSTOMDRAW) {
      NMCUSTOMDRAW* cd = (NMCUSTOMDRAW*)lp;
      if (cd->dwDrawStage == CDDS_PREPAINT) {
        RECT rc; GetClientRect(hdr, &rc);
        fillRect(cd->hdc, rc, C_SURFACE);
        hLine(cd->hdc, rc.left, rc.right, rc.bottom - std::max(1, S(1)), C_LINE);
        return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
      }
      if (cd->dwDrawStage == CDDS_POSTPAINT) {
        int n = (int)SendMessageW(hdr, HDM_GETITEMCOUNT, 0, 0);
        RECT last = {}, rc; GetClientRect(hdr, &rc);
        if (n > 0) SendMessageW(hdr, HDM_GETITEMRECT, n - 1, (LPARAM)&last);
        RECT fill = {last.right, rc.top, rc.right, rc.bottom};
        if (fill.right > fill.left) { fillRect(cd->hdc, fill, C_SURFACE); hLine(cd->hdc, fill.left, fill.right, rc.bottom - std::max(1, S(1)), C_LINE); }
        return CDRF_DODEFAULT;
      }
      if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
        RECT r = cd->rc;
        fillRect(cd->hdc, r, (cd->uItemState & CDIS_SELECTED) ? C_SURFACE_HI : C_SURFACE);
        hLine(cd->hdc, r.left, r.right, r.bottom - std::max(1, S(1)), C_LINE);
        RECT div = {r.right - std::max(1, S(1)), r.top + S(6), r.right, r.bottom - S(6)}; fillRect(cd->hdc, div, C_LINE);
        wchar_t txt[64] = {0}; HDITEMW it = {}; it.mask = HDI_TEXT | HDI_FORMAT; it.pszText = txt; it.cchTextMax = 64;
        SendMessageW(hdr, HDM_GETITEMW, cd->dwItemSpec, (LPARAM)&it);
        RECT t = {r.left + S(10), r.top, r.right - S(10), r.bottom};
        UINT align = (it.fmt & HDF_RIGHT) ? DT_RIGHT : DT_LEFT;
        drawText(cd->hdc, txt, t, gF.caption, C_DIM, align | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return CDRF_SKIPDEFAULT;
      }
      return CDRF_DODEFAULT;
    }
  } else if (m == WM_SYSKEYDOWN && wp == VK_RETURN) {
    SendMessageW(GetParent(h), WM_COMMAND, CM_PROPERTIES, 0);
    return 0;
  } else if (m == WM_NCDESTROY) {
    RemoveWindowSubclass(h, ListSubclass, 1);
  }
  return DefSubclassProc(h, m, wp, lp);
}

static LRESULT drawListCustom(Viewer* v, NMLVCUSTOMDRAW* cd) {
  HWND lv = v->list; HDC dc = cd->nmcd.hdc;
  switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
    case CDDS_ITEMPREPAINT: {
      int item = (int)cd->nmcd.dwItemSpec;
      RECT cl; GetClientRect(lv, &cl);
      RECT row = {}; row.left = LVIR_BOUNDS; SendMessageW(lv, LVM_GETITEMRECT, item, (LPARAM)&row);
      row.left = 0; row.right = cl.right;
      bool sel = SendMessageW(lv, LVM_GETITEMSTATE, item, LVIS_SELECTED) != 0;
      fillRect(dc, row, sel ? C_SURFACE_HI : C_SURFACE);
      if (sel) { RECT bar = {0, row.top, std::max(2, S(2)), row.bottom}; fillRect(dc, bar, C_GOLD); }
      wchar_t txt[512]; LVITEMW it = {};
      it.mask = LVIF_IMAGE; it.iItem = item; SendMessageW(lv, LVM_GETITEMW, 0, (LPARAM)&it);
      HWND hdr = (HWND)SendMessageW(lv, LVM_GETHEADER, 0, 0);
      int cols = (int)SendMessageW(hdr, HDM_GETITEMCOUNT, 0, 0);
      for (int c = 0; c < cols; c++) {
        RECT r = {}; r.top = c; r.left = c == 0 ? LVIR_LABEL : LVIR_BOUNDS;
        SendMessageW(lv, LVM_GETSUBITEMRECT, item, (LPARAM)&r);
        if (c == 0) {
          RECT ic = {}; ic.top = 0; ic.left = LVIR_ICON; SendMessageW(lv, LVM_GETSUBITEMRECT, item, (LPARAM)&ic);
          HIMAGELIST il = (HIMAGELIST)SendMessageW(lv, LVM_GETIMAGELIST, LVSIL_SMALL, 0);
          int iw = 16, ih = 16; if (il) ImageList_GetIconSize(il, &iw, &ih);
          if (il) ImageList_Draw(il, it.iImage, dc, ic.left + S(6), (ic.top + ic.bottom - ih) / 2, ILD_TRANSPARENT);
          r.left = ic.left + S(6) + iw + S(8);
        }
        LVITEMW ti = {}; ti.iSubItem = c; ti.pszText = txt; ti.cchTextMax = 512; txt[0] = 0;
        SendMessageW(lv, LVM_GETITEMTEXTW, item, (LPARAM)&ti);
        LVCOLUMNW col = {}; col.mask = LVCF_FMT; SendMessageW(lv, LVM_GETCOLUMNW, c, (LPARAM)&col);
        RECT tr = {r.left + (c ? S(10) : 0), r.top, r.right - S(10), r.bottom};
        drawText(dc, txt, tr, gF.body, c == 0 ? C_TEXT : C_DIM,
                 ((col.fmt & LVCFMT_RIGHT) ? DT_RIGHT : DT_LEFT) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      }
      return CDRF_SKIPDEFAULT;
    }
    case CDDS_POSTPAINT: {
      RECT cl; GetClientRect(lv, &cl);
      int n = (int)SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0);
      int top = 0;
      HWND hdr = (HWND)SendMessageW(lv, LVM_GETHEADER, 0, 0);
      if (hdr) { RECT hr; GetWindowRect(hdr, &hr); top = hr.bottom - hr.top; }
      if (n > 0) {
        RECT last = {}; last.left = LVIR_BOUNDS;
        int lastVisible = (int)SendMessageW(lv, LVM_GETTOPINDEX, 0, 0) + (int)SendMessageW(lv, LVM_GETCOUNTPERPAGE, 0, 0);
        lastVisible = std::min(n - 1, lastVisible);
        SendMessageW(lv, LVM_GETITEMRECT, lastVisible, (LPARAM)&last);
        top = std::max(top, (int)last.bottom);
      }
      RECT fill = {0, top, cl.right, cl.bottom};
      if (fill.bottom > fill.top) fillRect(dc, fill, C_SURFACE);
      return CDRF_DODEFAULT;
    }
  }
  return CDRF_DODEFAULT;
}

static void viewerLayout(Viewer* v) {
  RECT rc; GetClientRect(v->hwnd, &rc);
  int w = rc.right, h = rc.bottom, line = std::max(1, S(1));
  int bAllX = w - S(16) - S(150), bSelX = bAllX - S(8) - S(170);
  MoveWindow(v->bUp, S(16), S(16), S(36), S(32), TRUE);
  MoveWindow(v->bSel, bSelX, S(16), S(170), S(32), TRUE);
  MoveWindow(v->bAll, bAllX, S(16), S(150), S(32), TRUE);
  int listH = std::max(S(50), h - S(TOOLBAR_H) - S(FOOTER_H) - 2 * line);
  MoveWindow(v->list, 0, S(TOOLBAR_H) + line, w, listH, TRUE);
  int rest = S(100) + S(150) + S(190) + GetSystemMetrics(SM_CXVSCROLL) + S(4);
  SendMessageW(v->list, LVM_SETCOLUMNWIDTH, 0, std::max(S(200), w - rest));
  InvalidateRect(v->hwnd, NULL, FALSE);
}

static void paintViewer(Viewer* v) {
  BufferedPaint bp(v->hwnd); HDC dc = bp.dc; RECT rc = bp.rc;
  fillRect(dc, rc, C_BG);

  RECT tb = rToolbar(v);
  RECT bs; GetWindowRect(v->bSel, &bs); MapWindowPoints(NULL, v->hwnd, (POINT*)&bs, 2);
  int x = S(66), right = bs.left - S(16);
  auto piece = [&](const std::wstring& s, HFONT f, COLORREF c) {
    if (x >= right) return;
    RECT r = {x, tb.top, right, tb.bottom};
    drawText(dc, s, r, f, c, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    x += textWidth(dc, s, f);
  };
  piece(v->archive.filename().wstring(), gF.crumb, v->cur.empty() ? C_TEXT : C_DIM);
  size_t st = 0;
  while (st < v->cur.size()) {
    size_t e = v->cur.find('/', st);
    bool last = e + 1 >= v->cur.size();
    piece(L"   \u203A   ", gF.body, C_FAINT);
    piece(W(v->cur.substr(st, e - st)), last ? gF.bodyStrong : gF.body, last ? C_TEXT : C_DIM);
    st = e + 1;
  }
  hLine(dc, 0, rc.right, S(TOOLBAR_H), C_LINE);

  RECT ft = rFooter(v);
  hLine(dc, 0, rc.right, ft.top - std::max(1, S(1)), C_LINE);
  RECT txt = {S(16), ft.top, rc.right - S(16), ft.bottom};
  if (v->busy) {
    RECT bar = {rc.right - S(16) - S(220), (ft.top + ft.bottom) / 2 - 1, rc.right - S(16), (ft.top + ft.bottom) / 2 - 1 + std::max(2, S(3))};
    drawProgressBar(dc, bar, v->progress);
    txt.right = bar.left - S(16);
    drawText(dc, v->status, txt, gF.body, C_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  } else {
    const std::wstring hint = L"Doppelklick \u00F6ffnet   \u00B7   R\u00FCcktaste: eine Ebene h\u00F6her";
    int hw = textWidth(dc, hint, gF.caption);
    if (hw < (txt.right - txt.left) / 2) {
      drawText(dc, hint, txt, gF.caption, C_FAINT, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
      txt.right -= hw + S(24);
    }
    drawText(dc, v->summary, txt, gF.body, C_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }
}

LRESULT CALLBACK ViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  Viewer* v = (Viewer*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  switch (msg) {
    case WM_NCCREATE: {
      Viewer* nv = (Viewer*)((CREATESTRUCTW*)lp)->lpCreateParams;
      nv->hwnd = hwnd;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)nv);
      break;
    }
    case WM_CREATE: {
      windowOpened(); styleWindow(hwnd);
      v->bUp  = makeButton(hwnd, ID_V_UP, L"\uE74A", BTN_ICON);
      v->bSel = makeButton(hwnd, ID_V_SEL, L"Auswahl entpacken \u2026", BTN_GHOST);
      v->bAll = makeButton(hwnd, ID_V_ALL, L"Alles entpacken", BTN_PRIMARY);
      v->list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS,
                                0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_V_LIST, gHInst, NULL);
      SendMessageW(v->list, WM_SETFONT, (WPARAM)gF.body, TRUE);
      SendMessageW(v->list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
      SetWindowTheme(v->list, L"DarkMode_Explorer", NULL);
      SetWindowSubclass(v->list, ListSubclass, 1, 0);
      SendMessageW(v->list, LVM_SETBKCOLOR, 0, (LPARAM)C_SURFACE);
      SendMessageW(v->list, LVM_SETTEXTBKCOLOR, 0, (LPARAM)C_SURFACE);
      SendMessageW(v->list, LVM_SETTEXTCOLOR, 0, (LPARAM)C_TEXT);
      SHFILEINFOW sfi = {};
      HIMAGELIST il = (HIMAGELIST)SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
      SendMessageW(v->list, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)il);
      struct { const wchar_t* t; int w; int fmt; } cols[] = {{L"Name", 380, LVCFMT_LEFT}, {L"Gr\u00F6\u00DFe", 100, LVCFMT_RIGHT},
                                                            {L"Ge\u00E4ndert am", 150, LVCFMT_LEFT}, {L"Typ", 190, LVCFMT_LEFT}};
      for (int c = 0; c < 4; c++) {
        LVCOLUMNW col = {}; col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;
        col.pszText = const_cast<LPWSTR>(cols[c].t); col.cx = S(cols[c].w); col.fmt = cols[c].fmt; col.iSubItem = c;
        SendMessageW(v->list, LVM_INSERTCOLUMNW, (WPARAM)c, (LPARAM)&col);
      }
      viewerLayout(v);
      viewerRefresh(v);
      SetFocus(v->list);
      return 0;
    }
    case WM_SIZE: if (v && v->list) viewerLayout(v); return 0;
    case WM_GETMINMAXINFO: { MINMAXINFO* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = S(680); mm->ptMinTrackSize.y = S(340); return 0; }
    case WM_PAINT: if (v) { paintViewer(v); return 0; } break;
    case WM_ERASEBKGND: return 1;
    case WM_DRAWITEM: drawButton((const DRAWITEMSTRUCT*)lp); return TRUE;
    case WM_COMMAND: {
      if (!v) break;
      int id = LOWORD(wp);
      if (id >= CM_OPEN && id <= CM_UP) viewerCommand(v, id);
      else if (id == ID_V_UP) viewerUp(v);
      else if (id == ID_V_ALL) viewerStart(v, 1, {}, "", v->archive.parent_path());
      else if (id == ID_V_SEL) {
        if (v->busy) { v->cancel = true; v->status = L"Wird abgebrochen \u2026"; invalidateFooter(v); return 0; }
        std::vector<int> sel = viewerSelection(v);
        if (sel.empty()) { ckDialog(hwnd, DLG_INFO, L"Nichts ausgew\u00E4hlt", L"Bitte zuerst Dateien oder Ordner in der Liste markieren."); return 0; }
        fs::path out;
        if (!pickFolder(hwnd, L"Wohin soll entpackt werden?", out)) return 0;
        viewerStart(v, 2, viewerWant(v, sel), v->cur, out);
      }
      return 0;
    }
    case WM_NOTIFY: {
      if (!v) break;
      NMHDR* nh = (NMHDR*)lp;
      if (nh->hwndFrom != v->list) break;
      if (nh->code == LVN_ITEMACTIVATE) {
        viewerActivate(v, (int)SendMessageW(v->list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_FOCUSED));
      } else if (nh->code == LVN_KEYDOWN) {
        WORD key = ((NMLVKEYDOWN*)lp)->wVKey;
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if (key == VK_BACK && !v->busy) viewerUp(v);
        else if (ctrl && key == 'A') viewerCommand(v, CM_SELECT_ALL);
        else if (ctrl && key == 'C') viewerCommand(v, CM_COPY_NAME);
      } else if (nh->code == NM_CUSTOMDRAW) {
        return drawListCustom(v, (NMLVCUSTOMDRAW*)lp);
      }
      return 0;
    }
    case WM_CONTEXTMENU:
      if (v && (HWND)wp == v->list) { viewerContextMenu(v, lp); return 0; }
      break;
    case WM_V_PROGRESS: if (v && v->busy) { v->progress = (int)wp; invalidateFooter(v); } return 0;
    case WM_V_STATUS: {
      std::wstring* s = (std::wstring*)lp;
      if (v && v->busy && !v->cancel) { v->status = *s; invalidateFooter(v); }
      delete s; return 0;
    }
    case WM_V_DONE: {
      VDone* d = (VDone*)lp;
      if (v) {
        viewerSetBusy(v, false);
        if (d->ok) {
          if (d->kind == 0) {
            if ((INT_PTR)ShellExecuteW(hwnd, L"open", d->path.c_str(), NULL, d->path.parent_path().c_str(), SW_SHOWNORMAL) <= 32)
              ShellExecuteW(hwnd, L"openas", d->path.c_str(), NULL, NULL, SW_SHOWNORMAL);
          } else if (d->kind == 3) {
            OPENASINFO oi = {d->path.c_str(), NULL, OAIF_EXEC | OAIF_HIDE_REGISTRATION};
            if (FAILED(SHOpenWithDialog(hwnd, &oi))) ShellExecuteW(hwnd, L"openas", d->path.c_str(), NULL, NULL, SW_SHOWNORMAL);
          } else {
            revealInExplorer(d->path);
          }
        }
        if (!d->msg.empty())
          ckDialog(hwnd, d->ok ? DLG_WARN : DLG_ERROR, d->ok ? L"Achtung" : L"Entpacken fehlgeschlagen", d->msg);
      }
      delete d;
      return 0;
    }
    case WM_CLOSE:
      if (v) { v->cancel = true; if (v->worker.joinable()) v->worker.join(); }
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      if (v) {
        std::error_code ec; fs::remove_all(v->tempDir, ec);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete v;
      }
      windowClosed();
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

bool openViewer(const fs::path& archive, int nShow) {
  Viewer* v = new Viewer; v->archive = fs::absolute(archive); v->data = v->archive;
  wchar_t tmp[MAX_PATH + 1] = {0}; GetTempPathW(MAX_PATH, tmp);
  v->tempDir = fs::path(tmp) / L"KlimaxCompression" / (std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount()));
  auto fail = [&]() { std::error_code ec; fs::remove_all(v->tempDir, ec); delete v; return false; };
  try {
    if (isEncryptedArchive(v->archive)) {
      v->encrypted = true;
      fs::create_directories(v->tempDir);
      v->data = v->tempDir / L"archiv.kc";
      for (bool wrong = false;;) {
        std::wstring pw;
        if (!askPassword(NULL, v->archive.filename().wstring(), wrong, pw)) return fail();
        HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
        try { Progress pr; decryptFile(v->archive, v->data, asciiOnly(pw), pr); SetCursor(old); break; }
        catch (const CKWrongPassword&) { SetCursor(old); wrong = true; }
      }
    }
    v->ai = readArchiveInfo(v->data);
  } catch (const std::exception& e) {
    ckDialog(NULL, DLG_ERROR, L"Das Archiv kann nicht ge\u00F6ffnet werden", W(e.what()));
    return fail();
  }
  U64 files = 0; for (auto& e : v->ai.ents) if (!e.dir) files++;
  {
    const auto& ents = v->ai.ents; size_t n = ents.size();
    v->dirSize.assign(n, 0); v->dirFiles.assign(n, 0); v->dirDirs.assign(n, 0);
    std::unordered_map<std::string, size_t> dirIdx;
    for (size_t i = 0; i < n; i++) if (ents[i].dir) dirIdx[ents[i].path] = i;
    for (size_t i = 0; i < n; i++) {
      const std::string& p = ents[i].path;
      for (size_t sl = p.find_last_of('/'); sl != std::string::npos && sl > 0; sl = p.find_last_of('/', sl - 1)) {
        auto it = dirIdx.find(p.substr(0, sl));
        if (it == dirIdx.end()) continue;
        if (ents[i].dir) v->dirDirs[it->second]++; else { v->dirFiles[it->second]++; v->dirSize[it->second] += ents[i].size; }
      }
    }
  }
  v->summary = std::to_wstring(files) + (files == 1 ? L" Datei" : L" Dateien") + L"   \u00B7   " + W(fmtSize(v->ai.totalSize)) + L"  \u2192  " +
               W(fmtSize(v->ai.archiveSize)) + L"   \u00B7   " + W(fmtPercent(v->ai.archiveSize, v->ai.totalSize)) +
               L"   \u00B7   " + W(codecName(v->ai)) + (v->encrypted ? L"   \u00B7   geheim" : L"");
  std::wstring title = v->archive.filename().wstring() + L" \u2013 Klimax Compression";
  HWND h = CreateWindowExW(0, L"KlimaxCompressionViewer", title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                           S(960), S(620), NULL, NULL, gHInst, v);
  if (!h) return fail();
  ShowWindow(h, nShow); UpdateWindow(h);
  return true;
}

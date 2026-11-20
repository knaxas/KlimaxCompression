#include "core.h"

#ifdef _WIN32
  #include "winapi.h"
#else
  #include <sys/stat.h>
  #include <sys/time.h>
  #include <fcntl.h>
  #include <unistd.h>
#endif

static U32 CRCT[8][256];
void initCRC() {
  for (U32 i = 0; i < 256; i++) { U32 c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; CRCT[0][i] = c; }
  for (U32 i = 0; i < 256; i++) for (int t = 1; t < 8; t++) CRCT[t][i] = (CRCT[t - 1][i] >> 8) ^ CRCT[0][CRCT[t - 1][i] & 255];
}
U32 crcUpdate(U32 crc, const U8* p, size_t n) {
  crc = ~crc;
  while (n >= 8) {
    U32 a, b; memcpy(&a, p, 4); memcpy(&b, p + 4, 4); a ^= crc;
    crc = CRCT[7][a & 255] ^ CRCT[6][(a >> 8) & 255] ^ CRCT[5][(a >> 16) & 255] ^ CRCT[4][a >> 24] ^
          CRCT[3][b & 255] ^ CRCT[2][(b >> 8) & 255] ^ CRCT[1][(b >> 16) & 255] ^ CRCT[0][b >> 24];
    p += 8; n -= 8;
  }
  while (n--) crc = CRCT[0][(crc ^ *p++) & 255] ^ (crc >> 8);
  return ~crc;
}

FILE* openFile(const fs::path& p, int mode) {
#ifdef _WIN32
  return _wfopen(p.c_str(), mode == 0 ? L"rb" : mode == 1 ? L"wb" : L"r+b");
#else
  return fopen(p.c_str(), mode == 0 ? "rb" : mode == 1 ? "wb" : "r+b");
#endif
}
void seek64(FILE* f, U64 off) {
#ifdef _WIN32
  _fseeki64(f, (I64)off, SEEK_SET);
#else
  fseeko(f, (off_t)off, SEEK_SET);
#endif
}
U64 tell64(FILE* f) {
#ifdef _WIN32
  return (U64)_ftelli64(f);
#else
  return (U64)ftello(f);
#endif
}
std::string u8(const fs::path& p) { auto s = p.generic_u8string(); return std::string(s.begin(), s.end()); }
fs::path fromU8(const std::string& s) {
#if defined(__cpp_char8_t)
  return fs::path(std::u8string(s.begin(), s.end()));
#else
  return fs::u8path(s);
#endif
}

void getMeta(const fs::path& p, U64& mtime, U32& attr) {
  mtime = 0; attr = 0;
#ifdef _WIN32
  WIN32_FILE_ATTRIBUTE_DATA d;
  if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) {
    mtime = ((U64)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
    attr = d.dwFileAttributes;
  }
#else
  struct stat st;
  if (stat(p.c_str(), &st) == 0) {
    mtime = ((U64)st.st_mtim.tv_sec + 11644473600ull) * 10000000ull + (U64)st.st_mtim.tv_nsec / 100;
    attr = (st.st_mode & S_IWUSR) ? 0 : 1;
  }
#endif
}
void setMeta(const fs::path& p, U64 mtime, U32 attr, bool isDir) {
#ifdef _WIN32
  if (mtime) {
    HANDLE h = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, isDir ? FILE_FLAG_BACKUP_SEMANTICS : 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
      FILETIME ft; ft.dwLowDateTime = (DWORD)mtime; ft.dwHighDateTime = (DWORD)(mtime >> 32);
      SetFileTime(h, NULL, NULL, &ft); CloseHandle(h);
    }
  }
  DWORD keep = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE;
  if (attr & keep) SetFileAttributesW(p.c_str(), attr & keep);
#else
  if (mtime) {
    struct timespec ts[2];
    I64 t = (I64)(mtime / 10000000ull) - 11644473600ll;
    ts[0].tv_sec = ts[1].tv_sec = t; ts[0].tv_nsec = ts[1].tv_nsec = (long)((mtime % 10000000ull) * 100);
    utimensat(AT_FDCWD, p.c_str(), ts, 0);
  }
  if (!isDir && (attr & 1)) {
    struct stat st;
    if (stat(p.c_str(), &st) == 0) chmod(p.c_str(), st.st_mode & 07777 & ~0222);
  }
#endif
}

std::string lowerExt(const fs::path& p) {
  std::string e = u8(p.extension());
  for (auto& ch : e) if (ch >= 'A' && ch <= 'Z') ch += 32;
  return e;
}

fs::path uniquePath(const fs::path& p) {
  std::error_code ec;
  if (!fs::exists(p, ec)) return p;
  fs::path dir = p.parent_path(); std::string stem = u8(p.stem()), ext = u8(p.extension());
  if (stem.empty()) { stem = ext; ext.clear(); }
  for (int k = 2; k < 100000; k++) {
    fs::path q = dir / fromU8(stem + " (" + std::to_string(k) + ")" + ext);
    if (!fs::exists(q, ec)) return q;
  }
  throw CKError("Kein freier Dateiname gefunden");
}

U64 availableMemory() {
#if defined(__EMSCRIPTEN__)
  return 1ull << 30;
#elif defined(_WIN32)
  MEMORYSTATUSEX ms = {}; ms.dwLength = sizeof(ms);
  return GlobalMemoryStatusEx(&ms) ? (U64)ms.ullAvailPhys : 0;
#else
  long pages = sysconf(_SC_AVPHYS_PAGES), size = sysconf(_SC_PAGESIZE);
  return (pages > 0 && size > 0) ? (U64)pages * (U64)size : 0;
#endif
}

static void germanComma(std::string& s) { for (auto& ch : s) if (ch == '.') ch = ','; }

std::string fmtSize(U64 b) {
  char t[64]; const char* u[] = {"B", "KB", "MB", "GB", "TB"}; double v = (double)b; int i = 0;
  while (v >= 1024 && i < 4) { v /= 1024; i++; }
  if (i == 0) snprintf(t, sizeof t, "%llu B", (unsigned long long)b); else snprintf(t, sizeof t, "%.2f %s", v, u[i]);
  std::string s = t; germanComma(s);
  return s;
}
std::string fmtTime(double sec) {
  char t[64]; int s = (int)(sec + 0.5);
  if (sec < 9.95) { snprintf(t, sizeof t, "%.1f s", sec); std::string r = t; germanComma(r); return r; }
  if (s < 60) snprintf(t, sizeof t, "%d s", s); else if (s < 3600) snprintf(t, sizeof t, "%d:%02d min", s / 60, s % 60);
  else snprintf(t, sizeof t, "%d:%02d:%02d h", s / 3600, (s / 60) % 60, s % 60);
  return t;
}
std::string fmtPercent(U64 part, U64 whole) {
  char t[32]; snprintf(t, sizeof t, "%.1f %%", whole ? 100.0 * part / whole : 100.0);
  std::string s = t; germanComma(s);
  return s;
}

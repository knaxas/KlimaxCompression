#pragma once
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <stdexcept>
#include <exception>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>
#include <mutex>

namespace fs = std::filesystem;
typedef uint8_t U8; typedef uint16_t U16; typedef uint32_t U32; typedef uint64_t U64; typedef int64_t I64;

#define CK_APP_VERSION "1.6"

void initCRC();
U32 crcUpdate(U32 crc, const U8* p, size_t n);
inline U32 hash2(U32 a, U32 b) {
  U32 h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA6Bu;
  h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15; return h;
}
inline U32 hash3(U32 a, U32 b, U32 c) { return hash2(hash2(a, b), c); }

struct CKError : std::runtime_error { using std::runtime_error::runtime_error; };
struct CKCancelled : std::runtime_error { CKCancelled() : std::runtime_error("Abgebrochen") {} };

FILE* openFile(const fs::path& p, int mode);
void seek64(FILE* f, U64 off);
U64 tell64(FILE* f);
std::string u8(const fs::path& p);
fs::path fromU8(const std::string& s);
void getMeta(const fs::path& p, U64& mtime, U32& attr);
void setMeta(const fs::path& p, U64 mtime, U32 attr, bool isDir);
std::string lowerExt(const fs::path& p);
inline bool isArchiveExt(const std::string& e) { return e == ".kc" || e == ".ck"; }
fs::path uniquePath(const fs::path& p);
U64 availableMemory();

struct OutBuf {
  FILE* f; std::vector<U8> b; size_t n = 0; U64 total = 0;
  explicit OutBuf(FILE* f_) : f(f_), b(1 << 20) {}
  inline void put(U8 c) { b[n++] = c; total++; if (n == b.size()) flush(); }
  void write(const U8* p, size_t len) {
    if (len > b.size() - n) {
      flush();
      if (len >= b.size()) { if (fwrite(p, 1, len, f) != len) throw CKError("Schreibfehler (Festplatte voll?)"); total += len; return; }
    }
    memcpy(&b[n], p, len); n += len; total += len;
  }
  void flush() { if (n && fwrite(b.data(), 1, n, f) != n) throw CKError("Schreibfehler (Festplatte voll?)"); n = 0; }
};
struct InBuf {
  FILE* f; std::vector<U8> b; size_t n = 0, i = 0; U64 left;
  InBuf(FILE* f_, U64 limit) : f(f_), b(1 << 20), left(limit) {}
  inline int get() {
    if (i == n) {
      if (!left) return 0;
      size_t want = (size_t)std::min<U64>(left, b.size());
      n = fread(b.data(), 1, want, f); i = 0;
      if (n == 0) { left = 0; return 0; }
      left -= n;
    }
    return b[i++];
  }
};

struct Progress {
  std::function<void(const std::string& phase, U64 done, U64 total)> cb;
  std::atomic<bool>* cancel = nullptr;
  U64 done = 0, total = 0; std::string phase;
  std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
  void start(const std::string& ph, U64 tot) { phase = ph; done = 0; total = tot; tick(true); }
  void add(U64 n) { done += n; tick(false); }
  void tick(bool force) {
    if (cancel && cancel->load()) throw CKCancelled();
    auto now = std::chrono::steady_clock::now();
    if (force || now - last > std::chrono::milliseconds(150)) { last = now; if (cb) cb(phase, done, total); }
  }
};

std::string fmtSize(U64 b);
std::string fmtTime(double sec);
std::string fmtPercent(U64 part, U64 whole);

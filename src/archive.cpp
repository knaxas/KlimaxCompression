#include "archive.h"
#include "model.h"
#define ZSTD_STATIC_LINKING_ONLY
#include "../third_party/zstd/lib/zstd.h"

static const char CK_MAGIC[4] = {'C','K','N','G'};
static const U8 CK_MODEL_REV = 1;

enum : int { CODEC_CM = 0, CODEC_ZSTD = 1 };

void ckInit() { initModelTables(); }

static size_t e8e9Block(U8* b, size_t n, U64 base, bool encode, bool last) {
  size_t i = 0;
  while (i < n) {
    if (b[i] == 0xE8 || b[i] == 0xE9) {
      if (i + 5 > n) { if (last) return n; return i; }
      U32 a = (U32)b[i + 1] | ((U32)b[i + 2] << 8) | ((U32)b[i + 3] << 16) | ((U32)b[i + 4] << 24);
      if ((a >> 24) == 0 || (a >> 24) == 0xff) {
        U32 off = (U32)(base + i);
        U32 r = encode ? a + off : a - off;
        r = (U32)(((int32_t)(r << 7)) >> 7);
        b[i + 1] = (U8)r; b[i + 2] = (U8)(r >> 8); b[i + 3] = (U8)(r >> 16); b[i + 4] = (U8)(r >> 24);
      }
      i += 5;
    } else i++;
  }
  return n;
}
static U64 e8e9CleanSplit(const fs::path& p, U64 want, U64 size) {
  FILE* f = openFile(p, 0); if (!f) return size;
  std::vector<U8> b(1 << 20); U64 pos = 0, base = 0;
  U64 scan = 0;
  for (;;) {
    size_t n = fread(b.data(), 1, b.size(), f); if (!n) break;
    base = pos; pos += n;
    while (scan < pos) {
      if (scan >= want) { fclose(f); return scan; }
      U8 c = b[(size_t)(scan - base)];
      scan += (c == 0xE8 || c == 0xE9) ? 5 : 1;
    }
  }
  fclose(f);
  return std::min(std::max(scan, want), size);
}

struct Ser {
  std::vector<U8> b;
  void u8v(U8 v) { b.push_back(v); }
  void var(U64 v) { while (v >= 128) { b.push_back((U8)(v | 128)); v >>= 7; } b.push_back((U8)v); }
  void u32v(U32 v) { for (int i = 0; i < 4; i++) b.push_back((U8)(v >> (8 * i))); }
  void u64v(U64 v) { for (int i = 0; i < 8; i++) b.push_back((U8)(v >> (8 * i))); }
  void str(const std::string& s) { var(s.size()); b.insert(b.end(), s.begin(), s.end()); }
};
struct Deser {
  const std::vector<U8>& b; size_t i = 0;
  explicit Deser(const std::vector<U8>& v) : b(v) {}
  void need(size_t n) { if (i + n > b.size()) throw CKError("Archiv beschaedigt (Dateitabelle)"); }
  U8 u8v() { need(1); return b[i++]; }
  U64 var() { U64 v = 0; int s = 0; for (;;) { U8 c = u8v(); v |= (U64)(c & 127) << s; if (!(c & 128)) break; s += 7; if (s > 63) throw CKError("Archiv beschaedigt"); } return v; }
  U32 u32v() { need(4); U32 v = 0; for (int k = 0; k < 4; k++) v |= (U32)b[i++] << (8 * k); return v; }
  U64 u64v() { need(8); U64 v = 0; for (int k = 0; k < 8; k++) v |= (U64)b[i++] << (8 * k); return v; }
  std::string str() { U64 n = var(); need((size_t)n); std::string s((const char*)&b[i], (size_t)n); i += (size_t)n; return s; }
};

static void writeU64(FILE* f, U64 v) { U8 t[8]; for (int i = 0; i < 8; i++) t[i] = (U8)(v >> (8 * i)); if (fwrite(t, 1, 8, f) != 8) throw CKError("Schreibfehler"); }
static U64 readU64(FILE* f) { U8 t[8]; if (fread(t, 1, 8, f) != 8) throw CKError("Archiv beschaedigt (zu kurz)"); U64 v = 0; for (int i = 0; i < 8; i++) v |= (U64)t[i] << (8 * i); return v; }

static bool isPrecompressedExt(const std::string& e) {
  static const char* L[] = {".jpg",".jpeg",".png",".gif",".webp",".heic",".avif",".mp3",".aac",".m4a",".ogg",".opus",".flac",
    ".mp4",".m4v",".mkv",".avi",".mov",".webm",".wmv",".zip",".7z",".rar",".gz",".tgz",".bz2",".xz",".zst",".lz4",".cab",
    ".kc",".ck",".jar",".apk",".docx",".xlsx",".pptx",".odt",".ods",".epub",".msi",".pdf_compressed", nullptr};
  for (int i = 0; L[i]; i++) if (e == L[i]) return true;
  return false;
}
static double entropy0(const U8* p, size_t n) {
  if (n < 1024) return 0;
  U32 h[256] = {0}; for (size_t i = 0; i < n; i++) h[p[i]]++;
  double e = 0; for (int i = 0; i < 256; i++) if (h[i]) { double q = (double)h[i] / n; e -= q * std::log2(q); }
  return e;
}

static inline U64 dupHash(U64 h, const U8* p, size_t n) {
  size_t i = 0;
  for (; i + 8 <= n; i += 8) { U64 w; memcpy(&w, p + i, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
  for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
  return h;
}

static void compressBuffer(FILE* f, const std::vector<U8>& data, int level) {
  writeU64(f, data.size()); U64 sizePos = tell64(f); writeU64(f, 0);
  U32 crc = crcUpdate(0, data.data(), data.size());
  U8 cb[4] = {(U8)crc, (U8)(crc >> 8), (U8)(crc >> 16), (U8)(crc >> 24)}; fwrite(cb, 1, 4, f);
  OutBuf ob(f);
  { Encoder enc(level, ob); for (U8 c : data) enc.byte(c); enc.flush(); }
  ob.flush();
  U64 end = tell64(f); seek64(f, sizePos); writeU64(f, ob.total); seek64(f, end);
}

struct SegEncoder {
  virtual ~SegEncoder() {}
  virtual void write(const U8* p, size_t n) = 0;
  virtual void finish() = 0;
};
struct SegDecoder {
  virtual ~SegDecoder() {}
  virtual void read(U8* out, size_t n) = 0;
};

struct CMSegEncoder : SegEncoder {
  Encoder e;
  CMSegEncoder(int level, OutBuf& ob) : e(level, ob) {}
  void write(const U8* p, size_t n) override { for (size_t i = 0; i < n; i++) e.byte(p[i]); }
  void finish() override { e.flush(); }
};
struct CMSegDecoder : SegDecoder {
  InBuf ib; Decoder d;
  CMSegDecoder(FILE* f, U64 comp, int level) : ib(f, comp), d(level, ib) {}
  void read(U8* out, size_t n) override { for (size_t i = 0; i < n; i++) out[i] = (U8)d.byte(); }
};

static void zcheck(size_t r) { if (ZSTD_isError(r)) throw CKError(std::string("zstd: ") + ZSTD_getErrorName(r)); }

struct ZParams { int level; bool longMode; U64 minSeg; };
static ZParams zparamsFor(CkMode m) {
  return m == MODE_FAST ? ZParams{7, true, 32ull << 20} : ZParams{17, true, 16ull << 20};
}

static int ceilLog2(U64 v) { int b = 0; while (b < 63 && (1ull << b) < v) b++; return b; }

static ZSTD_CCtx_params* makeZParams(int level, bool longMode, U64 raw) {
  ZSTD_CCtx_params* p = ZSTD_createCCtxParams();
  if (!p) throw std::bad_alloc();
  ZSTD_CCtxParams_init(p, level);
  ZSTD_CCtxParams_setParameter(p, ZSTD_c_checksumFlag, 0);
  if (longMode) {
    ZSTD_CCtxParams_setParameter(p, ZSTD_c_enableLongDistanceMatching, 1);
    ZSTD_CCtxParams_setParameter(p, ZSTD_c_windowLog, std::max(20, std::min(27, ceilLog2(raw))));
  }
  return p;
}
static U64 zstdEncBytes(int level, bool longMode, U64 raw) {
  ZSTD_CCtx_params* p = makeZParams(level, false, raw);
  U64 b = ZSTD_estimateCStreamSize_usingCCtxParams(p);
  ZSTD_freeCCtxParams(p);
  if (longMode) b += 2 * (1ull << std::max(20, std::min(27, ceilLog2(raw))));
  return b + (4ull << 20);
}

struct ZstdSegEncoder : SegEncoder {
  ZSTD_CCtx* c; OutBuf& ob; std::vector<U8> out;
  ZstdSegEncoder(int level, bool longMode, U64 raw, OutBuf& o) : c(ZSTD_createCCtx()), ob(o), out(ZSTD_CStreamOutSize()) {
    if (!c) throw std::bad_alloc();
    ZSTD_CCtx_params* p = makeZParams(level, longMode, raw);
    size_t r = ZSTD_CCtx_setParametersUsingCCtxParams(c, p);
    ZSTD_freeCCtxParams(p);
    zcheck(r);
    zcheck(ZSTD_CCtx_setPledgedSrcSize(c, raw));
  }
  ~ZstdSegEncoder() override { ZSTD_freeCCtx(c); }
  void pump(const U8* p, size_t n, ZSTD_EndDirective mode) {
    ZSTD_inBuffer in = {p, n, 0};
    for (;;) {
      ZSTD_outBuffer o = {out.data(), out.size(), 0};
      size_t r = ZSTD_compressStream2(c, &o, &in, mode);
      zcheck(r);
      if (o.pos) ob.write(out.data(), o.pos);
      if (mode == ZSTD_e_end ? r == 0 : in.pos == in.size) break;
    }
  }
  void write(const U8* p, size_t n) override { pump(p, n, ZSTD_e_continue); }
  void finish() override { pump(nullptr, 0, ZSTD_e_end); }
};

struct ZstdSegDecoder : SegDecoder {
  FILE* f; U64 left; ZSTD_DStream* d; std::vector<U8> inb; ZSTD_inBuffer in;
  ZstdSegDecoder(FILE* f_, U64 comp) : f(f_), left(comp), d(ZSTD_createDStream()), inb(1 << 20) {
    if (!d) throw std::bad_alloc();
    in = {inb.data(), 0, 0};
  }
  ~ZstdSegDecoder() override { ZSTD_freeDStream(d); }
  void read(U8* out, size_t n) override {
    ZSTD_outBuffer o = {out, n, 0};
    while (o.pos < o.size) {
      if (in.pos == in.size && left) {
        size_t got = fread(inb.data(), 1, (size_t)std::min<U64>(left, inb.size()), f);
        if (!got) throw CKError("Archiv beschaedigt (zu kurz)");
        left -= got; in.size = got; in.pos = 0;
      }
      size_t oBefore = o.pos, iBefore = in.pos;
      size_t r = ZSTD_decompressStream(d, &o, &in);
      if (ZSTD_isError(r)) throw CKError("Archiv beschaedigt (zstd)");
      if (o.pos == oBefore && in.pos == iBefore && in.pos == in.size && !left) throw CKError("Archiv beschaedigt (zu kurz)");
    }
  }
};

struct Piece { U32 e; U64 off, len; };
struct SegPlan { std::vector<Piece> pieces; U64 raw = 0; };
static const U64 MIN_SEG = 16ull << 20;

static std::vector<U64> planSegments(const std::vector<Entry>& ents, const std::vector<U32>& solid, U64 S, int nseg) {
  std::vector<U64> sizes;
  if (S == 0) return sizes;
  U64 target = (S + nseg - 1) / nseg, cur = 0;
  for (U32 idx : solid) {
    const Entry& e = ents[idx]; U64 left = e.size, off = 0;
    while (left) {
      if ((int)sizes.size() + 1 >= nseg) { cur += left; left = 0; break; }
      U64 take = std::min(left, target - cur);
      if (e.filter == F_E8E9 && take < left) take = e8e9CleanSplit(e.src, off + take, e.size) - off;
      off += take;
      cur += take; left -= take;
      if (cur >= target) { sizes.push_back(cur); cur = 0; }
    }
  }
  if (cur) sizes.push_back(cur);
  return sizes;
}

static std::vector<SegPlan> mapSegments(const std::vector<Entry>& ents, const std::vector<U32>& solid, const std::vector<U64>& segRaw) {
  std::vector<SegPlan> segs(segRaw.size());
  size_t si = 0; U64 room = segRaw.empty() ? 0 : segRaw[0];
  for (U32 idx : solid) {
    U64 off = 0, left = ents[idx].size;
    while (left) {
      while (room == 0) { si++; if (si >= segs.size()) throw CKError("Archiv beschaedigt (Segmente)"); room = segRaw[si]; }
      U64 take = std::min(left, room);
      segs[si].pieces.push_back({idx, off, take}); segs[si].raw += take;
      off += take; left -= take; room -= take;
    }
  }
  return segs;
}

#ifdef __EMSCRIPTEN__
static Progress* gInlineProg = nullptr;
static std::atomic<U64>* gInlineDone = nullptr;
static U64 gInlineBase = 0;
static void inlinePump() { if (gInlineProg) { gInlineProg->done = gInlineBase + gInlineDone->load(); gInlineProg->tick(false); } }
#else
static inline void inlinePump() {}
#endif

static int hwThreads() {
#ifdef __EMSCRIPTEN__
  return 1;
#endif
  unsigned hw = std::thread::hardware_concurrency();
  return (int)std::max(1u, std::min(hw ? hw : 2u, 64u));
}
static U64 memBudget() {
  U64 a = availableMemory();
  return a ? a / 10 * 6 : (1ull << 30);
}
static U64 modelBytes(int L) {
  U64 t = 12ull << (L + 2);
  t += 1ull << std::min(30, std::max(18, L + 3));
  t += 2 * (4ull << std::max(16, L - 1));
  return t + (10ull << 20);
}
static int parallelJobs(U64 bytesPerJob, int jobs) {
  U64 byRam = std::max<U64>(1, memBudget() / std::max<U64>(1, bytesPerJob));
  return (int)std::max<U64>(1, std::min<U64>({(U64)jobs, (U64)hwThreads(), byRam}));
}

static int chooseLevel(U64 raw, int nseg) {
  int maxL = nseg <= 1 ? 24 : nseg == 2 ? 23 : nseg <= 4 ? 22 : nseg <= 8 ? 21 : 20;
  U64 par = (U64)std::min(nseg, hwThreads());
  while (maxL > 18 && modelBytes(maxL) * par > memBudget()) maxL--;
  int b = 0; while (b < 40 && (1ull << b) < raw) b++;
  return std::max(18, std::min(maxL, b + 3));
}

struct SegJob {
  SegPlan plan; int codec = CODEC_CM; int level = 22; bool longMode = false;
  fs::path tmp; U64 comp = 0; std::exception_ptr err; std::vector<U32> partial;
};

static void compressSegment(SegJob& job, const std::vector<Entry>& ents, std::atomic<U64>& done, std::atomic<bool>& stop) {
  FILE* out = nullptr;
  try {
    out = openFile(job.tmp, 1);
    if (!out) throw CKError("Kann temporaere Datei nicht anlegen: " + u8(job.tmp));
    OutBuf ob(out);
    std::unique_ptr<SegEncoder> enc;
    if (job.codec == CODEC_ZSTD) enc.reset(new ZstdSegEncoder(job.level, job.longMode, job.plan.raw, ob));
    else for (;;) {
      try { enc.reset(new CMSegEncoder(job.level, ob)); break; }
      catch (const std::bad_alloc&) { if (job.level <= 18) throw CKError("Zu wenig Arbeitsspeicher"); job.level--; }
    }
    std::vector<U8> io(1 << 20);
    for (const Piece& pc : job.plan.pieces) {
      const Entry& e = ents[pc.e];
      FILE* f = openFile(e.src, 0);
      if (!f) throw CKError("Kann Datei nicht lesen: " + u8(e.src));
      std::unique_ptr<FILE, int (*)(FILE*)> fg(f, fclose);
      seek64(f, pc.off);
      U64 left = pc.len, base = pc.off; size_t have = 0;
      bool filt = e.filter == F_E8E9;
      while (left || have) {
        size_t want = (size_t)std::min<U64>(left, io.size() - have);
        size_t n = want ? fread(io.data() + have, 1, want, f) : 0;
        if (n < want) std::fill(io.begin() + have + n, io.begin() + have + want, 0);
        left -= want; have += want;
        size_t fin = filt ? e8e9Block(io.data(), have, base, true, left == 0) : have;
        for (size_t k = 0; k < fin; k += 65536) {
          size_t c = std::min<size_t>(65536, fin - k);
          enc->write(io.data() + k, c);
          done += c; inlinePump(); if (stop) throw CKCancelled();
        }
        memmove(io.data(), io.data() + fin, have - fin); have -= fin; base += fin;
      }
    }
    enc->finish(); ob.flush(); job.comp = ob.total;
    enc.reset();
    if (fclose(out) != 0) { out = nullptr; throw CKError("Schreibfehler (Festplatte voll?)"); }
    out = nullptr;
  } catch (...) {
    if (out) fclose(out);
    job.err = std::current_exception(); stop = true;
  }
}

template <class Job>
static void runParallel(std::vector<Job>& jobs, std::function<void(Job&)> fn, std::atomic<U64>& done, std::atomic<bool>& stop,
                        Progress& prog, U64 base, int maxPar) {
#ifdef __EMSCRIPTEN__
  (void)maxPar;
  gInlineProg = &prog; gInlineDone = &done; gInlineBase = base;
  for (auto& j : jobs) if (!stop) fn(j);
  gInlineProg = nullptr; gInlineDone = nullptr;
#else
  std::atomic<int> finished{0}, next{0};
  std::vector<std::thread> th;
  int nth = std::max(1, std::min(maxPar, (int)jobs.size()));
  for (int t = 0; t < nth; t++)
    th.emplace_back([&] {
      for (;;) {
        int k = next++;
        if (k >= (int)jobs.size()) return;
        if (!stop) fn(jobs[k]);
        finished++;
      }
    });
  bool cancelled = false;
  while (finished.load() < (int)jobs.size()) {
    prog.done = base + done.load();
    try { prog.tick(false); } catch (const CKCancelled&) { stop = true; cancelled = true; break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  for (auto& t : th) t.join();
  if (cancelled) throw CKCancelled();
#endif
  std::exception_ptr cancel;
  for (auto& j : jobs) if (j.err) {
    try { std::rethrow_exception(j.err); } catch (const CKCancelled&) { if (!cancel) cancel = j.err; }
  }
  if (cancel) std::rethrow_exception(cancel);
  prog.done = base + done.load();
}

struct Scan { U32 crc = 0; U64 hash = 0, size = 0; bool highEntropy = false, exe = false; std::exception_ptr err; };

CompressResult compressPath(fs::path input, const fs::path& outFile, Progress& prog, int maxThreads, CkMode mode) {
  CompressResult res;
  std::error_code ec;
  input = fs::absolute(input).lexically_normal();
  if (input.has_filename() == false) input = input.parent_path();
  if (!fs::exists(input)) throw CKError("Eingabe existiert nicht: " + u8(input));
  std::string root = u8(input.filename());

  std::vector<Entry> ents;
  auto addEntry = [&](const fs::path& p, const std::string& rel, bool dir) {
    Entry e; e.path = rel; e.dir = dir; e.src = p; getMeta(p, e.mtime, e.attr);
    if (!dir) { e.size = fs::file_size(p, ec); if (ec) return; }
    ents.push_back(std::move(e));
  };
  if (fs::is_directory(input)) {
    addEntry(input, root, true);
    for (auto it = fs::recursive_directory_iterator(input, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) { ec.clear(); continue; }
      const fs::path& p = it->path();
      if (p == outFile) continue;
      std::string rel = root + "/" + u8(fs::relative(p, input, ec));
      if (it->is_symlink(ec)) continue;
      if (it->is_directory(ec)) addEntry(p, rel, true);
      else if (it->is_regular_file(ec)) addEntry(p, rel, false);
    }
  } else addEntry(input, root, false);

  U64 totalBytes = 0; for (auto& e : ents) totalBytes += e.size;
  res.inBytes = totalBytes;
  int nThreads = hwThreads(); if (maxThreads > 0) nThreads = std::min(nThreads, maxThreads);

  prog.start("Analysiere", totalBytes);
  std::vector<U32> fileIdx; for (U32 i = 0; i < ents.size(); i++) if (!ents[i].dir) fileIdx.push_back(i);
  std::vector<Scan> scans(fileIdx.size());
  {
    std::atomic<U64> done{0}; std::atomic<bool> stop{false};
    std::vector<size_t> order(fileIdx.size()); for (size_t k = 0; k < order.size(); k++) order[k] = k;
    struct ScanJob { size_t k; std::exception_ptr err; };
    std::vector<ScanJob> sj; for (size_t k : order) sj.push_back({k, nullptr});
    runParallel<ScanJob>(sj, [&](ScanJob& j) {
      try {
        const Entry& e = ents[fileIdx[j.k]]; Scan& s = scans[j.k];
        FILE* f = openFile(e.src, 0);
        if (!f) throw CKError("Kann Datei nicht lesen: " + u8(e.src));
        std::unique_ptr<FILE, int (*)(FILE*)> fg(f, fclose);
        std::vector<U8> io(1 << 20), sample; sample.reserve(1 << 18);
        s.hash = 0x51ED27F1C0FFEEull ^ e.size;
        for (;;) {
          size_t n = fread(io.data(), 1, io.size(), f); if (!n) break;
          s.crc = crcUpdate(s.crc, io.data(), n);
          s.hash = dupHash(s.hash, io.data(), n);
          if (sample.size() < (1 << 18)) sample.insert(sample.end(), io.begin(), io.begin() + std::min(n, (size_t)(1 << 18) - sample.size()));
          s.size += n; done += n; inlinePump();
          if (stop) throw CKCancelled();
        }
        s.highEntropy = entropy0(sample.data(), sample.size()) > 7.97;
        s.exe = sample.size() >= 64 &&
                ((sample[0] == 'M' && sample[1] == 'Z') || (sample[0] == 0x7f && sample[1] == 'E' && sample[2] == 'L' && sample[3] == 'F'));
      } catch (...) { j.err = std::current_exception(); stop = true; }
    }, done, stop, prog, 0, nThreads);
  }
  std::unordered_map<std::string, U32> seen;
  for (size_t k = 0; k < fileIdx.size(); k++) {
    U32 idx = fileIdx[k]; Entry& e = ents[idx]; const Scan& s = scans[k];
    res.files++;
    e.size = s.size; e.crc = s.crc; e.h64 = s.hash;
    std::string key = std::to_string(e.size) + ":" + std::to_string(s.hash) + ":" + std::to_string(s.crc);
    auto it = seen.find(key);
    if (e.size > 0 && it != seen.end()) { e.method = M_DUP; e.dupOf = it->second; res.dups++; continue; }
    seen[key] = idx;
    if (e.size >= 4096 && (isPrecompressedExt(lowerExt(e.src)) || s.highEntropy)) { e.method = M_STORED; res.stored++; }
    else if (s.exe) e.filter = F_E8E9;
  }

  std::vector<U32> order;
  for (U32 i = 0; i < ents.size(); i++) if (ents[i].dir) order.push_back(i);
  std::vector<U32> solid, stored, dups;
  for (U32 i = 0; i < ents.size(); i++) if (!ents[i].dir) {
    if (ents[i].method == M_SOLID) solid.push_back(i); else if (ents[i].method == M_STORED) stored.push_back(i); else dups.push_back(i);
  }
  std::stable_sort(solid.begin(), solid.end(), [&](U32 a, U32 b) {
    std::string ea = lowerExt(ents[a].src), eb = lowerExt(ents[b].src);
    if (ea != eb) return ea < eb;
    std::string na = u8(ents[a].src.filename()), nb = u8(ents[b].src.filename());
    if (na != nb) return na < nb;
    return ents[a].path < ents[b].path;
  });
  order.insert(order.end(), solid.begin(), solid.end());
  order.insert(order.end(), stored.begin(), stored.end());
  order.insert(order.end(), dups.begin(), dups.end());
  std::vector<U32> newIndex(ents.size());
  for (U32 k = 0; k < order.size(); k++) newIndex[order[k]] = k;

  Ser s; s.var(order.size());
  for (U32 k = 0; k < order.size(); k++) {
    const Entry& e = ents[order[k]];
    s.u8v(e.dir ? 0 : 1); s.str(e.path); s.u64v(e.mtime); s.u32v(e.attr);
    if (!e.dir) { s.var(e.size); s.u8v(e.method); s.u8v(e.filter); s.u32v(e.crc); if (e.method == M_DUP) s.var(newIndex[e.dupOf]); }
  }

  U64 S = 0; for (U32 i : solid) S += ents[i].size;
  U64 storedBytes = 0; for (U32 i : stored) storedBytes += ents[i].size;
  bool useZ = mode != MODE_MAX;
  ZParams zp = zparamsFor(mode);
  U64 minSeg = useZ ? zp.minSeg : MIN_SEG;
  int nseg = (S < 2 * minSeg) ? 1 : (int)std::min<U64>({(U64)nThreads, S / minSeg, 255});
  std::vector<U64> sizes = planSegments(ents, solid, S, nseg);
  std::vector<SegPlan> plans = mapSegments(ents, solid, sizes);
  std::vector<SegJob> jobs(plans.size());
  U64 perJob = 0;
  for (size_t k = 0; k < jobs.size(); k++) {
    SegJob& j = jobs[k];
    j.plan = plans[k];
    if (useZ) { j.codec = CODEC_ZSTD; j.level = zp.level; j.longMode = zp.longMode; perJob = std::max(perJob, zstdEncBytes(zp.level, zp.longMode, j.plan.raw)); }
    else { j.level = chooseLevel(plans[k].raw, (int)plans.size()); perJob = std::max(perJob, modelBytes(j.level)); }
    j.tmp = outFile; j.tmp += ".part" + std::to_string(k);
  }
  res.segments = (int)jobs.size();
  fs::path tmp = outFile; tmp += ".part";
  auto cleanup = [&]() { for (auto& j : jobs) fs::remove(j.tmp, ec); fs::remove(tmp, ec); };

  prog.start("Komprimiere", S + storedBytes);
  std::atomic<U64> done{0}; std::atomic<bool> stop{false};
  try {
    runParallel<SegJob>(jobs, [&](SegJob& j) { compressSegment(j, ents, done, stop); }, done, stop, prog, 0,
                        std::min(nThreads, parallelJobs(perJob, (int)jobs.size())));
  } catch (...) { cleanup(); throw; }

  bool allCM = true; for (auto& j : jobs) if (j.codec != CODEC_CM) allCM = false;
  int version = allCM ? 1 : 2;
  std::vector<U8> io(1 << 20);
  FILE* out = openFile(tmp, 1);
  if (!out) { cleanup(); throw CKError("Kann Archiv nicht anlegen: " + u8(outFile)); }
  try {
    fwrite(CK_MAGIC, 1, 4, out); fputc(version, out); fputc(CK_MODEL_REV, out);
    compressBuffer(out, s.b, 16);
    writeU64(out, S); fputc((int)jobs.size(), out);
    for (auto& j : jobs) {
      writeU64(out, j.plan.raw); writeU64(out, j.comp);
      if (version >= 2) fputc(j.codec, out);
      fputc(j.level, out);
    }
    for (auto& j : jobs) {
      FILE* f = openFile(j.tmp, 0); if (!f) throw CKError("Temporaere Datei verschwunden");
      for (;;) { size_t n = fread(io.data(), 1, io.size(), f); if (!n) break; if (fwrite(io.data(), 1, n, out) != n) { fclose(f); throw CKError("Schreibfehler (Festplatte voll?)"); } }
      fclose(f); fs::remove(j.tmp, ec);
    }
    prog.done = S;
    for (U32 i : stored) {
      Entry& e = ents[i];
      FILE* f = openFile(e.src, 0); if (!f) throw CKError("Kann Datei nicht lesen: " + u8(e.src));
      U64 left = e.size;
      while (left) {
        size_t want = (size_t)std::min<U64>(left, io.size());
        size_t n = fread(io.data(), 1, want, f);
        if (n < want) std::fill(io.begin() + n, io.begin() + want, 0);
        if (fwrite(io.data(), 1, want, out) != want) { fclose(f); throw CKError("Schreibfehler (Festplatte voll?)"); }
        left -= want; prog.add(want);
      }
      fclose(f);
    }
    res.outBytes = tell64(out);
    if (fclose(out) != 0) { out = nullptr; throw CKError("Schreibfehler beim Abschliessen"); }
    out = nullptr;
    fs::remove(outFile, ec);
    fs::rename(tmp, outFile);
    res.out = outFile;
  } catch (...) {
    if (out) fclose(out);
    cleanup();
    throw;
  }
  return res;
}

static bool safeRelPath(const std::string& p) {
  if (p.empty() || p[0] == '/' || p[0] == '\\' || p.find(':') != std::string::npos) return false;
  size_t st = 0;
  while (st <= p.size()) {
    size_t e = p.find('/', st); if (e == std::string::npos) e = p.size();
    std::string comp = p.substr(st, e - st);
    if (comp.empty() || comp == "." || comp == ".." || comp.find('\\') != std::string::npos) return false;
    st = e + 1;
  }
  return true;
}

static ArchiveInfo readArchiveInfo(FILE* f) {
  ArchiveInfo ai;
  char mg[4]; if (fread(mg, 1, 4, f) != 4 || memcmp(mg, CK_MAGIC, 4) != 0) throw CKError("Keine Klimax-Compression-Datei");
  int ver = fgetc(f), rev = fgetc(f);
  if (ver > 2) throw CKError("Archiv ist von einer neueren Version von Klimax Compression. Bitte aktualisieren.");
  if (ver < 1 || rev != CK_MODEL_REV) throw CKError("Archiv stammt von einer anderen Version von Klimax Compression");
  ai.version = ver;
  U64 tRaw = readU64(f), tComp = readU64(f);
  U8 cb[4]; if (fread(cb, 1, 4, f) != 4) throw CKError("Archiv beschaedigt (zu kurz)");
  U32 tCrc = cb[0] | (cb[1] << 8) | (cb[2] << 16) | ((U32)cb[3] << 24);
  if (tRaw > (1ull << 31)) throw CKError("Archiv beschaedigt");
  std::vector<U8> tb((size_t)tRaw);
  U64 tStart = tell64(f);
  { InBuf ib(f, tComp); Decoder d(16, ib); for (auto& c : tb) c = (U8)d.byte(); }
  if (crcUpdate(0, tb.data(), tb.size()) != tCrc) throw CKError("Archiv beschaedigt (Dateitabelle)");
  seek64(f, tStart + tComp);
  Deser ds(tb);
  U64 n = ds.var();
  if (n > (1ull << 28)) throw CKError("Archiv beschaedigt");
  ai.ents.resize((size_t)n);
  for (auto& e : ai.ents) {
    e.dir = ds.u8v() == 0; e.path = ds.str(); e.mtime = ds.u64v(); e.attr = ds.u32v();
    if (!e.dir) { e.size = ds.var(); e.method = ds.u8v(); e.filter = ds.u8v(); e.crc = ds.u32v(); if (e.method == M_DUP) e.dupOf = (U32)ds.var(); }
    if (!safeRelPath(e.path)) throw CKError("Unsicherer Pfad im Archiv");
    ai.totalSize += e.size;
  }
  for (auto& e : ai.ents)
    if (!e.dir && e.method == M_DUP && (e.dupOf >= ai.ents.size() || ai.ents[e.dupOf].dir || ai.ents[e.dupOf].method == M_DUP))
      throw CKError("Archiv beschaedigt (Duplikat)");
  ai.S = readU64(f); ai.nseg = fgetc(f);
  if (ai.nseg < 0) throw CKError("Archiv beschaedigt");
  ai.segRaw.resize(ai.nseg); ai.segComp.resize(ai.nseg); ai.segLevel.resize(ai.nseg); ai.segOff.resize(ai.nseg); ai.segCodec.resize(ai.nseg);
  U64 sumRaw = 0;
  for (int k = 0; k < ai.nseg; k++) {
    ai.segRaw[k] = readU64(f); ai.segComp[k] = readU64(f);
    ai.segCodec[k] = ver >= 2 ? fgetc(f) : CODEC_CM;
    ai.segLevel[k] = fgetc(f); sumRaw += ai.segRaw[k];
    bool ok = ai.segCodec[k] == CODEC_CM ? (ai.segLevel[k] >= 16 && ai.segLevel[k] <= 27)
            : ai.segCodec[k] == CODEC_ZSTD ? (ai.segLevel[k] >= 1 && ai.segLevel[k] <= 22) : false;
    if (!ok) throw CKError("Archiv beschaedigt (Level)");
  }
  if (sumRaw != ai.S) throw CKError("Archiv beschaedigt (Segmente)");
  U64 off = tell64(f);
  for (int k = 0; k < ai.nseg; k++) { ai.segOff[k] = off; off += ai.segComp[k]; }
  ai.storedOff = off;
  U64 checkS = 0; for (auto& e : ai.ents) if (!e.dir && e.method == M_SOLID) checkS += e.size;
  if (checkS != ai.S) throw CKError("Archiv beschaedigt (Groessen)");
  return ai;
}

ArchiveInfo readArchiveInfo(const fs::path& archive) {
  FILE* f = openFile(archive, 0);
  if (!f) throw CKError("Kann Archiv nicht oeffnen");
  std::unique_ptr<FILE, int (*)(FILE*)> guard(f, fclose);
  ArchiveInfo ai = readArchiveInfo(f);
  seek64(f, 0); fseek(f, 0, SEEK_END); ai.archiveSize = tell64(f);
  return ai;
}

std::string codecName(const ArchiveInfo& ai) {
  bool cm = false, z = false; int zl = 0;
  for (int k = 0; k < ai.nseg; k++) { if (ai.segCodec[k] == CODEC_ZSTD) { z = true; zl = ai.segLevel[k]; } else cm = true; }
  if (z && !cm) return zl <= 9 ? "zstd (Schnell)" : "zstd (Ausgewogen)";
  if (z && cm) return "gemischt";
  return "Context Mixing (Maximal)";
}

ExtractResult extractArchive(const fs::path& archive, const fs::path& outDir, Progress& prog,
                             const std::vector<char>* want, const std::string& prefix) {
  ExtractResult res; std::error_code ec;
  FILE* f = openFile(archive, 0);
  if (!f) throw CKError("Kann Archiv nicht oeffnen");
  std::unique_ptr<FILE, int (*)(FILE*)> guard(f, fclose);
  ArchiveInfo ai = readArchiveInfo(f);
  std::vector<Entry>& ents = ai.ents;
  if (ents.empty()) return res;
  size_t N = ents.size();

  std::vector<char> need(N, 0), viaOriginal(N, 0);
  for (size_t i = 0; i < N; i++) need[i] = want ? (*want)[i] : 1;
  std::unordered_map<std::string, std::string> topMap;
  auto mapTop = [&](const std::string& rel) -> fs::path {
    size_t sl = rel.find('/'); std::string top = rel.substr(0, sl);
    auto it = topMap.find(top);
    if (it == topMap.end()) it = topMap.emplace(top, u8(uniquePath(outDir / fromU8(top)).filename())).first;
    return outDir / fromU8(it->second + (sl == std::string::npos ? "" : rel.substr(sl)));
  };
  std::vector<fs::path> tgt(N);
  for (size_t i = 0; i < N; i++) {
    if (!need[i]) continue;
    if (ents[i].path.compare(0, prefix.size(), prefix) != 0 || ents[i].path.size() == prefix.size()) throw CKError("Ungueltige Auswahl");
    tgt[i] = mapTop(ents[i].path.substr(prefix.size()));
    if (!ents[i].dir && res.firstFile.empty()) res.firstFile = tgt[i];
  }
  for (size_t i = 0; i < N; i++) {
    if (!need[i] || ents[i].dir || ents[i].method != M_DUP) continue;
    U32 o = ents[i].dupOf;
    if (!need[o]) { need[o] = 1; tgt[o] = tgt[i]; viaOriginal[i] = 1; }
  }
  if (!topMap.empty()) res.root = outDir / fromU8(topMap.begin()->second);
  if (!want && !ents.empty()) res.root = tgt[0];

  std::vector<U32> solid;
  for (U32 i = 0; i < N; i++) if (!ents[i].dir && ents[i].method == M_SOLID) solid.push_back(i);
  std::vector<SegPlan> plans = mapSegments(ents, solid, ai.segRaw);
  std::vector<SegJob> jobs;
  std::vector<int> jobSeg; std::vector<size_t> jobLast;
  U64 work = 0, perJob = 0;
  for (int k = 0; k < ai.nseg; k++) {
    int last = -1; U64 w = 0;
    for (size_t q = 0; q < plans[k].pieces.size(); q++) if (need[plans[k].pieces[q].e]) last = (int)q;
    if (last < 0) continue;
    for (int q = 0; q <= last; q++) w += plans[k].pieces[q].len;
    SegJob j; j.plan = plans[k]; j.level = ai.segLevel[k]; j.codec = ai.segCodec[k];
    perJob = std::max(perJob, j.codec == CODEC_CM ? modelBytes(j.level) : (U64)(160ull << 20));
    jobs.push_back(std::move(j)); jobSeg.push_back(k); jobLast.push_back((size_t)last); work += w;
  }
  U64 storedNeed = 0; for (size_t i = 0; i < N; i++) if (need[i] && !ents[i].dir && ents[i].method == M_STORED) storedNeed += ents[i].size;

  for (size_t i = 0; i < N; i++) if (need[i] && ents[i].dir) fs::create_directories(tgt[i], ec);
  for (U32 i : solid) {
    if (!need[i]) continue;
    fs::create_directories(tgt[i].parent_path(), ec);
    FILE* o = openFile(tgt[i], 1); if (!o) throw CKError("Kann nicht schreiben: " + u8(tgt[i])); fclose(o);
  }
  prog.start("Entpacke", work + storedNeed);

  std::atomic<U64> done{0}; std::atomic<bool> stop{false}; std::atomic<int> crcErr{0};
  auto decodeSeg = [&](SegJob& job) {
    size_t ji = &job - &jobs[0]; int k = jobSeg[ji];
    FILE* a = nullptr; FILE* o = nullptr;
    try {
      a = openFile(archive, 0); if (!a) throw CKError("Kann Archiv nicht oeffnen");
      seek64(a, ai.segOff[k]);
      std::unique_ptr<SegDecoder> dec;
      try {
        if (job.codec == CODEC_ZSTD) dec.reset(new ZstdSegDecoder(a, ai.segComp[k]));
        else dec.reset(new CMSegDecoder(a, ai.segComp[k], job.level));
      } catch (const std::bad_alloc&) { throw CKError("Zu wenig Arbeitsspeicher zum Entpacken"); }
      std::vector<U8> io(1 << 20);
      auto pull = [&](U8* dst, U64 n) {
        for (U64 r = 0; r < n; r += 65536) {
          size_t c = (size_t)std::min<U64>(65536, n - r);
          dec->read(dst ? dst + r : io.data(), c);
          done += c; inlinePump(); if (stop) throw CKCancelled();
        }
      };
      for (size_t q = 0; q <= jobLast[ji]; q++) {
        const Piece& pc = job.plan.pieces[q];
        const Entry& e = ents[pc.e];
        if (!need[pc.e]) { pull(nullptr, pc.len); continue; }
        const fs::path& t = tgt[pc.e];
        bool whole = (pc.off == 0 && pc.len == e.size);
        bool filt = e.filter == F_E8E9;
        o = openFile(t, 2); if (!o) throw CKError("Kann nicht schreiben: " + u8(t));
        seek64(o, pc.off);
        U32 crc = 0; U64 left = pc.len, base = pc.off; size_t have = 0;
        while (left || have) {
          size_t wnt = (size_t)std::min<U64>(left, io.size() - have);
          pull(io.data() + have, wnt);
          left -= wnt; have += wnt;
          size_t fin = filt ? e8e9Block(io.data(), have, base, false, left == 0) : have;
          if (whole) crc = crcUpdate(crc, io.data(), fin);
          if (fin && fwrite(io.data(), 1, fin, o) != fin) throw CKError("Schreibfehler (Festplatte voll?)");
          memmove(io.data(), io.data() + fin, have - fin); have -= fin; base += fin;
        }
        if (fclose(o) != 0) { o = nullptr; throw CKError("Schreibfehler"); }
        o = nullptr;
        if (whole) { if (crc != e.crc) crcErr++; } else job.partial.push_back(pc.e);
      }
    } catch (...) { job.err = std::current_exception(); stop = true; }
    if (o) fclose(o);
    if (a) fclose(a);
  };
  runParallel<SegJob>(jobs, decodeSeg, done, stop, prog, 0, parallelJobs(perJob, (int)jobs.size()));

  {
    std::vector<U32> parts; for (auto& j : jobs) parts.insert(parts.end(), j.partial.begin(), j.partial.end());
    std::sort(parts.begin(), parts.end()); parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
    std::vector<U8> io(1 << 20);
    for (U32 i : parts) {
      FILE* r = openFile(tgt[i], 0); if (!r) { crcErr++; continue; }
      U32 crc = 0; for (;;) { size_t m = fread(io.data(), 1, io.size(), r); if (!m) break; crc = crcUpdate(crc, io.data(), m); }
      fclose(r); if (crc != ents[i].crc) crcErr++;
    }
  }
  auto countIt = [&](size_t i) { if (!want || (*want)[i]) { res.files++; res.bytes += ents[i].size; } };
  for (U32 i : solid) if (need[i]) countIt(i);

  prog.done = work;
  {
    std::vector<U8> io(1 << 20);
    U64 off = ai.storedOff;
    for (size_t i = 0; i < N; i++) {
      Entry& e = ents[i];
      if (e.dir || e.method != M_STORED) continue;
      U64 myOff = off; off += e.size;
      if (!need[i]) continue;
      seek64(f, myOff);
      fs::create_directories(tgt[i].parent_path(), ec);
      FILE* o = openFile(tgt[i], 1); if (!o) throw CKError("Kann nicht schreiben: " + u8(tgt[i]));
      U32 crc = 0; U64 left = e.size;
      while (left) {
        size_t wnt = (size_t)std::min<U64>(left, io.size());
        if (fread(io.data(), 1, wnt, f) != wnt) { fclose(o); throw CKError("Archiv beschaedigt (zu kurz)"); }
        crc = crcUpdate(crc, io.data(), wnt);
        if (fwrite(io.data(), 1, wnt, o) != wnt) { fclose(o); throw CKError("Schreibfehler"); }
        left -= wnt; prog.add(wnt);
      }
      fclose(o); if (crc != e.crc) crcErr++; countIt(i);
    }
  }
  for (size_t i = 0; i < N; i++) {
    Entry& e = ents[i];
    if (!need[i] || e.dir || e.method != M_DUP) continue;
    countIt(i);
    if (viaOriginal[i]) continue;
    fs::create_directories(tgt[i].parent_path(), ec);
    fs::copy_file(tgt[e.dupOf], tgt[i], fs::copy_options::overwrite_existing, ec);
    if (ec) throw CKError("Kann Duplikat nicht anlegen: " + u8(tgt[i]));
  }
  for (size_t i = 0; i < N; i++) if (need[i] && !ents[i].dir) setMeta(tgt[i], ents[i].mtime, ents[i].attr, false);
  for (size_t k = N; k-- > 0;) if (need[k] && ents[k].dir) setMeta(tgt[k], ents[k].mtime, ents[k].attr, true);
  res.crcErrors = crcErr.load();
  prog.tick(true);
  return res;
}

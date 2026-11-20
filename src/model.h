#pragma once
#include "core.h"
#include <cmath>
#if defined(_MSC_VER)
  #include <intrin.h>
#endif
#if defined(__AVX2__)
  #include <immintrin.h>
#endif

inline int G_LR = 4, G_LIM = 127, G_LR2 = 2, G_APMR = 6, G_LRB = 6, G_X1 = 1, G_FM = 2, G_SKIP = 32;

inline int SQ_T[33] = {1,2,3,6,10,16,27,45,73,120,194,310,488,747,1101,1546,2047,2549,2994,3348,3607,3785,3901,3975,4022,4050,4068,4079,4085,4089,4092,4093,4094};
inline short STRETCH[4096];
inline int DT[1024];
inline int squash(int d) {
  if (d > 2047) return 4095;
  if (d < -2047) return 1;
  int w = d & 127; int i = (d >> 7) + 16;
  return (SQ_T[i] * (128 - w) + SQ_T[i + 1] * w + 64) >> 7;
}
inline int stretch(int p) { return STRETCH[p]; }
inline void initModelTables() {
  int pi = 0;
  for (int x = -2047; x <= 2047; x++) { int v = squash(x); for (int j = pi; j <= v; j++) STRETCH[j] = (short)x; pi = v + 1; }
  for (int j = pi; j < 4096; j++) STRETCH[j] = 2047;
  for (int n = 0; n < 1024; n++) DT[n] = 131072 / (2 * n + 3);
  initCRC();
}

static const U32 CTR_INIT = 1u << 31;
inline void ctrUpdate(U32& t, int y, int limit) {
  int n = t & 1023; I64 p = t >> 10;
  I64 target = y ? 4194303 : 0;
  p += ((target - p) * DT[n]) >> 16;
  if (n < limit) n++;
  t = ((U32)p << 10) | (U32)n;
}
inline int ctrP12(U32 t) { return (int)(t >> 20); }

struct APM {
  std::vector<U16> t; int idx = 0; int rate;
  APM(int n, int r) : t((size_t)n * 33), rate(r) {
    for (int i = 0; i < n; i++) for (int j = 0; j < 33; j++) t[(size_t)i * 33 + j] = (U16)std::min(65535, squash((j - 16) * 128) * 16);
  }
  inline int pp(int pr, int cx) {
    int s = stretch(pr) + 2048; int lo = s >> 7, w = s & 127;
    idx = cx * 33 + lo;
    int p = (t[idx] * (128 - w) + t[idx + 1] * w) >> 7;
    if (w >= 64) idx++;
    return p;
  }
  inline void update(int y) {
    int g = y ? 65535 : 0;
    t[idx] += (g - (int)t[idx]) >> rate;
  }
};

struct MatchModel {
  int minLen; std::vector<U32> ht; U32 hmask;
  U64 ptr = 0; int len = 0; bool miss = false; int expBit = 0; int ctxIdx = -1;
  U32 sm[64];
  MatchModel(int minlen, int bits) : minLen(minlen), ht((size_t)1 << bits, 0), hmask((1u << bits) - 1) {
    for (auto& s : sm) s = CTR_INIT;
  }
  static inline int lenBucket(int l) {
    if (l < 16) return l;
    int b = 4; while (b < 16 && (l >> (b + 1))) b++;
    return std::min(31, 12 + b);
  }
  void byteUpdate(const U8* buf, U32 bmask, U64 pos, U32 bufSize) {
    U8 c = buf[(pos - 1) & bmask];
    if (len > 0 && !miss && buf[ptr & bmask] == c) { if (len < 65535) len++; ptr++; }
    else len = 0;
    miss = false;
    if (pos >= (U64)minLen) {
      U32 h = 0;
      for (int i = 1; i <= minLen; i++) h = (h + buf[(pos - i) & bmask] + 1) * 0x2F0B4C27u;
      h = (h ^ (h >> 15)) & hmask;
      if (len == 0) {
        U32 cand = ht[h];
        if (cand > 0 && pos - cand < (U64)bufSize - 256 && cand < pos) {
          U64 c0 = cand; int l = 0;
          while (l < 400 && (U64)l < c0 && buf[(c0 - 1 - l) & bmask] == buf[(pos - 1 - l) & bmask]) l++;
          if (l >= minLen) { len = l; ptr = cand; }
        }
      }
      ht[h] = (U32)pos;
    }
  }
  inline void predict(const U8* buf, U32 bmask, int bitpos, U32 c0, int* x, int& nx) {
    ctxIdx = -1;
    if (len > 0 && !miss) {
      int eb = buf[ptr & bmask];
      if ((U32)((eb | 256) >> (8 - bitpos)) == c0) {
        expBit = (eb >> (7 - bitpos)) & 1;
        ctxIdx = lenBucket(len) * 2 + expBit;
        int st = stretch(ctrP12(sm[ctxIdx]));
        x[nx++] = st;
        x[nx++] = (expBit ? 1 : -1) * std::min(len, 32) * 32;
        return;
      }
      miss = true;
    }
    x[nx++] = 0; x[nx++] = 0;
  }
  inline void update(int y) { if (ctxIdx >= 0) ctrUpdate(sm[ctxIdx], y, 1023); }
  int bucket() const { return (len > 0 && !miss) ? lenBucket(len) : 0; }
};

struct Model {
  static const int NC = 12;
  static const int NI = 48;
  static const int S0 = 64, S1 = 256, S2 = 2048;
  int L; U32 nbMask;
  std::vector<U32> Tmem[NC]; U32* T[NC];
  U32* slot[NC]; U32* slot0[NC];
  U32 ctxh[NC];
  int rbyte[NC], rcnt[NC], ridx[NC];
  U32 rmap[NC][64];
  U32 t0[256];
  std::vector<U8> buf; U32 bmask, bufSize; U64 pos = 0;
  U32 c0 = 1, c4 = 0, c8 = 0; int bitpos = 0, nib = 1;
  U32 w0 = 0, w1 = 0; U64 lineStart = 0, prevLineStart = 0;
  MatchModel mm1, mm2;
  int x[NI]; int nx = 0;
  std::vector<int> W0, W1, W2, WF; int *w0p, *w1p, *w2p, *wfp;
  int pr0 = 2048, pr1 = 2048, pr2 = 2048, prm = 2048; int xf[4];
  APM a1, a2, a3;
  int p16 = 32768;
  int lim;

  explicit Model(int level)
      : L(level), mm1(6, std::max(16, level - 1)), mm2(20, std::max(16, level - 1)),
        a1(65536, G_APMR), a2(65536, G_APMR), a3(8192, G_APMR) {
    nbMask = (1u << (L - 4)) - 1;
    for (int i = 0; i < NC; i++) {
      Tmem[i].assign(((size_t)1 << L) + 16, CTR_INIT);
      U32* p = Tmem[i].data(); while (((uintptr_t)p & 63) != 0) p++;
      T[i] = p; for (size_t k = 0; k < ((size_t)1 << L); k += 16) T[i][k] = 0;
    }
    int bl = std::min(30, std::max(18, L + 3));
    bufSize = 1u << bl; bmask = bufSize - 1; buf.assign(bufSize, 0);
    for (auto& t : t0) t = CTR_INIT;
    for (int i = 0; i < NC; i++) for (auto& r : rmap[i]) r = CTR_INIT;
    W0.assign((size_t)S0 * NI, 0); W1.assign((size_t)S1 * NI, 0); W2.assign((size_t)S2 * NI, 0);
    auto initW = [](std::vector<int>& W, int sets) {
      for (int s = 0; s < sets; s++) for (int j = 0; j < NI; j++) W[(size_t)s * NI + j] = (j >= 3 && j < 3 + NC * 3 && (j - 3) % 3 == 0) ? (1 << 14) : 0;
    };
    initW(W0, S0); initW(W1, S1); initW(W2, S2);
    WF.assign(256 * 4, 0);
    for (int s = 0; s < 256; s++) { WF[s * 4 + 0] = WF[s * 4 + 1] = WF[s * 4 + 2] = 65536 / 3; }
    lim = G_LIM;
    for (int i = 0; i < NC; i++) ctxh[i] = 0;
    computeContexts();
    predict();
  }

  inline size_t bucketOf(U32 h) const { return (size_t)((h >> (32 - (L - 4))) & nbMask) << 4; }
  inline void prefetch(U32* t, U32 h) const {
    size_t i = bucketOf(h);
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(&t[i]); __builtin_prefetch(&t[i ^ 16]);
#elif defined(_MSC_VER)
    _mm_prefetch((const char*)&t[i], _MM_HINT_T0); _mm_prefetch((const char*)&t[i ^ 16], _MM_HINT_T0);
#endif
  }
  inline U32* find(U32* t, U32 h) {
    U32 chk = (h * 0x2545F491u) >> 16;
    size_t i = bucketOf(h);
    U32* a = &t[i]; if ((a[0] & 0xffff) == chk) return a;
    U32* b = &t[i ^ 16]; if ((b[0] & 0xffff) == chk) return b;
    U32* r = ((a[1] & 1023) <= (b[1] & 1023)) ? a : b;
    r[0] = chk; for (int k = 1; k < 16; k++) r[k] = CTR_INIT;
    return r;
  }

  void computeContexts() {
    U32 c = c4 & 255;
    U64 col = pos - lineStart;
    U32 above = (prevLineStart + col < lineStart) ? buf[(prevLineStart + col) & bmask] : 0;
    ctxh[0] = hash2(1, c4 & 0xff);
    ctxh[1] = hash2(2, c4 & 0xffff);
    ctxh[2] = hash2(3, c4 & 0xffffff);
    ctxh[3] = hash2(4, c4);
    ctxh[4] = hash3(5, c4, c8 & 0xff);
    ctxh[5] = hash3(6, c4, c8 & 0xffff);
    ctxh[6] = hash3(7, c4, c8);
    ctxh[7] = hash3(8, w0, w1 + (w0 ? 0 : c * 0x10001u));
    ctxh[8] = hash2(9, c4 & 0xffff00);
    ctxh[9] = hash3(10, above | (c << 8), (U32)std::min<U64>(col, 255));
    ctxh[10] = hash2(11, c4 & 0xffff0000);
    ctxh[11] = hash3(12, w0, c);
    for (int i = 0; i < NC; i++) prefetch(T[i], ctxh[i]);
    for (int i = 0; i < NC; i++) {
      slot[i] = slot0[i] = find(T[i], ctxh[i]);
      U32 r = slot0[i][0] >> 16; rbyte[i] = r >> 8; rcnt[i] = r & 255;
    }
  }

  inline void addCtr(U32 t) {
    int p = ctrP12(t);
    x[nx++] = stretch(p);
    x[nx++] = (G_X1 && (t & 1023)) ? (p - 2048) >> 1 : 0;
  }

  static inline int dot(const int* x, const int* w, int n) {
    int s = 0; for (int i = 0; i < n; i++) s += (x[i] * w[i]) >> 8;
    s >>= 8; if (s > 2047) s = 2047; if (s < -2047) s = -2047; return s;
  }
  static inline void train(const int* x, int* w, int n, int err) {
    for (int i = 0; i < n; i++) { int v = w[i] + ((x[i] * err + 8192) >> 14); v = v > 524287 ? 524287 : v; v = v < -524288 ? -524288 : v; w[i] = v; }
  }
#if defined(__AVX2__)
  static inline int dotV(const int* x, const int* w) {
    __m256i s = _mm256_setzero_si256();
    for (int i = 0; i < NI; i += 8) {
      __m256i p = _mm256_mullo_epi32(_mm256_loadu_si256((const __m256i*)(x + i)), _mm256_loadu_si256((const __m256i*)(w + i)));
      s = _mm256_add_epi32(s, _mm256_srai_epi32(p, 8));
    }
    __m128i t = _mm_add_epi32(_mm256_castsi256_si128(s), _mm256_extracti128_si256(s, 1));
    t = _mm_add_epi32(t, _mm_shuffle_epi32(t, 0x4E));
    t = _mm_add_epi32(t, _mm_shuffle_epi32(t, 0xB1));
    int r = _mm_cvtsi128_si32(t) >> 8;
    return r > 2047 ? 2047 : r < -2047 ? -2047 : r;
  }
  static inline void trainV(const int* x, int* w, int err) {
    const __m256i e = _mm256_set1_epi32(err), rnd = _mm256_set1_epi32(8192);
    const __m256i hi = _mm256_set1_epi32(524287), lo = _mm256_set1_epi32(-524288);
    for (int i = 0; i < NI; i += 8) {
      __m256i d = _mm256_srai_epi32(_mm256_add_epi32(_mm256_mullo_epi32(_mm256_loadu_si256((const __m256i*)(x + i)), e), rnd), 14);
      __m256i v = _mm256_add_epi32(_mm256_loadu_si256((const __m256i*)(w + i)), d);
      _mm256_storeu_si256((__m256i*)(w + i), _mm256_max_epi32(_mm256_min_epi32(v, hi), lo));
    }
  }
#else
  static inline int dotV(const int* x, const int* w) { return dot(x, w, NI); }
  static inline void trainV(const int* x, int* w, int err) { train(x, w, NI, err); }
#endif

  void predict() {
    nx = 0;
    x[nx++] = 256;
    addCtr(t0[c0]);
    for (int i = 0; i < NC; i++) {
      addCtr(slot[i][nib]);
      ridx[i] = -1;
      if (rcnt[i] && (U32)((rbyte[i] | 256) >> (8 - bitpos)) == c0) {
        int e = (rbyte[i] >> (7 - bitpos)) & 1;
        ridx[i] = std::min(rcnt[i], 31) * 2 + e;
        x[nx++] = stretch(ctrP12(rmap[i][ridx[i]]));
      } else x[nx++] = 0;
    }
    mm1.predict(buf.data(), bmask, bitpos, c0, x, nx);
    mm2.predict(buf.data(), bmask, bitpos, c0, x, nx);
    while (nx < NI) x[nx++] = 0;

    int mb = mm1.bucket();
    int s0 = std::min(mb, 15) * 4 + (mm2.bucket() > 0 ? 2 : 0) + (bitpos == 0 ? 1 : 0);
    int s1 = c0;
    int s2 = ((c4 & 255) << 3) | bitpos;
    w0p = &W0[(size_t)s0 * NI]; w1p = &W1[(size_t)s1 * NI]; w2p = &W2[(size_t)s2 * NI];
    pr0 = squash(dotV(x, w0p)); pr1 = squash(dotV(x, w1p)); pr2 = squash(dotV(x, w2p));
    xf[0] = stretch(pr0); xf[1] = stretch(pr1); xf[2] = stretch(pr2); xf[3] = 256;
    wfp = &WF[c0 * 4];
    prm = squash(dot(xf, wfp, 4));

    U32 c1 = c4 & 255;
    int q1 = a1.pp(prm, (int)(c0 | (c1 << 8)));
    int q2 = a2.pp(prm, (int)((hash2(c4 & 0xffff, 77) ^ c0) & 0xffff));
    int q3 = a3.pp(prm, (int)(c0 | (std::min(mb, 31) << 8)));
    int p;
    switch (G_FM) {
      case 1: p = (prm * 16 + q1 + q2 + q3) / 4; break;
      case 2: p = (prm * 16 + q1 * 2 + q2 * 2 + q3 * 3) / 8; break;
      case 3: p = (q1 + q2 + q3 * 2) / 4; break;
      default: p = (prm * 16 * 2 + q1 + q2 + q3 * 2) / 6;
    }
    if (p < 1) p = 1;
    if (p > 65535) p = 65535;
    p16 = p;
  }

  void update(int y) {
    ctrUpdate(t0[c0], y, lim);
    for (int i = 0; i < NC; i++) {
      ctrUpdate(slot[i][nib], y, lim);
      if (ridx[i] >= 0) ctrUpdate(rmap[i][ridx[i]], y, 1023);
    }
    mm1.update(y); mm2.update(y);
    int lr = G_LR + (pos < (1u << 22) ? (int)((G_LRB * 65536ull) / (pos + 65536)) : 0);
    int e0 = ((y << 12) - pr0) * lr, e1 = ((y << 12) - pr1) * lr, e2 = ((y << 12) - pr2) * lr;
    if (std::abs(e0) > G_SKIP * lr) trainV(x, w0p, e0);
    if (std::abs(e1) > G_SKIP * lr) trainV(x, w1p, e1);
    if (std::abs(e2) > G_SKIP * lr) trainV(x, w2p, e2);
    train(xf, wfp, 4, ((y << 12) - prm) * G_LR2);
    a1.update(y); a2.update(y); a3.update(y);

    c0 = (c0 << 1) | y; nib = (nib << 1) | y; bitpos++;
    if (bitpos == 8) byteUpdate();
    else if (bitpos == 4) {
      U32 hh[NC];
      for (int i = 0; i < NC; i++) { hh[i] = hash2(ctxh[i], c0); prefetch(T[i], hh[i]); }
      for (int i = 0; i < NC; i++) slot[i] = find(T[i], hh[i]);
      nib = 1;
    }
    predict();
  }

  void byteUpdate() {
    U32 c = c0 & 255;
    buf[pos & bmask] = (U8)c; pos++;
    c8 = (c8 << 8) | (c4 >> 24); c4 = (c4 << 8) | c;
    for (int i = 0; i < NC; i++) {
      U32 r = slot0[i][0] >> 16; U32 rb = r >> 8, rc = r & 255;
      if (rc == 0 || rb != c) { rb = c; rc = 1; } else if (rc < 255) rc++;
      slot0[i][0] = (slot0[i][0] & 0xffff) | (((rb << 8) | rc) << 16);
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 128) {
      U32 lc = (c >= 'A' && c <= 'Z') ? c + 32 : c;
      w0 = (w0 + lc + 1) * 0x3D4D51CBu;
    } else if (w0) { w1 = w0; w0 = 0; }
    if (c == '\n') { prevLineStart = lineStart; lineStart = pos; }
    mm1.byteUpdate(buf.data(), bmask, pos, bufSize);
    mm2.byteUpdate(buf.data(), bmask, pos, bufSize);
    c0 = 1; bitpos = 0; nib = 1;
    computeContexts();
  }
};

struct Encoder {
  Model m; OutBuf& out; U32 x1 = 0, x2 = 0xffffffff;
  Encoder(int level, OutBuf& o) : m(level), out(o) {}
  inline void bit(int y) {
    U32 xmid = x1 + (U32)(((U64)(x2 - x1) * (U32)m.p16) >> 16);
    if (y) x2 = xmid; else x1 = xmid + 1;
    m.update(y);
    while (((x1 ^ x2) & 0xff000000) == 0) { out.put((U8)(x2 >> 24)); x1 <<= 8; x2 = (x2 << 8) | 255; }
  }
  inline void byte(int c) { for (int i = 7; i >= 0; i--) bit((c >> i) & 1); }
  void flush() { out.put((U8)(x1 >> 24)); out.put((U8)(x1 >> 16)); out.put((U8)(x1 >> 8)); out.put((U8)x1); }
};
struct Decoder {
  Model m; InBuf& in; U32 x1 = 0, x2 = 0xffffffff, x = 0;
  Decoder(int level, InBuf& i) : m(level), in(i) { for (int k = 0; k < 4; k++) x = (x << 8) | (U32)in.get(); }
  inline int bit() {
    U32 xmid = x1 + (U32)(((U64)(x2 - x1) * (U32)m.p16) >> 16);
    int y = x <= xmid;
    if (y) x2 = xmid; else x1 = xmid + 1;
    m.update(y);
    while (((x1 ^ x2) & 0xff000000) == 0) { x1 <<= 8; x2 = (x2 << 8) | 255; x = (x << 8) | (U32)in.get(); }
    return y;
  }
  inline int byte() { int c = 0; for (int i = 0; i < 8; i++) c = (c << 1) | bit(); return c; }
};

#include "crypto.h"
#include <cctype>

static const char MAGIC[4] = {'K', 'C', 'X', '1'};
static const U32 ITER = 200000;
static const U32 CHUNK = 1u << 20;
static const size_t HDR = 30, TAG = 16;

static void put32(U8* p, U32 v) { for (int i = 0; i < 4; i++) p[i] = (U8)(v >> (8 * i)); }
static U32 get32(const U8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((U32)p[3] << 24); }

bool isEncryptedArchive(const fs::path& p) {
  FILE* f = openFile(p, 0); if (!f) return false;
  char m[4]; bool r = fread(m, 1, 4, f) == 4 && memcmp(m, MAGIC, 4) == 0;
  fclose(f); return r;
}

std::string normalizePassword(const std::string& pw) {
  std::string r;
  for (char c : pw) if (std::isalnum((unsigned char)c)) r += (char)std::toupper((unsigned char)c);
  return r;
}

#ifdef _WIN32
#include "winapi.h"
#include <bcrypt.h>
#ifdef _MSC_VER
#pragma comment(lib, "bcrypt.lib")
#endif
static const NTSTATUS TAG_MISMATCH = (NTSTATUS)0xC000A002L;

static void check(NTSTATUS s, const char* what) {
  if (!BCRYPT_SUCCESS(s)) throw CKError(std::string("Verschlüsselung: ") + what + " fehlgeschlagen");
}
static void randomBytes(U8* p, size_t n) {
  check(BCryptGenRandom(NULL, p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG), "Zufallszahlen");
}

std::string makePassword() {
  static const char A[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  U8 r[25]; randomBytes(r, sizeof(r));
  std::string s;
  for (int i = 0; i < 25; i++) { if (i && i % 5 == 0) s += '-'; s += A[r[i] & 31]; }
  SecureZeroMemory(r, sizeof(r));
  return s;
}

struct Cipher {
  BCRYPT_ALG_HANDLE alg = NULL; BCRYPT_KEY_HANDLE key = NULL;
  Cipher(const std::string& pw, const U8* salt, U32 iter) {
    try { init(pw, salt, iter); } catch (...) { free(); throw; }
  }
  ~Cipher() { free(); }
  void init(const std::string& pw, const U8* salt, U32 iter) {
    std::string n = normalizePassword(pw);
    U8 k[32];
    BCRYPT_ALG_HANDLE h = NULL;
    check(BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG), "SHA-256");
    NTSTATUS s = BCryptDeriveKeyPBKDF2(h, (PUCHAR)n.data(), (ULONG)n.size(), (PUCHAR)salt, 16, iter, k, 32, 0);
    BCryptCloseAlgorithmProvider(h, 0);
    if (!n.empty()) SecureZeroMemory(&n[0], n.size());
    check(s, "PBKDF2");
    check(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0), "AES");
    check(BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0), "GCM");
    s = BCryptGenerateSymmetricKey(alg, &key, NULL, 0, k, 32, 0);
    SecureZeroMemory(k, sizeof(k));
    check(s, "Schlüssel");
  }
  void free() {
    if (key) { BCryptDestroyKey(key); key = NULL; }
    if (alg) { BCryptCloseAlgorithmProvider(alg, 0); alg = NULL; }
  }
  NTSTATUS block(bool enc, const U8* hdr, U64 idx, bool last, const U8* in, ULONG n, U8* out, U8* tag) {
    U8 nonce[12] = {0}, aad[HDR + 9];
    for (int i = 0; i < 8; i++) nonce[i] = (U8)(idx >> (8 * i));
    memcpy(aad, hdr, HDR);
    for (int i = 0; i < 8; i++) aad[HDR + i] = (U8)(idx >> (8 * i));
    aad[HDR + 8] = last ? 1 : 0;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO ai; BCRYPT_INIT_AUTH_MODE_INFO(ai);
    ai.pbNonce = nonce; ai.cbNonce = sizeof(nonce);
    ai.pbAuthData = aad; ai.cbAuthData = sizeof(aad);
    ai.pbTag = tag; ai.cbTag = TAG;
    ULONG got = 0;
    return enc ? BCryptEncrypt(key, (PUCHAR)in, n, &ai, NULL, 0, out, n, &got, 0)
               : BCryptDecrypt(key, (PUCHAR)in, n, &ai, NULL, 0, out, n, &got, 0);
  }
};

static const char* WRITE_ERR = "Schreibfehler (Festplatte voll?)";

void encryptFile(const fs::path& in, const fs::path& out, const std::string& pw, Progress& prog) {
  std::error_code ec;
  U64 size = fs::file_size(in, ec);
  if (ec) throw CKError("Kann Datei nicht lesen: " + u8(in));
  U8 hdr[HDR];
  memcpy(hdr, MAGIC, 4); hdr[4] = 1; hdr[5] = 1;
  put32(hdr + 6, ITER); randomBytes(hdr + 10, 16); put32(hdr + 26, CHUNK);
  prog.start("Verschlüssele", size);
  Cipher c(pw, hdr + 10, ITER);

  fs::path tmp = out; tmp += ".part";
  FILE* fi = openFile(in, 0);
  if (!fi) throw CKError("Kann Datei nicht lesen: " + u8(in));
  FILE* fo = openFile(tmp, 1);
  if (!fo) { fclose(fi); throw CKError("Kann Datei nicht anlegen: " + u8(out)); }
  std::vector<U8> a(CHUNK), b(CHUNK);
  try {
    if (fwrite(hdr, 1, HDR, fo) != HDR) throw CKError(WRITE_ERR);
    U64 nblocks = std::max<U64>(1, (size + CHUNK - 1) / CHUNK);
    for (U64 i = 0; i < nblocks; i++) {
      ULONG n = (ULONG)std::min<U64>(CHUNK, size - i * CHUNK);
      if (fread(a.data(), 1, n, fi) != n) throw CKError("Lesefehler: " + u8(in));
      U8 tag[TAG];
      check(c.block(true, hdr, i, i + 1 == nblocks, a.data(), n, b.data(), tag), "AES-GCM");
      if (fwrite(b.data(), 1, n, fo) != n || fwrite(tag, 1, TAG, fo) != TAG) throw CKError(WRITE_ERR);
      prog.add(n);
    }
    SecureZeroMemory(a.data(), a.size());
    fclose(fi); fi = nullptr;
    if (fclose(fo) != 0) { fo = nullptr; throw CKError(WRITE_ERR); }
    fo = nullptr;
    fs::remove(out, ec);
    fs::rename(tmp, out);
  } catch (...) {
    SecureZeroMemory(a.data(), a.size());
    if (fi) fclose(fi);
    if (fo) fclose(fo);
    fs::remove(tmp, ec);
    throw;
  }
}

void decryptFile(const fs::path& in, const fs::path& out, const std::string& pw, Progress& prog) {
  std::error_code ec;
  U64 total = fs::file_size(in, ec);
  if (ec) throw CKError("Kann Datei nicht lesen: " + u8(in));
  FILE* fi = openFile(in, 0);
  if (!fi) throw CKError("Kann Datei nicht lesen: " + u8(in));
  U8 hdr[HDR];
  if (total < HDR + TAG || fread(hdr, 1, HDR, fi) != HDR || memcmp(hdr, MAGIC, 4) != 0) { fclose(fi); throw CKError("Keine verschlüsselte Klimax-Compression-Datei"); }
  if (hdr[4] != 1 || hdr[5] != 1) { fclose(fi); throw CKError("Verschlüsselung von einer neueren Version. Bitte aktualisieren."); }
  U32 iter = get32(hdr + 6), chunk = get32(hdr + 26);
  if (iter < 1000 || iter > 10000000 || chunk < 4096 || chunk > (64u << 20)) { fclose(fi); throw CKError("Archiv beschädigt (Kopf)"); }
  prog.start("Entschlüssele", total - HDR);
  std::unique_ptr<Cipher> c;
  try { c.reset(new Cipher(pw, hdr + 10, iter)); } catch (...) { fclose(fi); throw; }

  fs::path tmp = out; tmp += ".part";
  FILE* fo = openFile(tmp, 1);
  if (!fo) { fclose(fi); throw CKError("Kann temporäre Datei nicht anlegen"); }
  std::vector<U8> a(chunk), b(chunk);
  try {
    U64 left = total - HDR;
    for (U64 i = 0; left; i++) {
      U64 rec = std::min<U64>(left, (U64)chunk + TAG);
      if (rec < TAG) throw CKError("Archiv beschädigt (abgeschnitten)");
      ULONG n = (ULONG)(rec - TAG);
      U8 tag[TAG];
      if (fread(a.data(), 1, n, fi) != n || fread(tag, 1, TAG, fi) != TAG) throw CKError("Lesefehler: " + u8(in));
      NTSTATUS s = c->block(false, hdr, i, rec == left, a.data(), n, b.data(), tag);
      if (s == TAG_MISMATCH) {
        if (i == 0) throw CKWrongPassword();
        throw CKError("Archiv beschädigt oder manipuliert (Block " + std::to_string(i + 1) + ")");
      }
      check(s, "AES-GCM");
      if (fwrite(b.data(), 1, n, fo) != n) throw CKError(WRITE_ERR);
      left -= rec; prog.add(rec);
    }
    SecureZeroMemory(b.data(), b.size());
    fclose(fi); fi = nullptr;
    if (fclose(fo) != 0) { fo = nullptr; throw CKError(WRITE_ERR); }
    fo = nullptr;
    fs::remove(out, ec);
    fs::rename(tmp, out);
  } catch (...) {
    SecureZeroMemory(b.data(), b.size());
    if (fi) fclose(fi);
    if (fo) fclose(fo);
    fs::remove(tmp, ec);
    throw;
  }
}

#else
static CKError noCrypto() { return CKError("Geheim-Modus gibt's nur unter Windows"); }
std::string makePassword() { throw noCrypto(); }
void encryptFile(const fs::path&, const fs::path&, const std::string&, Progress&) { throw noCrypto(); }
void decryptFile(const fs::path&, const fs::path&, const std::string&, Progress&) { throw noCrypto(); }
#endif

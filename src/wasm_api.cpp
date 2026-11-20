#include "archive.h"
#include <emscripten.h>

EM_JS(void, js_progress, (const char* phase, double done, double total), {
  if (Module["onProgress"]) Module["onProgress"](UTF8ToString(phase), done, total);
});

static std::string gError, gInfo;

static std::string jsonStr(const std::string& v) {
  std::string r = "\"";
  for (unsigned char c : v) {
    if (c == '"' || c == '\\') { r += '\\'; r += (char)c; }
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); r += b; }
    else r += (char)c;
  }
  return r + "\"";
}

static Progress makeProgress() {
  Progress p;
  p.cb = [](const std::string& ph, U64 d, U64 t) { js_progress(ph.c_str(), (double)d, (double)t); };
  return p;
}

extern "C" {

EMSCRIPTEN_KEEPALIVE void kc_init() { ckInit(); }

EMSCRIPTEN_KEEPALIVE const char* kc_error() { return gError.c_str(); }
EMSCRIPTEN_KEEPALIVE const char* kc_info() { return gInfo.c_str(); }

EMSCRIPTEN_KEEPALIVE int kc_compress(const char* input, const char* output, int mode) {
  gError.clear(); gInfo.clear();
  try {
    Progress p = makeProgress();
    CompressResult r = compressPath(fs::path(input), fs::path(output), p, 1, (CkMode)std::max(0, std::min(2, mode)));
    gInfo = "{\"inBytes\":" + std::to_string(r.inBytes) + ",\"outBytes\":" + std::to_string(r.outBytes) +
            ",\"files\":" + std::to_string(r.files) + ",\"dups\":" + std::to_string(r.dups) +
            ",\"stored\":" + std::to_string(r.stored) + "}";
    return 0;
  } catch (const std::bad_alloc&) {
    gError = "Zu wenig Arbeitsspeicher im Browser. Kleinere Auswahl oder Modus Schnell probieren.";
  } catch (const std::exception& e) {
    gError = e.what();
  }
  return 1;
}

EMSCRIPTEN_KEEPALIVE int kc_peek(const char* archive) {
  gError.clear(); gInfo.clear();
  try {
    ArchiveInfo ai = readArchiveInfo(fs::path(archive));
    U64 files = 0; for (auto& e : ai.ents) if (!e.dir) files++;
    gInfo = "{\"totalSize\":" + std::to_string(ai.totalSize) + ",\"files\":" + std::to_string(files) + "}";
    return 0;
  } catch (const std::exception& e) {
    gError = e.what();
  }
  return 1;
}

EMSCRIPTEN_KEEPALIVE int kc_extract(const char* archive, const char* outDir) {
  gError.clear(); gInfo.clear();
  try {
    Progress p = makeProgress();
    ArchiveInfo ai = readArchiveInfo(fs::path(archive));
    ExtractResult r = extractArchive(fs::path(archive), fs::path(outDir), p);
    gInfo = "{\"files\":" + std::to_string(r.files) + ",\"bytes\":" + std::to_string(r.bytes) +
            ",\"crcErrors\":" + std::to_string(r.crcErrors) + ",\"archiveSize\":" + std::to_string(ai.archiveSize) +
            ",\"codec\":" + jsonStr(codecName(ai)) + "}";
    return 0;
  } catch (const std::bad_alloc&) {
    gError = "Zu wenig Arbeitsspeicher im Browser, das Archiv ist zu groß dafür.";
  } catch (const std::exception& e) {
    gError = e.what();
  }
  return 1;
}

}

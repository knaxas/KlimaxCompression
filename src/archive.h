#pragma once
#include "core.h"

enum : U8 { M_SOLID = 0, M_STORED = 1, M_DUP = 2 };
enum : U8 { F_NONE = 0, F_E8E9 = 1 };

enum CkMode : int { MODE_FAST = 0, MODE_BALANCED = 1, MODE_MAX = 2 };

struct Entry {
  std::string path; bool dir = false;
  U64 size = 0, mtime = 0; U32 attr = 0;
  U8 method = M_SOLID, filter = F_NONE; U32 crc = 0; U64 h64 = 0; U32 dupOf = 0;
  fs::path src;
};

struct CompressResult { U64 inBytes = 0, outBytes = 0, files = 0, dups = 0, stored = 0; int segments = 0; fs::path out; };
struct ExtractResult  { U64 files = 0, bytes = 0; fs::path root; fs::path firstFile; int crcErrors = 0; };

struct ArchiveInfo {
  std::vector<Entry> ents;
  int version = 1;
  U64 S = 0; int nseg = 0;
  std::vector<U64> segRaw, segComp, segOff; std::vector<int> segLevel, segCodec;
  U64 storedOff = 0, archiveSize = 0, totalSize = 0;
};

void ckInit();

CompressResult compressPath(fs::path input, const fs::path& outFile, Progress& prog, int maxThreads = 0, CkMode mode = MODE_MAX);
ArchiveInfo readArchiveInfo(const fs::path& archive);
std::string codecName(const ArchiveInfo& ai);

ExtractResult extractArchive(const fs::path& archive, const fs::path& outDir, Progress& prog,
                             const std::vector<char>* want = nullptr, const std::string& prefix = "");

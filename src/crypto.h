#pragma once
#include "core.h"

struct CKWrongPassword : CKError {
  CKWrongPassword() : CKError("Falsches Passwort (oder die Datei ist beschädigt)") {}
};

bool isEncryptedArchive(const fs::path& p);
std::string makePassword();
std::string normalizePassword(const std::string& pw);
void encryptFile(const fs::path& in, const fs::path& out, const std::string& pw, Progress& prog);
void decryptFile(const fs::path& in, const fs::path& out, const std::string& pw, Progress& prog);

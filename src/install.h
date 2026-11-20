#pragma once
#include "core.h"

static const wchar_t* const SETTINGS_KEY = L"Software\\Klimax Compression";

fs::path installDir();
bool isInstalled();
bool runningInstalled();
bool installedIsOlder();
bool legacyInstalled();
void doInstall();
void doUninstall();

#pragma once
#ifndef UNICODE
  #define UNICODE
#endif
#ifndef _UNICODE
  #define _UNICODE
#endif
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#ifndef _WIN32_WINNT
  #define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
  #define WINVER 0x0A00
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <dwmapi.h>

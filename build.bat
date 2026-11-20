@echo off
setlocal
cd /d "%~dp0"

set SRC=src\main.cpp src\core.cpp src\archive.cpp src\crypto.cpp src\install.cpp src\ui_theme.cpp src\ui_main.cpp src\ui_viewer.cpp src\ui_secret.cpp
set ZDIRS=third_party\zstd\lib\common third_party\zstd\lib\compress third_party\zstd\lib\decompress
set ZDEF=-DZSTD_DISABLE_ASM -DZSTD_LEGACY_SUPPORT=0

where cl >nul 2>nul
if %errorlevel%==0 goto msvc

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" (
      call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul
      goto msvc
    )
  )
)

where g++ >nul 2>nul
if %errorlevel%==0 goto mingw

echo.
echo  Kein C++-Compiler gefunden!
echo  Installiere EINEN davon:
echo   - "Build Tools fuer Visual Studio" (kostenlos, Workload "Desktopentwicklung mit C++")
echo     https://visualstudio.microsoft.com/de/visual-cpp-build-tools/
echo   - oder MinGW-w64 (z.B. via https://winlibs.com)
echo.
pause
exit /b 1

:msvc
echo [MSVC] Baue KlimaxCompression.exe ...
if not exist build\msvc_zstd mkdir build\msvc_zstd
if not exist build\msvc_zstd\zstd_compress.obj (
  echo   zstd wird einmalig gebaut ...
  for %%d in (%ZDIRS%) do (
    cl /nologo /c /O2 /GL /MT /arch:AVX2 /DNDEBUG /w %ZDEF% /Fobuild\msvc_zstd\ %%d\*.c >nul || goto fail
  )
)
rc /nologo /fo build\ck.res ck.rc || goto fail
cl /nologo /O2 /GL /Oi /EHsc /std:c++17 /utf-8 /MT /arch:AVX2 /DNDEBUG /W3 /Fobuild\ %SRC% build\msvc_zstd\*.obj build\ck.res /Fe:KlimaxCompression.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /LTCG || goto fail
goto ok

:mingw
echo [MinGW] Baue KlimaxCompression.exe ...
if not exist build\mingw_zstd mkdir build\mingw_zstd
if not exist build\mingw_zstd\zstd.rsp (
  echo   zstd wird einmalig gebaut ...
  for %%d in (%ZDIRS%) do (
    for %%f in (%%d\*.c) do (
      gcc -O3 -march=x86-64-v3 -DNDEBUG %ZDEF% -c %%f -o build\mingw_zstd\%%~nf.o || goto fail
      echo build/mingw_zstd/%%~nf.o>> build\mingw_zstd\zstd.tmp
    )
  )
  move /y build\mingw_zstd\zstd.tmp build\mingw_zstd\zstd.rsp >nul
)
windres ck.rc -O coff -o build\ck_res.o || goto fail
g++ -O3 -march=x86-64-v3 -std=c++17 -municode -mwindows -static -s %SRC% @build/mingw_zstd/zstd.rsp build\ck_res.o -o KlimaxCompression.exe -lcomctl32 -lshell32 -lgdi32 -luser32 -ladvapi32 -lole32 -luuid -luxtheme -ldwmapi -lbcrypt || goto fail
goto ok

:ok
echo.
echo  Fertig:  %cd%\KlimaxCompression.exe
echo.
if "%1"=="" pause
exit /b 0

:fail
echo.
echo  Build fehlgeschlagen.
if "%1"=="" pause
exit /b 1

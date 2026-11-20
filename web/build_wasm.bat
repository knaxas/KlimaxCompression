@echo off
setlocal
cd /d "%~dp0.."

if "%EMSDK%"=="" set "EMSDK=%LOCALAPPDATA%\emsdk"
if not exist "%EMSDK%\emsdk_env.bat" (
  echo.
  echo  Emscripten nicht gefunden. Einmalig installieren:
  echo    git clone https://github.com/emscripten-core/emsdk.git "%LOCALAPPDATA%\emsdk"
  echo    cd /d "%LOCALAPPDATA%\emsdk"
  echo    emsdk install latest
  echo    emsdk activate latest
  echo.
  if "%1"=="" pause
  exit /b 1
)
call "%EMSDK%\emsdk_env.bat" >nul 2>nul

if not exist build\wasm_zstd mkdir build\wasm_zstd
if not exist build\wasm_zstd\zstd.rsp (
  echo [wasm] zstd wird einmalig gebaut ...
  if exist build\wasm_zstd\zstd.tmp del build\wasm_zstd\zstd.tmp
  for %%d in (common compress decompress) do (
    for %%f in (third_party\zstd\lib\%%d\*.c) do (
      call emcc -O3 -DNDEBUG -DZSTD_DISABLE_ASM -DZSTD_LEGACY_SUPPORT=0 -c %%f -o build\wasm_zstd\%%~nf.o || goto fail
      echo build/wasm_zstd/%%~nf.o>> build\wasm_zstd\zstd.tmp
    )
  )
  move /y build\wasm_zstd\zstd.tmp build\wasm_zstd\zstd.rsp >nul
)

echo [wasm] Baue web\kc_engine.js ...
call em++ -O3 -std=c++17 -fwasm-exceptions -DNDEBUG ^
  src\wasm_api.cpp src\core.cpp src\archive.cpp @build/wasm_zstd/zstd.rsp ^
  -sMODULARIZE=1 -sEXPORT_NAME=KCModule -sSINGLE_FILE=1 -sENVIRONMENT=worker,node ^
  -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=8MB -sFORCE_FILESYSTEM=1 ^
  -sEXPORTED_FUNCTIONS=_kc_init,_kc_compress,_kc_extract,_kc_peek,_kc_error,_kc_info ^
  -sEXPORTED_RUNTIME_METHODS=FS,ccall,UTF8ToString ^
  --extern-pre-js web\src\pre.js --extern-post-js web\src\post.js ^
  -o web\kc_engine.js || goto fail

echo.
echo  Fertig:  %cd%\web\kc_engine.js
echo.
if "%1"=="" pause
exit /b 0

:fail
echo.
echo  Build fehlgeschlagen.
if "%1"=="" pause
exit /b 1

@echo off
setlocal enabledelayedexpansion

echo Setting environment...
rem Favor VS2022 over VS2026 for now.
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
  for /f "usebackq tokens=*" %%i in (`call "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -version "[17, 18)" -latest -property installationPath`) do set "VSINSTPATH=%%i"
  if defined VSINSTPATH (
    echo VSINSTPATH=!VSINSTPATH!
    call "!VSINSTPATH!\VC\Auxiliary\Build\vcvarsall.bat" x64 uwp || goto error
  ) else (
    for /f "usebackq tokens=*" %%i in (`call "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -version "[18, 19)" -latest -property installationPath`) do set "VSINSTPATH=%%i"
    if defined VSINSTPATH (
      echo VSINSTPATH=!VSINSTPATH!
      call "!VSINSTPATH!\VC\Auxiliary\Build\vcvarsall.bat" x64 uwp || goto error
    ) else (
      echo Visual Studio not found.
      goto error
    )
  )
) else (
  echo Visual Studio not found.
  goto error
)
echo VSCMD_ARG_APP_PLAT=%VSCMD_ARG_APP_PLAT%
if /I "%VSCMD_ARG_APP_PLAT%"=="Desktop" (
  echo vcvarsall did not enter UWP/store mode. Aborting.
  goto error
)

set SEVENZIP="C:\Program Files\7-Zip\7z.exe"
set PATCH="C:\Program Files\Git\usr\bin\patch.exe"
set GIT="C:\Program Files\Git\bin\git.exe"
rem We only expect people to be running this software on Xbox One/Series X|S in Developer mode or on Windows UWP.
set "UWP_CMAKE_FLAGS=-DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0 -DCMAKE_SYSTEM_PROCESSOR=AMD64 -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY"

if defined DEBUG (
  echo DEBUG=%DEBUG%
) else (
  set DEBUG=1
)

set FORCEPDB=-DCMAKE_SHARED_LINKER_FLAGS_RELEASE="/DEBUG" -DCMAKE_MODULE_LINKER_FLAGS_RELEASE="/DEBUG" -DCMAKE_SHARED_LINKER_FLAGS_MINSIZEREL="/DEBUG" -DCMAKE_MODULE_LINKER_FLAGS_MINSIZEREL="/DEBUG"

pushd %~dp0
set "SCRIPTDIR=%CD%"
cd ..\..\..\..
mkdir deps-build
cd deps-build || goto error
set "BUILDDIR=%CD%"
cd ..
mkdir deps
cd deps || goto error
set "INSTALLDIR=%CD%"
popd

echo SCRIPTDIR=%SCRIPTDIR%
echo BUILDDIR=%BUILDDIR%
echo INSTALLDIR=%INSTALLDIR%

set "PATH=%PATH%;%INSTALLDIR%\bin"

cd "%BUILDDIR%"

set FREETYPE=2.14.3
set HARFBUZZ=14.2.0
set LIBJPEGTURBO=3.2.0
set LIBPNG=1658
set LIBPNGLONG=1.6.58
set SDL=uwp-3.4.12
set LZ4=1.10.0
set WEBP=1.6.0
set ZLIB=1.3.2
set ZLIBSHORT=132
set ZSTD=1.5.7
set PLUTOVG=1.3.2
set PLUTOSVG=0.0.7
set RAPIDYAML=0.12.1
set DXHEADERS=1.619.1

set SHADERC=2026.2
set SHADERC_GLSLANG=275822a6261ee689aadb1da5f09a0ec2f058685c
set SHADERC_SPIRVHEADERS=58006c901d1d5c37dece6b6610e9af87fa951375
set SHADERC_SPIRVTOOLS=6337eb62cadd7d124ac6789bf39c0f71148f0a73

call :downloadfile "freetype-%FREETYPE%.tar.gz" https://sourceforge.net/projects/freetype/files/freetype2/%FREETYPE%/freetype-%FREETYPE%.tar.gz/download e61b31ab26358b946e767ed7eb7f4bb2e507da1cfefeb7a8861ace7fd5c899a1 || goto error
call :downloadfile "harfbuzz-%HARFBUZZ%.zip" https://github.com/harfbuzz/harfbuzz/archive/refs/tags/%HARFBUZZ%.zip bb2f83255706b1c92d731541c7cefaf98bb5b93e8f76d16f6deda05225ff20ee || goto error
call :downloadfile "lpng%LIBPNG%.zip" https://download.sourceforge.net/libpng/lpng1658.zip b32f170855dbbe3e6d9e645af40b538137041773672c3ba3e02db5816c82d376 || goto error
call :downloadfile "lpng%LIBPNG%-apng.patch.gz" https://download.sourceforge.net/libpng-apng/libpng-%LIBPNGLONG%-apng.patch.gz eee7dea22ed502868017971c86c63c4ed1e6085de0baebfdcc3d3322f00f3eb0 || goto error
call :downloadfile "libjpeg-turbo-%LIBJPEGTURBO%.tar.gz" "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/%LIBJPEGTURBO%/libjpeg-turbo-%LIBJPEGTURBO%.tar.gz" 6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e || goto error
call :downloadfile "libwebp-%WEBP%.tar.gz" "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-%WEBP%.tar.gz" e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564 || goto error
call :downloadfile "%SDL%.zip" "https://github.com/SternXD/SDL3-uwp/archive/refs/heads/%SDL%.zip" 1149c0965c35474f0176f0333ec7450bcbda6d5500b2d409442d37e9b5a16d8b || goto error
call :downloadfile "lz4-%LZ4%.zip" "https://github.com/lz4/lz4/archive/refs/tags/v%LZ4%.zip" 3224b4c80f351f194984526ef396f6079bd6332dd9825c72ac0d7a37b3cdc565 || goto error
call :downloadfile "zlib%ZLIBSHORT%.zip" "https://github.com/madler/zlib/releases/download/v%ZLIB%/zlib%ZLIBSHORT%.zip" e8bf55f3017aa181690990cb58a994e77885da140609fc8f94abe9b65d2cae28 || goto error
call :downloadfile "zstd-%ZSTD%.zip" "https://github.com/facebook/zstd/archive/refs/tags/v%ZSTD%.zip" 7897bc5d620580d9b7cd3539c44b59d78f3657d33663fe97a145e07b4ebd69a4 || goto error
call :downloadfile "plutovg-%PLUTOVG%.zip" "https://github.com/sammycage/plutovg/archive/v%PLUTOVG%.zip" 4fe4e48f28aa80171b2166d45c0976ab0f21eecedb52cd4c3ef73b5afb48fac9 || goto error
call :downloadfile "plutosvg-%PLUTOSVG%.zip" "https://github.com/sammycage/plutosvg/archive/v%PLUTOSVG%.zip" 82dee2c57ad712bdd6d6d81d3e76249d89caa4b5a4214353660fd5adff12201a || goto error
call :downloadfile "rapidyaml-%RAPIDYAML%-src.zip" "https://github.com/biojppm/rapidyaml/releases/download/v%RAPIDYAML%/rapidyaml-%RAPIDYAML%-src.zip" 96276f55b9fa7837ac8f3f72fd52965879cbb5d5d2e6af548c69a177fb078304 || goto error
call :downloadfile "DirectX-Headers-%DXHEADERS%.zip" "https://github.com/microsoft/DirectX-Headers/archive/v%DXHEADERS%.zip" 9eb8b102a90a42e4ea72a825f7d249d55ec90d164f030966c9b7784b93374927 || goto error

call :downloadfile "shaderc-%SHADERC%.zip" "https://github.com/google/shaderc/archive/refs/tags/v%SHADERC%.zip" f9401cc5cb36c276cd1e072b6595dbd728148e8dba389e50f7339e2d388dbc08 || goto error
call :downloadfile "shaderc-glslang-%SHADERC_GLSLANG%.zip" "https://github.com/KhronosGroup/glslang/archive/%SHADERC_GLSLANG%.zip" 2b63189efad0348d88d410a5e12ec550a612e0b6ceef64624b8f45491269fb9c || goto error
call :downloadfile "shaderc-spirv-headers-%SHADERC_SPIRVHEADERS%.zip" "https://github.com/KhronosGroup/SPIRV-Headers/archive/%SHADERC_SPIRVHEADERS%.zip" d2f071e94c081f5a4606559770ebf1f7d1eac92a1def0c3e10609844aa8b69b2 || goto error
call :downloadfile "shaderc-spirv-tools-%SHADERC_SPIRVTOOLS%.zip" "https://github.com/KhronosGroup/SPIRV-Tools/archive/%SHADERC_SPIRVTOOLS%.zip" 4011be89aa73e3461c9deef73936a62c79a3097590c5135d058041cc9fb99c6f || goto error

if %DEBUG%==1 (
  echo Building debug and release libraries...
) else (
  echo Building release libraries...
)

echo Building Zlib...
rmdir /S /Q "zlib-%ZLIB%"
%SEVENZIP% x "zlib%ZLIBSHORT%.zip" || goto error
cd "zlib-%ZLIB%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release %UWP_CMAKE_FLAGS% %FORCEPDB% -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DZLIB_BUILD_EXAMPLES=OFF -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building libpng...
rmdir /S /Q "lpng%LIBPNG%"
%SEVENZIP% x "lpng%LIBPNG%.zip" || goto error
rem apng not in released libpng yet
%SEVENZIP% x "lpng%LIBPNG%-apng.patch.gz" -aoa || goto error
cd "lpng%LIBPNG%" || goto error
%PATCH% -p1 < "../libpng-%LIBPNGLONG%-apng.patch" || goto error
cmake -DCMAKE_BUILD_TYPE=Release %UWP_CMAKE_FLAGS% %FORCEPDB% -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DPNG_TESTS=OFF -DPNG_STATIC=OFF -DPNG_SHARED=ON -DPNG_TOOLS=OFF -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building libjpegturbo...
rmdir /S /Q "libjpeg-turbo-%LIBJPEGTURBO%"
tar -xf "libjpeg-turbo-%LIBJPEGTURBO%.tar.gz" || goto error
cd "libjpeg-turbo-%LIBJPEGTURBO%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release %UWP_CMAKE_FLAGS% %FORCEPDB% -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DENABLE_SHARED=ON -DENABLE_STATIC=OFF -DWITH_CRT_DLL=ON -DWITH_TOOLS=OFF -DWITH_TESTS=OFF -DWITH_JAVA=OFF -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building LZ4...
rmdir /S /Q "lz4"
%SEVENZIP% x "lz4-%LZ4%.zip" || goto error
rename "lz4-%LZ4%" "lz4" || goto error
cd "lz4" || goto error
cmake -DCMAKE_BUILD_TYPE=Release %UWP_CMAKE_FLAGS% %FORCEPDB% -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF -DCMAKE_C_FLAGS="/wd4711 /wd5045" -B build-dir -G Ninja build/cmake || goto error
cmake --build build-dir --parallel || goto error
ninja -C build-dir install || goto error
cd .. || goto error

echo Building FreeType without HarfBuzz...
rmdir /S /Q "freetype-%FREETYPE%"
tar -xf "freetype-%FREETYPE%.tar.gz" || goto error
cd "freetype-%FREETYPE%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release %UWP_CMAKE_FLAGS% %FORCEPDB% -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DFT_REQUIRE_ZLIB=TRUE -DFT_REQUIRE_PNG=TRUE -DFT_DISABLE_BZIP2=TRUE -DFT_DISABLE_BROTLI=TRUE -DFT_DISABLE_HARFBUZZ=TRUE -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building HarfBuzz...
rmdir /S /Q "harfbuzz-%HARFBUZZ%"
%SEVENZIP% x "-x^!harfbuzz-%HARFBUZZ%\README" "harfbuzz-%HARFBUZZ%.zip" || goto error
cd "harfbuzz-%HARFBUZZ%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release %UWP_CMAKE_FLAGS% %FORCEPDB% -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DHB_BUILD_UTILS=OFF -DHB_BUILD_GPU=OFF -DHAVE_MMAP=0 -DHAVE_MPROTECT=0 -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building FreeType with HarfBuzz...
rmdir /S /Q "freetype-%FREETYPE%"
tar -xf "freetype-%FREETYPE%.tar.gz" || goto error
cd "freetype-%FREETYPE%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release %UWP_CMAKE_FLAGS% %FORCEPDB% -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DFT_REQUIRE_ZLIB=TRUE -DFT_REQUIRE_PNG=TRUE -DFT_DISABLE_BZIP2=TRUE -DFT_DISABLE_BROTLI=TRUE -DFT_REQUIRE_HARFBUZZ=TRUE -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building Zstandard...
rmdir /S /Q "zstd-%ZSTD%"
%SEVENZIP% x "-x^!zstd-%ZSTD%\tests\cli-tests\bin" "zstd-%ZSTD%.zip" || goto error
cd "zstd-%ZSTD%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release %UWP_CMAKE_FLAGS% %FORCEPDB% -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DZSTD_BUILD_SHARED=ON -DZSTD_BUILD_STATIC=OFF -DZSTD_BUILD_PROGRAMS=OFF -B build -G Ninja build/cmake || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building WebP...
rmdir /S /Q "libwebp-%WEBP%"
tar -xf "libwebp-%WEBP%.tar.gz" || goto error
cd "libwebp-%WEBP%" || goto error
cmake -B build -DCMAKE_BUILD_TYPE=Release %UWP_CMAKE_FLAGS% %FORCEPDB% -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF -DWEBP_USE_THREAD=OFF -DHAVE_BUILTIN_BSWAP16=0 -DHAVE_BUILTIN_BSWAP32=0 -DHAVE_BUILTIN_BSWAP64=0 -DCMAKE_C_FLAGS="/D__builtin_bswap16=_byteswap_ushort /D__builtin_bswap32=_byteswap_ulong /D__builtin_bswap64=_byteswap_uint64" -DBUILD_SHARED_LIBS=ON -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building SDL...
rmdir /S /Q "SDL3-uwp-%SDL%"
%SEVENZIP% x "%SDL%.zip" || goto error
cd "SDL3-uwp-%SDL%" || goto error
cmake -B build -DCMAKE_BUILD_TYPE=Release %UWP_CMAKE_FLAGS% %FORCEPDB% -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DSDL_SHARED=ON -DSDL_STATIC=OFF -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
copy build\SDL3.pdb "%INSTALLDIR%\bin" || goto error
cd .. || goto error

echo "Building PlutoVG..."
rmdir /S /Q "plutovg-%PLUTOVG%"
%SEVENZIP% x "plutovg-%PLUTOVG%.zip" || goto error
cd "plutovg-%PLUTOVG%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release %UWP_CMAKE_FLAGS% %FORCEPDB% -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DPLUTOVG_BUILD_EXAMPLES=OFF -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo "Building PlutoSVG..."
rmdir /S /Q "plutosvg-%PLUTOSVG%"
%SEVENZIP% x "plutosvg-%PLUTOSVG%.zip" || goto error
cd "plutosvg-%PLUTOSVG%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release %UWP_CMAKE_FLAGS% %FORCEPDB% -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DPLUTOSVG_ENABLE_FREETYPE=ON -DPLUTOSVG_BUILD_EXAMPLES=OFF -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo "Building RapidYAML..."
rmdir /S /Q "rapidyaml-%RAPIDYAML%-src"
%SEVENZIP% x "rapidyaml-%RAPIDYAML%-src.zip" || goto error
cd "rapidyaml-%RAPIDYAML%-src" || goto error
cmake -DCMAKE_BUILD_TYPE=Release %UWP_CMAKE_FLAGS% %FORCEPDB% -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Unpacking DirectX Headers
rmdir /S /Q "DirectX-Headers-%DXHEADERS%"
%SEVENZIP% x "DirectX-Headers-%DXHEADERS%.zip" || goto error
cd "DirectX-Headers-%DXHEADERS%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release %UWP_CMAKE_FLAGS% %FORCEPDB% -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DDXHEADERS_BUILD_TEST=OFF -DDXHEADERS_BUILD_GOOGLE_TEST=OFF -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building shaderc...
rmdir /S /Q "shaderc-%SHADERC%"
%SEVENZIP% x "shaderc-%SHADERC%.zip" || goto error
cd "shaderc-%SHADERC%" || goto error
cd third_party || goto error
%SEVENZIP% x "..\..\shaderc-glslang-%SHADERC_GLSLANG%.zip" || goto error
rename "glslang-%SHADERC_GLSLANG%" "glslang" || goto error
%SEVENZIP% x "..\..\shaderc-spirv-headers-%SHADERC_SPIRVHEADERS%.zip" || goto error
rename "SPIRV-Headers-%SHADERC_SPIRVHEADERS%" "spirv-headers" || goto error
%SEVENZIP% x "..\..\shaderc-spirv-tools-%SHADERC_SPIRVTOOLS%.zip" || goto error
rename "SPIRV-Tools-%SHADERC_SPIRVTOOLS%" "spirv-tools" || goto error
cd .. || goto error
%PATCH% -p1 < "%SCRIPTDIR%\..\common\shaderc-changes.patch" || goto error
cmake -DCMAKE_BUILD_TYPE=Release %UWP_CMAKE_FLAGS% %FORCEPDB% -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSHADERC_ENABLE_SHARED_CRT=ON -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Cleaning up...
cd ..
rd /S /Q deps-build

echo Exiting with success.
exit 0

:error
echo Failed with error #%errorlevel%.
pause
exit %errorlevel%

:downloadfile
if not exist "%~1" (
  echo Downloading %~1 from %~2...
  curl -L -o "%~1" "%~2" || goto error
)

rem based on https://gist.github.com/gsscoder/e22daefaff9b5d8ac16afb070f1a7971
set idx=0
for /f %%F in ('certutil -hashfile "%~1" SHA256') do (
    set "out!idx!=%%F"
    set /a idx += 1
)
set filechecksum=%out1%

if /i %~3==%filechecksum% (
    echo Validated %~1.
    exit /B 0
) else (
    echo Expected %~3 got %filechecksum%.
    exit /B 1
)

:downloadfile-nohash
if not exist "%~1" (
  echo Downloading %~1 from %~2...
  curl -L -o "%~1" "%~2" || goto error
)
echo Skipping validation for %~1 (development build).
exit /B 0

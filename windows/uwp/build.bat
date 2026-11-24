@echo off
setlocal EnableExtensions

rem Builds the SDL host as a UWP app for Windows and Xbox Dev Mode.
rem Run bash build/build-win.sh first, so build\PokeDaisy-windows-x64 has the cores.
rem Set SDL2_UWP_DIR to a folder holding SDL2-2.30.5 and bin\SDL2.dll, or build SDL2
rem into the default build\uwp\sdl2 (see sdl2\README.md).
rem Output: build\uwp\host\layout and build\uwp\PokeDaisyUwpHost.appx.

set "HOST=%~dp0"
for %%I in ("%HOST%..\..") do set "ROOT=%%~fI"
set "OBJ=%ROOT%\build\uwp\host\obj"
set "LAYOUT=%ROOT%\build\uwp\host\layout"
set "RUNTIME=%ROOT%\build\PokeDaisy-windows-x64"
if not defined SDL2_UWP_DIR set "SDL2_UWP_DIR=%ROOT%\build\uwp\sdl2"

if not exist "%SDL2_UWP_DIR%\SDL2-2.30.5\include\SDL.h" (
  echo SDL2 UWP source not found at "%SDL2_UWP_DIR%\SDL2-2.30.5".
  echo Set SDL2_UWP_DIR to a folder with SDL2-2.30.5 and bin\SDL2.dll.
  exit /b 2
)
if not exist "%SDL2_UWP_DIR%\bin\SDL2.dll" (
  echo SDL2.dll not found at "%SDL2_UWP_DIR%\bin".
  echo Build SDL2 2.30.5 for UWP first, or set SDL2_UWP_DIR.
  exit /b 2
)
if not exist "%RUNTIME%\pokedaisy_gba_libretro.dll" (
  echo Cores not found at "%RUNTIME%".
  echo Run bash build/build-win.sh first.
  exit /b 2
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo vswhere.exe not found. Install Visual Studio 2022 with the C++ desktop workload.
  exit /b 2
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
  echo Visual Studio with the C++ tools not found.
  exit /b 2
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 2

set "WINMD=%WindowsSdkDir%UnionMetadata\%WindowsSDKVersion%Windows.winmd"
set "MAKEAPPX=%WindowsSdkDir%bin\%WindowsSDKVersion%x64\MakeAppx.exe"
if not exist "%WINMD%" (
  echo Windows.winmd not found at "%WINMD%".
  echo Install the Windows 10 or 11 SDK.
  exit /b 2
)
if not exist "%MAKEAPPX%" (
  echo MakeAppx.exe not found at "%MAKEAPPX%".
  exit /b 2
)

if not defined MINGW_GCC set "MINGW_GCC=C:\msys64\mingw64\bin\gcc.exe"
if exist "%MINGW_GCC%" goto :have_mingw
set "MINGW_GCC=gcc"
where gcc >nul 2>nul
if errorlevel 1 (
  echo MinGW gcc not found. Set MINGW_GCC to the MSYS2 mingw64 gcc.exe.
  exit /b 2
)
:have_mingw

if not exist "%OBJ%" mkdir "%OBJ%"
del /q "%OBJ%\*.obj" 2>nul
if exist "%LAYOUT%" rmdir /s /q "%LAYOUT%"
mkdir "%LAYOUT%\Assets"

rem The host's own sources, then every part of the companion the library draws with.
> "%OBJ%\sources.rsp" echo "%ROOT%\linux\main.c"
>> "%OBJ%\sources.rsp" echo "%ROOT%\linux\library.c"
>> "%OBJ%\sources.rsp" echo "%ROOT%\linux\cover_png.c"
>> "%OBJ%\sources.rsp" echo "%ROOT%\3ds\desktop\pd_png.c"
>> "%OBJ%\sources.rsp" echo "%ROOT%\third_party\zlib\contrib\puff\puff.c"
for %%f in ("%ROOT%\3ds\core\*.c") do if not "%%~nxf"=="pd_guide_gen.c" >> "%OBJ%\sources.rsp" echo "%%~f"

echo === compile host (C, app container) ===
cl /nologo /c /MD /O2 /W3 /std:c11 /utf-8 ^
 /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE /DNOMINMAX /DPD_UWP ^
 /DWINAPI_FAMILY=WINAPI_FAMILY_APP /D_WIN32_WINNT=0x0A00 /DUNICODE /D_UNICODE ^
 /FI"%HOST%include\pd_uwp_compat.h" ^
 /I"%HOST%include" /I"%ROOT%\linux" /I"%ROOT%\3ds\core" /I"%ROOT%\3ds\desktop" ^
 /I"%ROOT%\third_party\mgba\src\platform\libretro" /I"%ROOT%\third_party\zlib\contrib\puff" ^
 /I"%SDL2_UWP_DIR%\SDL2-2.30.5\include" ^
 /Fo"%OBJ%\\" /Fd"%OBJ%\host.pdb" @"%OBJ%\sources.rsp"
echo HOST_COMPILE_EXIT=%ERRORLEVEL%
if errorlevel 1 goto :done

rem MSVC 14.44's front end crashes on this generated table file (C1001, whatever the flags), though it is plain C data.
rem MinGW, the toolchain the cores are built with, compiles it; link.exe takes its COFF object, whose symbols are C
rem data with external linkage only.
echo === compile pd_guide_gen.c (MinGW, MSVC C1001 workaround) ===
"%MINGW_GCC%" -std=c11 -O2 -c -I"%ROOT%\3ds\core" -o "%OBJ%\pd_guide_gen.obj" "%ROOT%\3ds\core\pd_guide_gen.c"
echo GUIDE_COMPILE_EXIT=%ERRORLEVEL%
if errorlevel 1 goto :done

echo === compile SDL winrt main ===
cl /nologo /c /MD /O2 /W3 /EHsc /utf-8 /ZW /D_CRT_SECURE_NO_WARNINGS /DSDL_WINRT_METADATA_FILE_AVAILABLE ^
 /DWINAPI_FAMILY=WINAPI_FAMILY_APP /D_WIN32_WINNT=0x0A00 /DUNICODE /D_UNICODE ^
 /I"%SDL2_UWP_DIR%\SDL2-2.30.5\include" /FU"%WINMD%" ^
 "%SDL2_UWP_DIR%\SDL2-2.30.5\src\main\winrt\SDL_winrt_main_NonXAML.cpp" /Fo"%OBJ%\winrtmain.obj" /Fd"%OBJ%\host.pdb"
echo MAIN_COMPILE_EXIT=%ERRORLEVEL%
if errorlevel 1 goto :done

echo === link ===
> "%OBJ%\objects.rsp" echo "%OBJ%\winrtmain.obj"
for %%f in ("%OBJ%\*.obj") do if not "%%~nxf"=="winrtmain.obj" >> "%OBJ%\objects.rsp" echo "%%~f"
cl /nologo /MD /Fe"%LAYOUT%\PokeDaisy.exe" @"%OBJ%\objects.rsp" ^
 /link /SUBSYSTEM:WINDOWS /APPCONTAINER /LIBPATH:"%SDL2_UWP_DIR%\bin" SDL2.lib WindowsApp.lib runtimeobject.lib
echo LINK_EXIT=%ERRORLEVEL%
if errorlevel 1 goto :done

echo === stage layout ===
copy /y "%SDL2_UWP_DIR%\bin\SDL2.dll" "%LAYOUT%\" >nul
copy /y "%HOST%app\AppxManifest.xml" "%LAYOUT%\" >nul
copy /y "%HOST%app\Assets\*.png" "%LAYOUT%\Assets\" >nul
copy /y "%RUNTIME%\pokedaisy_gbc_libretro.dll" "%LAYOUT%\" >nul
copy /y "%RUNTIME%\pokedaisy_gba_libretro.dll" "%LAYOUT%\" >nul
copy /y "%RUNTIME%\pokedaisy_ds_libretro.dll" "%LAYOUT%\" >nul
copy /y "%RUNTIME%\libgcc_s_seh-1.dll" "%LAYOUT%\" >nul
copy /y "%RUNTIME%\libwinpthread-1.dll" "%LAYOUT%\" >nul
copy /y "%RUNTIME%\zlib1.dll" "%LAYOUT%\" >nul

rem The MSVC runtime beside the exe: no VCLibs framework package to depend on.
for %%f in (vcruntime140.dll vcruntime140_1.dll msvcp140.dll vccorlib140.dll concrt140.dll) do (
  copy /y "%VCToolsRedistDir%x64\Microsoft.VC143.CRT\%%f" "%LAYOUT%\" >nul
)

echo === pack ===
if exist "%ROOT%\build\uwp\PokeDaisyUwpHost.appx" del /q "%ROOT%\build\uwp\PokeDaisyUwpHost.appx"
"%MAKEAPPX%" pack /d "%LAYOUT%" /p "%ROOT%\build\uwp\PokeDaisyUwpHost.appx" /o
echo PACK_EXIT=%ERRORLEVEL%
echo === layout ===
dir /b "%LAYOUT%"
:done
echo HOST_BUILD_DONE

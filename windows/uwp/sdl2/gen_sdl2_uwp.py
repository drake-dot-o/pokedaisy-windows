"""Turn SDL2's VisualC-WinRT/SDL-UWP.vcxproj into a standalone cl.exe build script.

The UWP workload is not installed on every build machine (no
Microsoft.Cpp.WindowsStore.props, no Microsoft.AppxPackage.targets), so MSBuild
cannot always build that project. Everything the project needs is expressible on
the command line, so this reproduces it with cl directly.

Download SDL2 2.30.5's source to build/uwp/sdl2/SDL2-2.30.5, then run:

    python windows/uwp/sdl2/gen_sdl2_uwp.py
    build/uwp/sdl2/build_sdl2_uwp.bat

SDL2_UWP_SRC and SDL2_UWP_OUT move the source and output folders.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.abspath(os.environ.get("SDL2_UWP_OUT", os.path.join(ROOT, "build", "uwp", "sdl2")))
SRC = os.path.abspath(os.environ.get("SDL2_UWP_SRC", os.path.join(OUT, "SDL2-2.30.5")))
PROJ = os.path.join(SRC, "VisualC-WinRT", "SDL-UWP.vcxproj")
OBJ = os.path.join(OUT, "obj", "sdl2-uwp-x64")
BIN = os.path.join(OUT, "bin")

if not os.path.exists(PROJ):
    sys.exit("SDL2 2.30.5 source not found at %s" % SRC)

text = open(PROJ, encoding="utf-8-sig").read()
zw, plain = [], []
for m in re.finditer(r"<ClCompile\s+Include=\"([^\"]+)\"([^>]*)>", text):
    if m.group(2).strip().endswith("/"):
        continue  # self-closing: parsed by the loop below
    rel = m.group(1)
    body = text[m.end():text.find("</ClCompile>", m.end())]
    (zw if "CompileAsWinRT>true" in body else plain).append(rel)
for m in re.finditer(r"<ClCompile\s+Include=\"([^\"]+)\"\s*/>", text):
    plain.append(m.group(1))


def win(p):
    return os.path.normpath(os.path.join(SRC, "VisualC-WinRT", p)).replace("/", "\\")


missing = [p for p in zw + plain if not os.path.exists(win(p))]
print("zw=%d plain=%d total=%d missing=%d" % (len(zw), len(plain), len(zw) + len(plain), len(missing)))
for p in missing:
    print("MISSING", p)
if missing:
    sys.exit("SDL2's UWP project names files that are not in %s" % SRC)

# UWP/C++-CX: WINAPI_FAMILY_APP is what SDL's SDL_platform.h keys __WINRT__ off, so it is
# not passed explicitly. /ZW marks the files the project flags as CompileAsWinRT.
DEFS = ["/DDLL_EXPORT", "/D_CRT_SECURE_NO_WARNINGS", "/DSDL_BUILDING_WINRT=1",
        "/DWINAPI_FAMILY=WINAPI_FAMILY_APP", "/D_WIN32_WINNT=0x0A00", "/DUNICODE", "/D_UNICODE"]
INC = ['/I"%s"' % os.path.join(SRC, "include"), '/I"%s"' % os.path.join(SRC, "src")]
COMMON = ["/nologo", "/c", "/MD", "/O2", "/W3", "/EHsc", "/utf-8", "/D_CRT_SECURE_NO_WARNINGS"] + DEFS + INC

os.makedirs(OBJ, exist_ok=True)
os.makedirs(BIN, exist_ok=True)

lines = ["@echo off",
         "setlocal",
         'set "VSWHERE=%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer\\vswhere.exe"',
         'if not exist "%VSWHERE%" (echo vswhere.exe not found. Install Visual Studio 2022. & exit /b 2)',
         'for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"',
         'if not defined VS (echo Visual Studio with the C++ tools not found. & exit /b 2)',
         'call "%VS%\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul',
         'if errorlevel 1 exit /b 2',
         'set "WINMD=%WindowsSdkDir%UnionMetadata\\%WindowsSDKVersion%Windows.winmd"',
         'if not exist "%WINMD%" (echo Windows.winmd not found. Install the Windows 10 or 11 SDK. & exit /b 2)',
         'if not exist "%s" mkdir "%s"' % (OBJ, OBJ),
         'if not exist "%s" mkdir "%s"' % (BIN, BIN),
         'set FAILED=0', 'set FIRSTFAIL=',
         'del /q "%s\\*.obj" 2>nul' % OBJ,
         'del /q "%s\\SDL2.dll" 2>nul' % BIN]

for rel in plain:
    src = win(rel)
    obj = os.path.join(OBJ, rel.replace("\\", "_").replace("/", "_") + ".obj")
    cmd = 'cl %s "%s" /Fo"%s" /Fd"%s"' % (" ".join(COMMON), src, obj, os.path.join(OBJ, "vc.pdb"))
    lines.append('echo [CC] %s' % os.path.basename(rel))
    lines.append(cmd)
    lines.append('if errorlevel 1 (set FAILED=1 & if not defined FIRSTFAIL set FIRSTFAIL=%s)' % rel)

for rel in zw:
    src = win(rel)
    obj = os.path.join(OBJ, rel.replace("\\", "_").replace("/", "_") + ".obj")
    cmd = 'cl %s /ZW /TP /FU"%s" "%s" /Fo"%s" /Fd"%s"' % (
        " ".join(COMMON), "%WINMD%", src, obj, os.path.join(OBJ, "vc.pdb"))
    lines.append('echo [CC-ZW] %s' % os.path.basename(rel))
    lines.append(cmd)
    lines.append('if errorlevel 1 (set FAILED=1 & if not defined FIRSTFAIL set FIRSTFAIL=%s)' % rel)

lines += ['echo COMPILE_FAILED=%FAILED%',
          'echo COMPILE_FIRSTFAIL=%FIRSTFAIL%',
          'if "%FAILED%"=="1" goto :done',
          'cl /nologo /LD /MD /EHsc /Fe"%s\\SDL2.dll" "%s\\*.obj" /link /IMPLIB:"%s\\SDL2.lib" WindowsApp.lib runtimeobject.lib ole32.lib oleaut32.lib uuid.lib dxgi.lib d3d11.lib version.lib' % (BIN, OBJ, BIN),
          'echo LINK_EXIT=%ERRORLEVEL%',
          ':done',
          'echo BUILD_DONE']

script = os.path.join(OUT, "build_sdl2_uwp.bat")
open(script, "w", newline="\r\n").write("\n".join(lines) + "\n")
print("wrote", script)

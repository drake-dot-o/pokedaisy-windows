# SDL2 for the UWP host

The UWP host links SDL2's UWP target. SDL2 is not bundled in the repository, so a
build needs the SDL2 2.30.5 source and its UWP build outputs.

## Get the source

Download the SDL2 2.30.5 source release and unpack it into the default folder:

```
build/uwp/sdl2/SDL2-2.30.5
```

The source release is on SDL's releases page:
`https://github.com/libsdl-org/SDL/releases/tag/release-2.30.5`. The 2.30.5
tarball's SHA-256 is
`f374f3fa29c37dfcc20822d4a7d7dc57e58924d1a5f2ad511bfab4c8193de63b`.
`SDL2_UWP_SRC` points somewhere else if the source lives elsewhere.

## Build it

SDL2's `VisualC-WinRT/SDL-UWP.sln` needs the Visual Studio UWP workload. If that
workload is installed, open the solution, build the x64 UWP target, and copy
`SDL2.dll` and `SDL2.lib` into `build/uwp/sdl2/bin`.

Without the UWP workload, generate a standalone `cl.exe` build. It reads the
same source list and flags from `SDL-UWP.vcxproj`, so it needs only the C++
desktop tools and the Windows SDK:

```
python windows/uwp/sdl2/gen_sdl2_uwp.py
build/uwp/sdl2/build_sdl2_uwp.bat
```

`SDL2_UWP_OUT` moves the output folder, which then holds `SDL2-2.30.5`,
`bin/SDL2.dll` and `bin/SDL2.lib`.

The host build expects that folder as `SDL2_UWP_DIR`, and defaults to
`build/uwp/sdl2`.

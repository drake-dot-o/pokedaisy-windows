# PokeDaisy UWP host

The Windows host built as a UWP app for Windows and Xbox Dev Mode. It runs the
same three libretro cores, companion, HUD and quick menu as the desktop Windows
host. `linux/main.c` and `linux/library.c` carry the `PD_UWP` paths: the
package's install folder, its data under LocalState, and cores loaded with
`LoadPackagedLibrary`.

This is not a Microsoft Store package. It is for an Xbox with Dev Mode enabled,
or a Windows PC for testing.

## Build

### Prerequisites

- Visual Studio 2022, Desktop development with C++.
- Windows 10 or 11 SDK 10.0.22621, or another 10.0 SDK. The script uses the SDK
  that `vcvars64.bat` selects.
- MSYS2 MinGW-w64 gcc. `pd_guide_gen.c` crashes MSVC's C front end (C1001), so
  the script compiles that one file with gcc. Set `MINGW_GCC` if gcc is not at
  `C:\msys64\mingw64\bin\gcc.exe`.
- Python 3 for the SDL2 helper.
- SDL2 2.30.5 built for UWP, see `sdl2/README.md`.

### Cores

```
bash build/build-win.sh
```

This fills `build/PokeDaisy-windows-x64` with the three cores.

### SDL2

Put SDL2 2.30.5 at `build/uwp/sdl2/SDL2-2.30.5` and build its UWP target as
`sdl2/README.md` says. Set `SDL2_UWP_DIR` if it is elsewhere; the folder must
hold `SDL2-2.30.5` and `bin\SDL2.dll`.

### Host

```
windows\uwp\build.bat
```

Outputs `build/uwp/host/layout` and `build/uwp/PokeDaisyUwpHost.appx`.

## Install

For a Windows PC, `windows\uwp\install.ps1` registers the loose layout in place.
For an Xbox, enable Dev Mode, open the Device Portal, add
`PokeDaisyUwpHost.appx` and the `.cer`, then start PokeDaisy.

`install.ps1` only calls `Add-AppxPackage -Register`; it never removes the
package, so LocalState stays. Do not use a remove/register script: it takes the
games and saves with it.

Package identity: `PokeDaisy.UwpHost_5wqf515wt4wy6`, publisher
`CN=PokeDaisyHost`, x64. Keep both stable: the package family name is what
LocalState is keyed by.

Game data:

```
LocalAppData\PokeDaisy.UwpHost_5wqf515wt4wy6\LocalState\PokeDaisy\UwpHost
```

`roms` holds the games, `saves` the battery saves, `states` the states and
`host.ini` the host settings. Use the Device Portal's File Explorer on an Xbox.

## Signing

The build packs an unsigned appx. For the Device Portal, sign it with a
certificate whose subject is exactly `CN=PokeDaisyHost`, then install the
certificate's `.cer` on the Xbox. Run these in a Developer Command Prompt:

```powershell
$cert = New-SelfSignedCertificate -Type Custom -Subject "CN=PokeDaisyHost" `
  -KeyUsage DigitalSignature -FriendlyName "PokeDaisy Xbox" `
  -CertStoreLocation "Cert:\CurrentUser\My" `
  -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
Export-Certificate -Cert $cert -FilePath PokeDaisyHost.cer
Export-PfxCertificate -Cert $cert -FilePath PokeDaisyHost.pfx `
  -Password (Read-Host -AsSecureString "PFX password")
signtool sign /fd SHA256 /a /f PokeDaisyHost.pfx `
  /p "<pfx password>" build\uwp\PokeDaisyUwpHost.appx
```

Never commit the `.pfx` or its password. Ship only the `.cer`. The certificate
subject must match the manifest's publisher exactly; a different subject fails
to install or update. The same identity and certificate across builds preserves
saves.

## Notes

- The appx is x64.
- The host registers on Windows. It has not been run on an Xbox here. No console
  was available, so input, rendering, audio and core loading are untested on
  hardware.
- The UWP data root is LocalState; the desktop Windows host's data stays beside
  the executable.
- The host starts the same cores; no Microsoft Store certification is needed
  for Dev Mode sideloading.

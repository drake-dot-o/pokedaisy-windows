---
title: Install on Xbox
summary: Play PokeDaisy on an Xbox in Dev Mode with your own games and saves.
section: start
order: 87
keywords: [xbox, series x, series s, dev mode, uwp, sideload, appx, certificate, device portal]
---

PokeDaisy runs on an Xbox in Dev Mode. The game fills the TV and the companion sits beside it, as on Windows. The Xbox build is a preview: it is not in the Microsoft Store, and you install it yourself.

> [!WARNING]
> The Xbox build needs Dev Mode, a Windows PC and a certificate. It is not a plug-and-play download yet.

## What you need

- An Xbox Series X or Series S with Dev Mode enabled. Turning it on needs a Microsoft account and a one-time activation. Microsoft's Dev Mode page has the current steps.
- A Windows PC on the same network as the Xbox.
- `PokeDaisyUwpHost.appx`, signed with a certificate whose subject is exactly `CN=PokeDaisyHost`, and that certificate's `.cer` file.
- Your own games: `.gba`, `.gb`, `.gbc` or `.nds` files, unzipped.
- A controller.

The exact certificate and signing commands are in [the host's build notes](https://github.com/lidor30/pokedaisy/blob/main/windows/uwp/README.md). If someone gave you the package, they should also give you the matching `.cer`.

## Install it

1. On the PC, open the Xbox Device Portal at `https://YOUR-XBOX-IP:11443` and sign in with the Xbox's Dev Mode account.
2. Open **Add your certificate**, choose the `.cer` file, and add it. Do this before the package, or the Xbox will refuse the package's publisher.
3. Open **Add package**, choose the signed `PokeDaisyUwpHost.appx`, and install it.
4. On the Xbox, open **PokeDaisy** from the apps list. The first start makes the data folders.

The package is `PokeDaisy.UwpHost_5wqf515wt4wy6`. Keep that identity and the certificate the same when you update: the package family name is what the Xbox keys the app's data by.

## Add your games and saves

On the Xbox, open the Device Portal's **File Explorer** and go to:

`LocalAppData\PokeDaisy.UwpHost_5wqf515wt4wy6\LocalState\PokeDaisy\UwpHost`

Put games in `roms` and battery saves in `saves`. Use the whole ROM file name plus `.sav` for a save, or the ROM name without its extension. Save states go in `states`. The library scans `roms` when it starts.

The host keeps its settings in `host.ini` in that folder. The desktop Windows build keeps its data beside the executable instead.

## Updating

Install the newer appx signed with the same certificate and the same package identity. The Xbox updates it in place and keeps `roms`, `saves`, `states` and `host.ini`.

An appx signed with a different certificate is a different publisher and will not update the installed one.

## If something's wrong

- **The package will not install:** add the matching `.cer` first, and check that its subject is exactly `CN=PokeDaisyHost`.
- **The Xbox says the publisher does not match:** the appx was signed with a different certificate from the one you installed. Use the matching pair.
- **No games are listed:** the files must be right in `roms`, unzipped, and end in `.gba`, `.gb`, `.gbc` or `.nds`.
- **A save does not load:** put it in `saves` under the ROM's whole file name plus `.sav`, or the ROM's name without its extension.
- **The controller does nothing:** start the app with the controller connected, then reopen it.

The Xbox build is new. If it fails before the library opens, the Device Portal's app log has the reason; include it when you [tell us](https://pokedaisy.com/feedback/#bug).

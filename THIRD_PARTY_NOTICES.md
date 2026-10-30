# Third-party notices

## Application and native integration

The application is licensed GPL-3.0-or-later. Native display/controller integration is adapted from [MiladRTX/ps5-homebrew-starter](https://github.com/MiladRTX/ps5-homebrew-starter), revision `c3f939230aee18863da1f359e39eb43bf95a056d`, particularly `app/src/screen.cpp`, `screen.hpp`, and `pad.cpp`. Those files derive display work from BlackBearReloaded's ps5-native-app-boilerplate, Copyright (C) 2026 BlackBearReloaded, with starter changes by MiladRTX. Retain their source notices.

Some public ABI declarations were cross-referenced against [SvenGDK/SharpProspero](https://github.com/SvenGDK/SharpProspero) `Interop/VideoOut`, `Kernel`, `Pad`, and `UserService` bindings, Copyright (C) 2026 SvenGDK, GPL-3.0-or-later. The native source retains this attribution. The exact reference revision for those declarations was not recorded; no SharpProspero implementation or binary is bundled.

## Native build kit and SDK (external build inputs)

The verified starter revision includes BlackBearReloaded's GPL-3.0-or-later native app kit. The kit supplies startup/linker/module-conversion tools and generates its own clean-room `libc.prx`. It is not a proprietary Sony runtime. The kit's `LICENSE`, `THIRD_PARTY_NOTICES.md`, and `docs/RUNTIME_SHIM.md` describe its code and dependencies.

The kit's setup scripts pin the public [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk) v0.42 and zlib 1.3.2 with archive checksums. This app links the SDK's assertion and local-time compatibility objects, not the full payload CRT. Retain the SDK and its component notices when distributing compiled artifacts. LLVM/Clang and host build utilities are external tools and are not vendored here.

The application source archive intentionally does not vendor the entire kit/SDK. Distributing app binaries and their generated loader companion requires supplying the applicable corresponding source and build inputs, not only this application's source archive.

## Outfit font

`assets/Outfit-Light.ttf`:

- Copyright 2021 The Outfit Project Authors.
- Upstream: https://github.com/Outfitio/Outfit-Fonts
- License: SIL Open Font License 1.1, retained in `assets/OFL.txt` and embedded in the font.
- Bundled SHA-256: `a624896cae00e3a8a64286dc3606a7c3b02e5ef18e47273e1f6954ce5981d4a0`.
- Metadata: version `1.100;gftools[0.9.27]`, designer Rodrigo Fuenzalida, manufacturer fragTYPE.

The bundled font has weight class 300, but internal family/full/PostScript names say Outfit Thin despite the local Light filename. Its exact original download revision is not recorded. The known-working bytes and embedded attribution are preserved; this is not represented as a separately verified pristine upstream Light release. The font is embedded at build time and is not loaded from user-provided files at runtime.

## stb_truetype

`vendor/stb_truetype.h`, version 1.26, by Sean Barrett / RAD Game Tools.

- Upstream: https://github.com/nothings/stb
- Taken byte-for-byte from the pinned starter's `app/src/stb_truetype.h`.
- SHA-256: `ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab`.
- Used under the MIT option of its embedded dual MIT/public-domain license, Copyright (c) 2017 Sean Barrett. The complete license is retained at the end of the header.

The header expressly provides no security guarantee for untrusted font input.

## Project assets

The Aurora Clock icon and desktop-rendered preview were created for this project and are distributed under the project license. The preview is generated from the shared scene renderer, not captured from a console. No Sony artwork, game assets, firmware modules, account data, or console logs are included in this source repository.

# Quran PS5 — credits and licenses

PS5 app by **Youss Riaal**. The app code is licensed under GPL-3.0 (it builds on the GPL native-title
pipeline below); see `ps5/LICENSE`.

## Content used at runtime (not included in this repository)
- Mushaf page images (Madani, 604 pages): https://github.com/zeyadetman/quran-pages-images
- Recitations: per-ayah audio from https://everyayah.com/ — credit to the reciters named in the app
  and to everyayah.com.

## Included data and fonts
- Page / ayah layout data (`data/suras.json`, `data/pages/`): https://github.com/rayed/Quran
- Amiri font (`data/Amiri-*.ttf`), SIL Open Font License 1.1 (`data/Amiri-OFL.txt`); used at build
  time to pre-render the Arabic interface labels in `assets/ui/`.
- Mozilla CA certificate bundle (`ps5/assets/ca-bundle.crt`), MPL 2.0, from the pacbrew curl package.

## Libraries
- SDL2 (zlib), libcurl (curl license), OpenSSL (Apache 2.0), zlib, zstd, libpsl — linked from the
  ps5-payload-dev pacbrew sysroot.
- stb_image (public domain / MIT), dr_mp3 (public domain / MIT-0).
- Native-title toolchain: ps5-native-tool and the clean-room `libc.prx` (GPL-3.0-or-later,
  BlackBearReloaded's ps5-native-app-boilerplate via ProsperoStore), `ps5_compat.cpp`
  (GPL-3.0-or-later, Andrei Kvapil) and the modified SDL PS5 video driver, all vendored through the
  Forgotten Warrior PS5 build pipeline (youss17x/Forgotten-Warrior-PS5).

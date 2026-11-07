# Building Quran PS5

## 1. Desktop build (Linux) — for development

```bash
sudo apt install build-essential libsdl2-dev libcurl4-openssl-dev
make
./quran            # opens the last page read
./quran 50         # opens page 50
```

Keyboard: ← → turn pages · ↑ ↓ jump 20 pages · Tab/T index · Space/S recitation · C reciter ·
Enter/X confirm · Backspace back · H help · Esc quit.

Pages and audio are cached in `./cache` (or `QURAN_CACHE=/path`).

### Test hooks (desktop only)
| Variable | Purpose |
|---|---|
| `QURAN_TEST="t d d x"` | simulated key presses, 0.5 s apart (`l r u d L R x c s t o h`, `.` = wait) |
| `QURAN_SHOT=out.bmp` | save a screenshot after the pages load and the test keys run, then exit |
| `QURAN_AUDIO_URL=file:///dir/%s/%03d%03d.mp3` | read recitation files locally instead of everyayah.com |

Headless: `SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy`.

## 2. PS5 build (native title QRAN00001)

```bash
sudo apt install clang-18 lld-18 python3 curl unzip git
bash tools/setup-ps5-env.sh          # downloads SDK 0.45, pacbrew 0.37, zlib, SDL source; ~400 MB
source ~/ps5-env/env.sh
cd ps5
python3 build-ps5.py --zip           # -> ps5/QRAN00001/ and ps5/QRAN00001-v<version>.zip
```

Raise `contentVersion` in `ps5/sce_sys/param.json` for every release.

Install: copy `QRAN00001` to `/data/homebrew/QRAN00001`, scan with ShadowMount+, launch.
Runtime files on the console: `/data/QuranPS5/` (page cache, `audio/` cache, `quran.log`,
`network.log` on network errors, `last_page.txt`, `reciter.txt`).

## 3. Regenerating data (only when changing labels or layout)

```bash
pip install pillow   # must be built with libraqm (python3 -c "from PIL import features; print(features.check('raqm'))")
python3 tools/gen_data.py                      # src/quran_data.h + assets/ui/*.png (Arabic labels)
git clone --depth 1 https://github.com/zeyadetman/quran-pages-images /tmp/qimg
python3 tools/gen_layout.py /tmp/qimg/quran-images   # src/quran_layout.h (ayah positions per page)
```

## 4. Lessons from the console (all handled in this repo)

1. **Only game-title libraries.** The linker must not see `libkernel_sys.so` / `libkernel_web.so`
   (the title would import `libkernel_sys.prx` and close at launch). `setup-ps5-env.sh` builds a
   filtered stub folder and `build-ps5.py` links only against it.
2. **No `getaddrinfo`.** It lives in `libScePosixForWebKit`, which a title does not load, and
   `libSceNet` cannot be loaded either. `ps5/quran_shims.cpp` has a small DNS client (UDP to
   1.1.1.1 / 8.8.8.8 / 9.9.9.9 / 1.0.0.1).
3. **`fcntl(F_SETFD)` returns EINVAL.** curl treats that as fatal; the shim reports success.
4. **The libc heap is ~16 MiB.** Allocations ≥ 64 KiB are mapped separately (`ps5/vendor/ps5_compat.cpp`,
   lowered from 256 KiB). All UI labels are packed into one atlas texture; 146 separate textures
   filled the heap and OpenSSL could no longer load the CA bundle.
5. Missing libc functions used by curl/OpenSSL (`pipe2`, `gmtime_r`, `basename`, `memrchr`,
   `realpath`, `strdup`, ...) are defined in `ps5/quran_shims.cpp`.
6. Always verify the uploaded `eboot.bin` (size/hash) before testing on the console.

## Layout

```
src/            main.c (app), quran_data.h, quran_layout.h (generated), stb_image.h, dr_mp3.h
assets/ui/      pre-rendered Arabic labels (generated)
data/           sura list, per-page ayah segments (rayed/Quran), Amiri font + OFL
tools/          gen_data.py, gen_layout.py, setup-ps5-env.sh
ps5/            build-ps5.py, quran_shims.cpp, native_shims.cpp, vendor/ (ps5-native-tool,
                ps5_compat.cpp, modified SDL video driver), runtime/libc.prx, sce_sys/, assets/ca-bundle.crt
```

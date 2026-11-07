#!/usr/bin/env bash
# Prepare everything the PS5 build needs, under $PS5_ENV (default: ~/ps5-env).
#   - PS5 Payload SDK v0.45
#   - pacbrew-repo v0.37 prebuilt homebrew sysroot (SDL2, libcurl, OpenSSL, ...)
#   - zlib 1.3.1 source (for the host-side ps5-native-tool)
#   - ps5-payload-dev/SDL at commit 160954cf (headers for the modified video driver)
#   - a filtered stub directory with ONLY the system libraries a game title may load
# Needs: curl, unzip, tar, git. Linux host with clang-18 / ld.lld-18 (apt install clang-18 lld-18).
set -euo pipefail
ENV_DIR="${PS5_ENV:-$HOME/ps5-env}"
mkdir -p "$ENV_DIR" && cd "$ENV_DIR"

if [ ! -d ps5-payload-sdk ]; then
    echo "== PS5 Payload SDK v0.45"
    curl -fL -o sdk.zip https://github.com/ps5-payload-dev/sdk/releases/download/v0.45/ps5-payload-sdk.zip
    unzip -q sdk.zip && rm sdk.zip
fi

if [ ! -d hb ]; then
    echo "== pacbrew v0.37 homebrew sysroot (~340 MB)"
    curl -fL -o hb.tar.gz https://github.com/ps5-payload-dev/pacbrew-repo/releases/download/v0.37/ps5-payload-dev.tar.gz
    mkdir -p hb && tar xzf hb.tar.gz -C hb && rm hb.tar.gz
fi

if [ ! -d tools/zlib-1.3.1 ]; then
    echo "== zlib 1.3.1"
    mkdir -p tools
    curl -fL -o z.tgz https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
    tar xzf z.tgz -C tools && rm z.tgz
fi

if [ ! -f SDL/src/SDL_internal.h ]; then
    echo "== ps5-payload-dev/SDL @160954cf"
    rm -rf SDL && git init -q SDL && cd SDL
    git remote add origin https://github.com/ps5-payload-dev/SDL
    git fetch -q --depth 1 origin 160954cf9833f39197431225f73a3cf737b95d5a
    git checkout -q FETCH_HEAD && cd ..
fi

# IMPORTANT: the SDK also ships libkernel_sys / libkernel_web / WebKit stubs. If the linker resolves
# kernel functions against those, the title imports libkernel_sys.prx and the console refuses to
# start it (it closes instantly). Give the linker only the libraries a game title may load.
echo "== title stubs"
rm -rf stubs && mkdir stubs
for l in libkernel libSceLibcInternal libSceAudioOut libSceImeDialog libScePad libSceSystemService \
         libSceUserService libSceVideoOut libSceKeyboard libSceNet libSceNetCtl libSceSysmodule; do
    cp "ps5-payload-sdk/target/lib/$l.so" stubs/
done

cat > env.sh <<EOF
export LLVM_BIN=/usr/bin
export PS5_PAYLOAD_SDK=$ENV_DIR/ps5-payload-sdk
export PS5_HBROOT=$ENV_DIR/hb/opt/ps5-payload-sdk/target/user/homebrew
export FW_TOOLS=$ENV_DIR/tools
export PS5_SDL_SRC=$ENV_DIR/SDL
export PS5_TITLE_STUBS=$ENV_DIR/stubs
EOF
echo
echo "Done. Before building:  source $ENV_DIR/env.sh"

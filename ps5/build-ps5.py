# Quran PS5 — native PS5 title build (QRAN00001)
# Based on the native-title build pipeline of youss17x/Forgotten-Warrior-PS5 (GPL-3.0-only).
# Env: LLVM_BIN, PS5_PAYLOAD_SDK (v0.45), PS5_HBROOT (pacbrew v0.37 homebrew sysroot),
#      FW_TOOLS (dir with zlib-1.3.1 source), PS5_SDL_SRC (ps5-payload-dev/SDL @160954cf)
from pathlib import Path
import subprocess, sys, shutil, json, os, zipfile
base = Path(__file__).resolve().parent
root = base.parent
tools = Path(os.environ.get('FW_TOOLS', 'tools')).resolve()
llvm = Path(os.environ['LLVM_BIN']); sdk = Path(os.environ['PS5_PAYLOAD_SDK']); hb = Path(os.environ['PS5_HBROOT'])
sdl = Path(os.environ['PS5_SDL_SRC'])
vendor = base / 'vendor/ps5-native-tool/tooling/native'
exe = '.exe' if os.name == 'nt' else ''
def pick(env, *names):
    if os.environ.get(env): return Path(os.environ[env])
    for n in names:
        if (llvm / (n + exe)).exists(): return llvm / (n + exe)
    return llvm / (names[-1] + exe)
cc = pick('FW_CLANG', 'clang-23', 'clang-18', 'clang'); lld = pick('FW_LLD', 'ld.lld-18', 'ld.lld')
hcc = pick('FW_HOST_CC', 'clang'); hcxx = pick('FW_HOST_CXX', 'clang++')
# Only the system libraries a game title may load. The SDK also ships libkernel_sys/libkernel_web and
# WebKit stubs; if the linker picks those, the title is rejected at launch.
stubs = Path(os.environ.get('PS5_TITLE_STUBS', '/opt/ps5/stubs'))
build = base / os.environ.get('QB_BUILD', 'build'); (build / 'host').mkdir(parents=True, exist_ok=True); (build / 'obj').mkdir(exist_ok=True)

def run(cmd):
    r = subprocess.run([str(x) for x in cmd], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.stdout.strip(): print(r.stdout, flush=True)
    if r.returncode: raise RuntimeError('Command failed: ' + str(cmd[0]))

def host_tool():
    z = tools / 'zlib-1.3.1'; zobj = []
    for n in 'adler32 crc32 deflate inflate infback inffast inftrees trees zutil compress uncompr'.split():
        o = build / 'host' / (n + '.o'); zobj.append(o)
        if not o.exists(): run([hcc, '-O2', '-I' + str(z), '-c', z / (n + '.c'), '-o', o])
    t = build / ('host/ps5-native-tool' + exe)
    if not t.exists():
        objs = []
        for n in ['native_app_builder', 'self_container', 'elf_object', 'sce_module_writer']:
            o = build / 'host' / (n + '.o'); objs.append(o)
            run([hcxx, '-std=c++20', '-O2', '-I' + str(z), '-c', vendor / (n + '.cpp'), '-o', o])
        run([hcxx] + (['-static'] if os.name == 'nt' else []) + ['-o', t] + objs + zobj)
    return t
tool = host_tool()

common = [cc, '--no-default-config', '-target', 'x86_64-sie-ps5', '-std=c++17', '-O2', '-DQURAN_PS5=1',
          '-fexceptions', '-fcxx-exceptions', '-fno-stack-protector', '-fno-plt', '-femulated-tls',
          '-fvisibility-nodllstorageclass=default', '-ffunction-sections', '-fdata-sections',
          '-isysroot', sdk, '-nostdinc++', '-isystem', sdk / 'target/include/c++/v1', '-isystem', sdk / 'target/include',
          '-idirafter', hb / 'include', '-I' + str(hb / 'include/SDL2'), '-DSTBI_NO_THREAD_LOCALS', '-DNDEBUG'] + os.environ.get('QB_DEFS', '').split()
objects = []
for src, name in [(vendor / 'app_crt.cpp', 'app_crt'), (Path(os.environ.get('QB_SRC', str(root / 'src/main.c'))), 'main'),
                  (base / 'vendor/ps5_compat.cpp', 'ps5_compat'), (base / 'native_shims.cpp', 'native_shims'), (base / 'quran_shims.cpp', 'quran_shims')]:
    o = build / 'obj' / (name + '.o'); objects.append(o)
    run(common + (['-x', 'c++'] if src.suffix == '.c' else []) + ['-c', src, '-o', o])
video = build / 'obj/SDL_ps5video.o'
run([cc, '--no-default-config', '--target=x86_64-sie-ps5', '-O2', '-fno-stack-protector', '-fno-plt', '-ffunction-sections',
     '-fdata-sections', '-isysroot', sdk, '-isystem', sdk / 'target/include', '-I' + str(hb / 'include/SDL2'),
     '-I' + str(sdl / 'src/video/ps5'), '-I' + str(sdl / 'src'), '-I' + str(sdl / 'include'),
     '-c', base / 'vendor/sdl-ps5video/SDL_ps5video.c', '-o', video])
objects.append(video)
wrap = 'sceKeyboardInit sceKeyboardOpen sceKeyboardReadState sceKeyboardClose sceImeDialogGetStatus connect fcntl malloc free calloc realloc posix_memalign'.split()
pie = build / 'llvm-pie.elf'
run([lld, '-m', 'elf_x86_64', '-pie', '-T', vendor / 'ps5-pie.ld', '--eh-frame-hdr', '-z', 'max-page-size=0x4000',
     '--hash-style=gnu', '--gc-sections', '--version-script', vendor / 'app-symbols.map', '-e', '_start', '-o', pie]
    + objects + ['--wrap=' + n for n in wrap]
    + ['-L' + str(hb / 'lib'), '-L' + str(sdk / 'target/lib'), '--start-group', '-lSDL2', '-lcurl', '-lpsl', '-lssl', '-lcrypto',
       '-lzstd', '-lz', '-lsamplerate', '-lexpat', '-liconv', '-lc++', '-lc++abi', '-lunwind', '--end-group', '--as-needed']
    + sorted(stubs.glob('*.so')))
run([tool, 'link', '--in', pie, '--out', build / 'eboot.elf', '--stub-dir', stubs,
     '--module-sdk', '0x02000009', '--companion-sdk', '0x08050001', '--file-name', 'eboot.elf'])

param = json.loads((base / 'sce_sys/param.json').read_text(encoding='utf-8'))
if os.environ.get('QB_TITLE'):
    t = os.environ['QB_TITLE']; param['titleId'] = t
    param['contentId'] = 'IV9999-' + t + '_00-' + (t + '0000000000000000')[:16]
    param['localizedParameters'] = {'defaultLanguage': 'en-US', 'en-US': {'titleName': os.environ.get('QB_NAME', t)}}
tid = param['titleId']
title = base / tid
shutil.rmtree(title, ignore_errors=True)
(title / 'sce_sys').mkdir(parents=True); (title / 'sce_module').mkdir()
run([tool, 'self', '--sign', '--in', build / 'eboot.elf', '--out', title / 'eboot.bin', '--magic', '0x1D3D154F'])
shutil.copyfile(base / 'sce_sys/icon0.png', title / 'sce_sys/icon0.png')
(title / 'sce_sys/param.json').write_text(json.dumps(param, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
shutil.copytree(base / 'assets', title / 'assets')
shutil.copytree(root / 'assets' / 'ui', title / 'assets' / 'ui')
shutil.copyfile(base / 'runtime/libc.prx', title / 'sce_module/libc.prx')
shutil.copyfile(base / 'LICENSE', title / 'LICENSE')
shutil.copyfile(root / 'NOTICE.md', title / 'NOTICE.md')
if '--zip' in sys.argv:
    ver = '.'.join(str(int(x)) for x in param['contentVersion'].split('.'))
    out = base / f'{tid}-v{ver}.zip'
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for f in sorted(title.rglob('*')):
            if f.is_file(): z.write(f, f.relative_to(base).as_posix())
    print('Packaged ' + str(out), flush=True)
print('Built native title folder ' + str(title), flush=True)

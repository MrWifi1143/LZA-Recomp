#!/usr/bin/env python3
# build.py: the Windows exe built natively on Windows (or anywhere zig runs) - no make, no MSYS2, no bash. What the
# Makefile's PLATFORM=windows rules + tools/windows/build_exe.sh do, with zig as the compiler, linker and resource
# compiler: build/win/<exe>.exe and its import library build/win/lasrecomp.dll.a (the game code the start screen builds
# imports the runtime's functions through it), plus the Steam relay plugin where this branch has it.
#
#   python tools/windows/build.py                     build (incremental; -j <jobs>, default all cores)
#   python tools/windows/build.py --install <folder>  ... then copy the exe (+ plugin) into an installed release
#                                                     folder - the quick way to play a change (the old exe kept as .prev)
#
# Needs: Python 3.8+, and LZA_WINBUILD (or --winbuild) pointing at the build folder of tools/windows/README.md - only its
# deps/ (SDL3, shaderc, FFmpeg headers and import libraries) and zig (zig-windows/ or zig-windows.zip, unpacked here on
# first use; or --zig / ZIG / zig on PATH) are used. Run from the repository root.
import argparse, concurrent.futures as cf, glob, hashlib, os, re, shutil, subprocess, sys, time, zipfile

ROOT = os.getcwd()
B = 'build/win-native'  # (not build/win: the Linux cross build's objects stay its own)
GENFLAGS = ('-O2 -fno-strict-aliasing -fdenormal-fp-math=dynamic -Wno-return-type -Wno-unused-label -fvisibility=hidden '
            '-Iruntime -D_FILE_OFFSET_BITS=64 -march=x86-64-v3  ')  # (as make expands it, spaces and all: part of the id)
OSFLAGS = ['-D_FILE_OFFSET_BITS=64', '-D_POSIX_THREAD_SAFE_FUNCTIONS', '-D_CRT_RAND_S', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX']
LIBS = ['-lSDL3', '-lavformat', '-lavcodec', '-lavutil', '-lswscale', '-lvulkan-1', '-lwinmm', '-lole32', '-lshell32',
        '-lcomdlg32', '-luuid', '-lws2_32', '-lshaderc_shared']


def die(msg):
    print('build.py: ' + msg, file=sys.stderr)
    sys.exit(1)


def makefile_var(name):  # a := / ?= assignment of the Makefile, continuation lines joined
    text = open('Makefile', encoding='utf-8').read().replace('\\\n', ' ')
    m = re.search(r'^%s\s*[:?]?=\s*(.*)$' % name, text, re.M)
    # (make functions such as $(if $(filter aarch64,$(ARCH)),...) are other targets' parts: left out)
    return [w for w in m.group(1).split('#')[0].split() if '$' not in w and ')' not in w] if m else []


def sha16(paths, extra=b''):
    h = hashlib.sha256()
    for p in paths:
        h.update(open(p, 'rb').read())
    h.update(extra)
    return h.hexdigest()[:16]


def find_zig(args, winbuild):
    cands = [args.zig, os.environ.get('ZIG')]
    if winbuild:
        cands += glob.glob(os.path.join(winbuild, 'zig-windows', '*', 'zig.exe'))
        cands += glob.glob(os.path.join(winbuild, 'zig-linux', 'zig'))
    cands.append(shutil.which('zig'))
    for c in cands:
        if c and os.path.isfile(c):
            return c
    z = winbuild and os.path.join(winbuild, 'zig-windows.zip')
    if z and os.path.isfile(z) and os.name == 'nt':
        print('unpacking %s (once)...' % z)
        zipfile.ZipFile(z).extractall(os.path.join(winbuild, 'zig-windows'))
        return find_zig(args, winbuild)
    die('no zig: --zig <zig.exe>, ZIG=..., zig on PATH, or LZA_WINBUILD with zig-windows.zip (tools/windows/README.md)')


def run(cmd, log=None, tries=3):
    for t in range(tries):  # (Windows Defender can hold a freshly unpacked header for a moment: again)
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if r.returncode == 0:
            return True, r.stdout.decode(errors='replace')
        if t + 1 < tries:
            time.sleep(0.5 * (t + 1))
    return False, r.stdout.decode(errors='replace')


def main():
    ap = argparse.ArgumentParser(description='Native Windows build of the runtime exe (tools/windows/build.py)')
    ap.add_argument('--winbuild', default=os.environ.get('LZA_WINBUILD'))
    ap.add_argument('--zig')
    ap.add_argument('-j', '--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--install', metavar='FOLDER', help='copy the result into an installed release folder')
    args = ap.parse_args()
    if not os.path.isfile('Makefile') or not os.path.isdir('runtime'):
        die('run it from the repository root')
    if not args.winbuild or not os.path.isdir(os.path.join(args.winbuild, 'deps')):
        die('LZA_WINBUILD / --winbuild: the build folder with deps/ (tools/windows/README.md)')
    deps = os.path.join(args.winbuild, 'deps')
    zig = find_zig(args, args.winbuild)
    exe_name = (makefile_var('WIN_EXE') or ['lasrecomp'])[0]
    cc = [zig, 'cc', '-target', 'x86_64-windows-gnu', '-Wno-error=date-time']
    cxx = [zig, 'c++', '-target', 'x86_64-windows-gnu', '-Wno-error=date-time']
    env_cache = os.path.join(ROOT, B, 'zig-cache')
    os.environ.setdefault('ZIG_GLOBAL_CACHE_DIR', env_cache)
    os.environ.setdefault('ZIG_LOCAL_CACHE_DIR', env_cache)

    # what the Makefile computes in the shell
    gamelib_srcs = makefile_var('GAMELIB_SRCS')
    # (as the Makefile: + the release's profile, zig's family - make_zip.py ships it as gamelib-src/pgo.profdata -
    # so this exe loads a release's game code instead of having the start screen build it again)
    pgo_zst = sorted(glob.glob('data/pgo/*.clang21.profdata.zst'))
    gamelib_id = sha16(gamelib_srcs + pgo_zst[:1], (GENFLAGS + '\n').encode())
    gamelib_cflags = ' '.join(f for f in GENFLAGS.split() if f != '-Iruntime')
    xlat_hash = sha16(['runtime/nvn/maxwell/translate.c', 'runtime/nvn/maxwell/shader.h'])
    print('%s.exe with %s; game code id %s, translator %s' % (exe_name, zig, gamelib_id, xlat_hash))

    rtflags = ['-O2', '-g', '-fno-strict-aliasing', '-fdenormal-fp-math=dynamic', '-Wall', '-Wno-unused-function',
               '-Iruntime', '-Iruntime/nvn', '-Ithird_party', '-I' + deps + '/include'] + OSFLAGS
    cxxflags = ['-O2', '-g', '-std=c++17', '-Ithird_party/imgui', '-Ithird_party/imgui/backends', '-Iruntime',
                '-Iruntime/nvn', '-Ithird_party', '-I' + deps + '/include']
    extra = {
        'runtime/gamelib.c': ['-DGAMELIB_ID="%s"' % gamelib_id],
        'runtime/gamebuild.c': ['-DGAMELIB_ID="%s"' % gamelib_id, '-DGAMELIB_CFLAGS="%s"' % gamelib_cflags,
                                '-DGAMELIB_SHARED="-shared"'],
        'runtime/nvn/maxwell/translate.c': ['-DXLAT_SRC_HASH="%s"' % xlat_hash],
        'runtime/nvn/smaa_data.c': ['-I' + B + '/inc', '-Ithird_party/smaa/Textures'],
    }

    # SMAA's shader as a C string (the Makefile's tr / sed): CR dropped, non-ASCII as '?', each line quoted
    inc = os.path.join(B, 'inc', 'smaa_hlsl.inc')
    os.makedirs(os.path.dirname(inc), exist_ok=True)
    src = open('third_party/smaa/SMAA.hlsl', 'rb').read().replace(b'\r', b'')
    src = bytes(c if c < 128 else 63 for c in src).decode('ascii')
    lines = src.split('\n')
    if src.endswith('\n'):
        lines = lines[:-1]
    out = ''.join('"%s\\n"\n' % l.replace('\\', '\\\\').replace('"', '\\"') for l in lines)
    if not os.path.isfile(inc) or open(inc).read() != out:
        open(inc, 'w', newline='\n').write(out)

    # the objects (as the Makefile names them): rebuilt when older than their source or any runtime header
    hdrs = [h for p in ('runtime/*.h', 'runtime/platform/*.h', 'runtime/nvn/*.h', 'runtime/nvn/maxwell/*.h',
                        'runtime/nvn/vulkan/*.h', 'runtime/ui/*.h') for h in glob.glob(p)]
    newest_hdr = max(os.path.getmtime(h) for h in hdrs)
    jobs = []
    rt = sorted(glob.glob('runtime/*.c') + glob.glob('runtime/hle/*.c') + glob.glob('runtime/services/*.c') +
                glob.glob('runtime/nvn/*.c') + glob.glob('runtime/nvn/maxwell/*.c') + ['runtime/platform/windows.c'] +
                glob.glob('runtime/nvn/vulkan/*.c'))
    for s in rt:
        s = s.replace('\\', '/')
        jobs.append((s, B + '/rt/' + s[len('runtime/'):-2] + '.o', cc + rtflags + extra.get(s, []), True))
    for s in sorted(glob.glob('runtime/nvn/vulkan/*.cpp')):
        s = s.replace('\\', '/')
        jobs.append((s, B + '/rt/' + s[len('runtime/'):-4] + '.o', cxx + cxxflags + OSFLAGS, True))
    for s in sorted(glob.glob('runtime/ui/*.mm')):
        s = s.replace('\\', '/')
        jobs.append((s, B + '/rt/' + s[len('runtime/'):-3] + '.o', cxx + cxxflags + ['-x', 'c++'] + OSFLAGS, True))
    for n in ('imgui', 'imgui_draw', 'imgui_tables', 'imgui_widgets'):
        jobs.append(('third_party/imgui/%s.cpp' % n, B + '/imgui/%s.o' % n, cxx + cxxflags, False))
    for n in ('sdl3', 'vulkan'):
        jobs.append(('third_party/imgui/backends/imgui_impl_%s.cpp' % n, B + '/imgui/imgui_impl_%s.o' % n, cxx + cxxflags, False))

    def stale(src, obj, uses_hdrs):
        if not os.path.isfile(obj):
            return True
        t = os.path.getmtime(obj)
        return os.path.getmtime(src) > t or (uses_hdrs and newest_hdr > t) or os.path.getmtime(__file__) > t

    todo = [j for j in jobs if stale(*j[0:2], j[3])]
    # (the ids are in the sources' flags, not their text: a changed id rebuilds those three)
    stamp = os.path.join(B, 'ids.txt')
    ids = '%s %s %s' % (gamelib_id, xlat_hash, gamelib_cflags)
    if not os.path.isfile(stamp) or open(stamp).read() != ids:
        todo += [j for j in jobs if j[0] in extra and j not in todo]
    print('%d of %d objects to compile, %d at a time' % (len(todo), len(jobs), args.jobs))

    def compile_one(j):
        src, obj, cmd, _ = j
        os.makedirs(os.path.dirname(obj), exist_ok=True)
        ok, out = run(cmd + ['-c', src, '-o', obj])
        return src, ok, out

    failed = []
    with cf.ThreadPoolExecutor(args.jobs) as ex:
        for src, ok, out in ex.map(compile_one, todo):
            if out.strip():
                print(out.rstrip())
            if not ok:
                failed.append(src)
    if failed:
        die('%d failed: %s' % (len(failed), ' '.join(failed)))
    open(stamp, 'w').write(ids)

    # the icon, manifest and version info
    res = B + '/rt/platform/windows.res'
    ok, out = run([zig, 'rc', '/i', 'docs/icon', '/i', 'runtime/platform', '/fo', res, 'runtime/platform/windows.rc'])
    if not ok:
        die('resources: ' + out)

    # link: zig c++ shows its lld-link line (-v); that line is run again with every runtime function exported and the
    # import library the game code links with (zig c++ drops --export-all-symbols / --out-implib: build_exe.sh)
    objs = [j[1] for j in jobs]
    os.makedirs(B + '/link', exist_ok=True)
    tmp = B + '/link/%s.exe' % exe_name
    ok, log = run(cxx + ['-o', tmp] + objs + ['-fuse-ld=lld', '-Wl,--stack,16777216', '-L' + deps + '/lib'] + LIBS + ['-v'])
    line = next((l for l in log.splitlines() if 'lld-link' in l and '-OUT:' in l), None)
    if not ok or not line:  # (zig builds its C runtime here the first time: a drive that drops files under load fails it)
        die('link failed:\n' + log[-3000:])
    a = line[line.index('lld-link'):].split()
    keep = []
    for x in a:
        if x == '-DEBUG' or x.startswith('-PDB:') or x.startswith('-PDBALTPATH:') or x.upper().startswith('-SUBSYSTEM:'):
            continue
        if x.startswith('-IMPLIB:'):
            keep += ['-IMPLIB:' + B + '/lasrecomp.dll.a', '-export-all-symbols']
            continue
        if x.startswith('-OUT:'):
            x = '-OUT:' + B + '/%s.exe' % exe_name
        keep.append(x)
    keep += [res, '-SUBSYSTEM:windows']  # (a windowed program: no console window, the log in lasrecomp.log; build_exe.sh)
    ok, out = run([zig] + keep, tries=1)
    if not ok:
        die('relink failed:\n' + out[-3000:])
    print('built %s/%s.exe and %s/lasrecomp.dll.a' % (B, exe_name, B))

    # the Steam relay plugin (Z-A's branch)
    plugin = None
    if os.path.isfile('contrib/steamrelay/steamrelay.c'):
        plugin = B + '/plugins/steamrelay.dll'
        os.makedirs(os.path.dirname(plugin), exist_ok=True)
        ok, out = run(cc + ['-O2', '-g', '-Wall', '-shared', '-Iruntime', '-Ithird_party', '-I' + deps + '/include',
                            'contrib/steamrelay/steamrelay.c', '-o', plugin, '-lws2_32', '-ladvapi32'])
        if not ok:
            die('steamrelay: ' + out)
        print('built ' + plugin)
        helper = B + '/plugins/steamrelay-helper.exe'  # (the Steam API in a process of its own: contrib/steamrelay)
        ok, out = run(cc + ['-O2', '-g', '-Wall', '-DRELAY_HELPER', 'contrib/steamrelay/steamrelay.c', '-o', helper, '-ladvapi32'])
        if not ok:
            die('steamrelay-helper: ' + out)
        print('built ' + helper)

    # Legends Lua (contrib/legendslua): Lua scripting of the game - shipped, off until switched on in the Mods page
    lua = None
    if os.path.isfile('contrib/legendslua/legendslua.c'):
        lua = B + '/plugins/legendslua.dll'
        os.makedirs(os.path.dirname(lua), exist_ok=True)
        ok, out = run(cc + ['-O2', '-g', '-Wall', '-shared', '-Iruntime', '-Ithird_party', '-I' + deps + '/include',
                            'contrib/legendslua/legendslua.c'] + sorted(glob.glob('third_party/lua/*.c')) + ['-o', lua])
        if not ok:
            die('legendslua: ' + out)
        print('built ' + lua)

    if args.install:
        d = args.install
        if not os.path.isfile(os.path.join(d, exe_name + '.exe')):
            die('%s has no %s.exe: not an installed release folder' % (d, exe_name))
        def put(src, dst):  # (a running exe / loaded DLL can't be written over, but can be renamed: moved aside first)
            if os.path.isfile(dst):
                if os.path.isfile(dst + '.prev'):
                    try:
                        os.remove(dst + '.prev')
                    except OSError:
                        pass
                os.replace(dst, dst + '.prev')
            shutil.copy2(src, dst)
        put(B + '/%s.exe' % exe_name, os.path.join(d, exe_name + '.exe'))
        shutil.copy2(B + '/lasrecomp.dll.a', d)
        if plugin:
            os.makedirs(os.path.join(d, 'plugins'), exist_ok=True)
            put(plugin, os.path.join(d, 'plugins', 'steamrelay.dll'))
            put(B + '/plugins/steamrelay-helper.exe', os.path.join(d, 'plugins', 'steamrelay-helper.exe'))
        if lua:
            os.makedirs(os.path.join(d, 'plugins'), exist_ok=True)
            put(lua, os.path.join(d, 'plugins', 'legendslua.dll'))
        print('installed into %s (the previous exe: %s.exe.prev)' % (d, exe_name))


if __name__ == '__main__':
    main()

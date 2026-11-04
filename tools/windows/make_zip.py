#!/usr/bin/env python3
# make_zip.py: the Windows release zip made on Windows - what tools/windows/make_zip.sh does on Linux, from build.py's
# output (build/win-native/<exe>.exe + lasrecomp.dll.a + plugins/): <out>/LZA-Recomp-<tag>/ and LZA-Recomp-<tag>-windows.zip.
# No game files, no game code: the start screen generates and compiles that on the player's PC from their own dump
# (runtime/gamesetup.c, runtime/gamebuild.c).
#
#   python tools/windows/build.py && python tools/windows/make_zip.py [--out <folder>]   (default: $LZA_WINBUILD/dist)
#
# Needs LZA_WINBUILD (or --winbuild) with deps/bin, python-embed.zip, wheels/ and zig-windows.zip (tools/windows/README.md).
# Run from the repository root.
import argparse, glob, os, re, shutil, struct, subprocess, sys, zipfile

B = 'build/win-native'


def die(msg):
    print('make_zip.py: ' + msg, file=sys.stderr)
    sys.exit(1)


def makefile_var(name):
    text = open('Makefile', encoding='utf-8').read().replace('\\\n', ' ')
    m = re.search(r'^%s\s*[:?]?=\s*(.*)$' % name, text, re.M)
    return [w for w in m.group(1).split('#')[0].split() if '$' not in w and ')' not in w] if m else []


def pe_imports(path):  # the DLL names a PE file imports (its import directory)
    b = open(path, 'rb').read()
    pe = struct.unpack_from('<I', b, 0x3c)[0]
    nsec, optsz = struct.unpack_from('<H', b, pe + 6)[0], struct.unpack_from('<H', b, pe + 20)[0]
    opt = pe + 24
    dd = opt + (112 if struct.unpack_from('<H', b, opt)[0] == 0x20b else 96)  # (PE32+ / PE32 data directories)
    rva = struct.unpack_from('<I', b, dd + 8)[0]  # (directory 1: imports)
    secs = [struct.unpack_from('<IIII', b, opt + optsz + 40 * i + 8) for i in range(nsec)]  # vsize, va, rawsize, rawoff

    def off(r):
        for vs, va, rs, ro in secs:
            if va <= r < va + max(vs, rs):
                return r - va + ro
        return None

    names, o = [], off(rva) if rva else None
    while o is not None:
        name_rva = struct.unpack_from('<I', b, o + 12)[0]
        if not name_rva:
            break
        n = off(name_rva)
        names.append(b[n:b.index(b'\0', n)].decode())
        o += 20
    return names


def crlf(path):  # (line ends for Notepad)
    t = open(path, 'rb').read().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')
    open(path, 'wb').write(t)


def main():
    ap = argparse.ArgumentParser(description='Windows release zip (tools/windows/make_zip.py)')
    ap.add_argument('--winbuild', default=os.environ.get('LZA_WINBUILD'))
    ap.add_argument('--out')
    args = ap.parse_args()
    if not os.path.isfile('Makefile') or not os.path.isdir('runtime'):
        die('run it from the repository root')
    W = args.winbuild
    if not W or not os.path.isdir(os.path.join(W, 'deps', 'bin')):
        die('LZA_WINBUILD / --winbuild: the build folder (tools/windows/README.md)')
    tag, exe = (makefile_var('WIN_TAG') or ['ZA'])[0], (makefile_var('WIN_EXE') or ['lasrecomp'])[0]
    if not os.path.isfile(os.path.join(B, exe + '.exe')):
        die('no %s/%s.exe: run tools/windows/build.py first' % (B, exe))
    out = args.out or os.path.join(W, 'dist')
    D = os.path.join(out, 'LZA-Recomp-' + tag)
    shutil.rmtree(D, ignore_errors=True)
    os.makedirs(os.path.join(D, 'licenses'))
    for f in (exe + '.exe', 'lasrecomp.dll.a'):
        shutil.copy(os.path.join(B, f), D)
    # the DLLs the exe needs, and theirs (not the Vulkan loader: every GPU driver installs it)
    bin_ = os.path.join(W, 'deps', 'bin')
    avail = {f.lower(): f for f in os.listdir(bin_)}
    todo, seen = [exe + '.exe'], set()
    while todo:
        for d in pe_imports(os.path.join(D, todo.pop())):
            d = d.lower()
            if d in seen or d not in avail or d == 'vulkan-1.dll':
                continue
            seen.add(d)
            shutil.copy(os.path.join(bin_, avail[d]), D)
            todo.append(avail[d])
    # the game code generator: portable Python + capstone / unicorn / lz4 + tools/gen.py and its modules, only the parts
    # the generator uses (no network, SSL, SQLite, XML, asyncio or multiprocessing modules, no static libraries)
    P = os.path.join(D, 'python')
    os.makedirs(os.path.join(P, 'Lib', 'site-packages'))
    os.makedirs(os.path.join(D, 'gen'))
    zipfile.ZipFile(os.path.join(W, 'python-embed.zip')).extractall(P)
    for m in ('_asyncio _bz2 _decimal _elementtree _hashlib _lzma _msi _multiprocessing _overlapped pyexpat _queue select '
              '_socket _sqlite3 _ssl unicodedata _uuid _wmi _zoneinfo winsound').split():
        if os.path.exists(os.path.join(P, m + '.pyd')):
            os.remove(os.path.join(P, m + '.pyd'))
    for f in ('libcrypto-3.dll', 'libssl-3.dll', 'sqlite3.dll'):
        if os.path.exists(os.path.join(P, f)):
            os.remove(os.path.join(P, f))
    for w in sorted(glob.glob(os.path.join(W, 'wheels', '*.whl'))):
        zipfile.ZipFile(w).extractall(os.path.join(P, 'Lib', 'site-packages'))
    sp = os.path.join(P, 'Lib', 'site-packages')
    if os.path.exists(os.path.join(sp, 'unicorn', 'lib', 'unicorn.lib')):
        os.remove(os.path.join(sp, 'unicorn', 'lib', 'unicorn.lib'))
    for d in ('unicorn/include', 'capstone/include'):
        shutil.rmtree(os.path.join(sp, d), ignore_errors=True)
    pth = glob.glob(os.path.join(P, 'python3*._pth'))
    if not pth:
        die('python-embed.zip has no python3*._pth')
    zipname = os.path.basename(pth[0])[:-5] + '.zip'
    open(pth[0], 'w', newline='').write('%s\n.\nLib\\site-packages\n..\\gen\n' % zipname)
    for m in ('gen', 'lifter', 'analyze', 'nso', 'passes', 'patches', 'simd_c'):
        shutil.copy('tools/%s.py' % m, os.path.join(D, 'gen'))
    # the compiler: zig for Windows, only what compiling C for x86-64 Windows needs
    zipfile.ZipFile(os.path.join(W, 'zig-windows.zip')).extractall(D)
    zdir = glob.glob(os.path.join(D, 'zig-x86_64-windows-*'))[0]
    os.rename(zdir, os.path.join(D, 'zig'))
    L = os.path.join(D, 'zig', 'lib')
    for d in glob.glob(os.path.join(L, 'libc', 'include', '*')):
        if not re.search('windows|mingw', os.path.basename(d)):
            shutil.rmtree(d, ignore_errors=True) if os.path.isdir(d) else os.remove(d)
    for d in glob.glob(os.path.join(L, 'libc', '*')):
        if os.path.basename(d) not in ('include', 'mingw'):
            shutil.rmtree(d, ignore_errors=True) if os.path.isdir(d) else os.remove(d)
    for d in ('libc/mingw/lib32', 'libc/mingw/libarm32', 'libc/mingw/libarm64', 'libcxx', 'libcxxabi', 'libtsan', 'libunwind',
              'compiler', 'init', 'build-web', 'docs'):
        shutil.rmtree(os.path.join(L, d), ignore_errors=True)
    shutil.rmtree(os.path.join(D, 'zig', 'doc'), ignore_errors=True)
    if os.path.exists(os.path.join(D, 'zig', 'README.md')):
        os.remove(os.path.join(D, 'zig', 'README.md'))
    G = os.path.join(D, 'gamelib-src')
    os.makedirs(G)
    for f in ('runtime/cpu.h', 'runtime/simd.h', 'runtime/simd_fp.h', 'runtime/gamelib.h', 'tools/gamelib_entry.c',
              'runtime/hle/hle_symbols.txt'):
        shutil.copy(f, G)
    for z in glob.glob('data/pgo/*.clang21.profdata.zst'):  # (the optimiser's profile, zig's family: Makefile PGO)
        subprocess.run(['zstd', '-q', '-d', '-f', z, '-o', os.path.join(G, 'pgo.profdata')], check=True)
    shutil.copy('docs/icon/icon_1024.png', os.path.join(D, 'icon.png'))
    shutil.copy('third_party/fonts/Baloo2/Baloo2-SemiBold.ttf', D)
    if os.path.isfile('data/shader_manifest.txt'):  # (the warm-up's first programs: runtime/nvn/vulkan/warmup.c)
        shutil.copy('data/shader_manifest.txt', D)
    if os.path.isdir('contrib/steamrelay'):  # (the Steam relay plugin: shipped, off until switched on in the Mods page)
        plug = os.path.join(B, 'plugins', 'steamrelay.dll')
        if not os.path.isfile(plug):
            die('no %s: build.py builds it' % plug)
        os.makedirs(os.path.join(D, 'plugins'))
        shutil.copy(plug, os.path.join(D, 'plugins'))
    shutil.copy('docs/release-README.txt', os.path.join(D, 'README.txt'))
    # licenses: this project's, then every bundled component's (THIRD-PARTY-NOTICES.md says which is which)
    Ls = os.path.join(D, 'licenses')
    shutil.copy('LICENSE', os.path.join(D, 'LICENSE.txt'))
    shutil.copy('LICENSE-LASRecomp', os.path.join(Ls, 'LICENSE-LASRecomp.txt'))
    shutil.copy('THIRD-PARTY-NOTICES.md', os.path.join(Ls, 'THIRD-PARTY-NOTICES.txt'))
    for f in glob.glob('third_party/licenses/*'):
        shutil.copy(f, Ls)
    for src, dst in (('third_party/imgui/LICENSE.txt', 'imgui-LICENSE.txt'),
                     ('third_party/vma/LICENSE.txt', 'VulkanMemoryAllocator-LICENSE.txt'),
                     ('third_party/smaa/LICENSE.txt', 'SMAA-LICENSE.txt'), ('third_party/fonts/Baloo2/OFL.txt', 'Baloo2-OFL.txt'),
                     (os.path.join(P, 'LICENSE.txt'), 'Python-LICENSE.txt'), (os.path.join(D, 'zig', 'LICENSE'), 'zig-LICENSE.txt'),
                     (glob.glob(os.path.join(sp, 'capstone-*.dist-info', 'LICENSE.TXT'))[0], 'capstone-LICENSE.txt'),
                     (glob.glob(os.path.join(sp, 'lz4-*.dist-info', 'licenses', 'LICENSE'))[0], 'python-lz4-LICENSE.txt'),
                     ('third_party/licenses/GPL-2.0.txt', 'unicorn-COPYING-GPL-2.0.txt')):
        shutil.copy(src, os.path.join(Ls, dst))
    # nothing from the game: no game code library, no game files
    bad = []
    for dp, dns, fns in os.walk(D):
        for n in dns + fns:
            l = n.lower()
            if l.startswith('lasgame') or l.endswith(('.nso', '.nca', '.nsp', '.xci')) or l in ('prod.keys', 'title.keys') \
                    or (n in ('exefs', 'romfs') and n in dns):
                bad.append(os.path.join(dp, n))
    if bad:
        die('game files in the release folder: ' + ', '.join(bad))
    for f in [os.path.join(D, 'README.txt'), os.path.join(D, 'LICENSE.txt')] + glob.glob(os.path.join(Ls, '*')):
        crlf(f)
    z = os.path.join(out, 'LZA-Recomp-%s-windows.zip' % tag)
    if os.path.exists(z):
        os.remove(z)
    total = 0
    with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for dp, dns, fns in os.walk(D):
            for f in sorted(fns):
                p = os.path.join(dp, f)
                total += os.path.getsize(p)
                zf.write(p, os.path.relpath(p, out).replace('\\', '/'))
    print('%s: %.0f MB in the folder, zip %.0f MB' % (z, total / 1e6, os.path.getsize(z) / 1e6))


if __name__ == '__main__':
    main()

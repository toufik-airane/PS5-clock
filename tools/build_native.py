#!/usr/bin/env python3
"""Build the standalone native clock with the public native app kit; never deploy."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile

STARTER_URL = 'https://github.com/MiladRTX/ps5-homebrew-starter.git'
STARTER_COMMIT = 'c3f939230aee18863da1f359e39eb43bf95a056d'
ROOT = Path(__file__).resolve().parents[1]
NATIVE_SOURCES = (
    'native_main.c', 'native_platform.c', 'native_platform.h',
    'scene.c', 'scene.h', 'native_lifecycle.c',
)
SOURCE_FILES = (
    'README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md', '.gitignore',
    *NATIVE_SOURCES, 'vendor/stb_truetype.h',
    'assets/Outfit-Light.ttf', 'assets/OFL.txt', 'assets/preview.png',
    'sce_sys/param.json', 'sce_sys/icon0.png',
    'tools/build_native.py', 'tools/embed_font.py',
)


def executable(value, candidates, path=None):
    """Accept either an executable path or a command name, including overrides."""
    for name in ([value] if value else candidates):
        found = shutil.which(str(Path(name).expanduser()), path=path)
        if found:
            return str(Path(found).absolute())
    raise SystemExit('Missing executable: ' + (value or ' / '.join(candidates)))


def source_zip():
    # An explicit allowlist excludes caches, previews, logs and local files.
    for name in SOURCE_FILES:
        if not (ROOT / name).is_file() or (ROOT / name).is_symlink():
            raise SystemExit('Missing regular source file: ' + name)
    destination = ROOT / 'dist/AuroraClock-Source.zip'
    destination.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(destination, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for name in sorted(SOURCE_FILES):
            entry = zipfile.ZipInfo('AuroraClock/' + name, (1980, 1, 1, 0, 0, 0))
            entry.create_system = 3
            entry.external_attr = 0o100644 << 16
            entry.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(entry, (ROOT / name).read_bytes())
    print('Source archive:', destination)


def build(args):
    kit = args.kit.expanduser().resolve()
    for name in ('tools/setup-native-dependencies.sh', 'tools/build.sh',
                 'runtime/libc.prx.sha256'):
        if not (kit / name).is_file():
            raise SystemExit('Not a native app kit: missing ' + str(kit / name))
    clang = executable(args.clang, ['clang-18', 'clang'])
    llvm_config = executable(args.llvm_config, ['llvm-config-18', 'llvm-config'])
    # llvm-config may live in /usr/bin while its matching tools live elsewhere.
    llvm_bin = Path(subprocess.check_output([llvm_config, '--bindir'], text=True).strip())
    env = dict(os.environ)
    env['PATH'] = os.pathsep.join((str(llvm_bin), str(Path(clang).parent),
                                   env.get('PATH', os.defpath)))
    bash = executable(None, ['bash'], env['PATH'])
    subprocess.run([bash, '-c',
                    '(( BASH_VERSINFO[0] > 4 || '
                    '(BASH_VERSINFO[0] == 4 && BASH_VERSINFO[1] >= 4) )) || '
                    '{ echo "Bash 4.4+ is required; put it first on PATH." >&2; exit 2; }'],
                   check=True, env=env)
    for name in ('python3', 'wget', 'unzip', 'sha256sum', 'tar', 'make',
                 'ninja', 'sort', 'cmp', 'realpath', 'readlink'):
        executable(None, [name], env['PATH'])
    subprocess.run(['sort', '-z'], input='', text=True, check=True, env=env)
    llvm_ar = executable(str(llvm_bin / 'llvm-ar'), [], env['PATH'])
    llvm_readelf = executable(str(llvm_bin / 'llvm-readelf'), [], env['PATH'])
    executable(str(llvm_bin / 'llvm-ranlib'), [], env['PATH'])
    linker = llvm_bin / 'ld.lld'
    # SDK v0.42 also supports Homebrew's separately packaged lld.
    if sys.platform == 'darwin' and not os.access(linker, os.X_OK):
        brew = executable(None, ['brew'], env['PATH'])
        prefix = subprocess.check_output([brew, '--prefix', 'lld'], text=True, env=env).strip()
        linker = Path(prefix) / 'bin/ld.lld'
    executable(str(linker), [], env['PATH'])
    cxx = executable(os.environ.get('CXX'), ['clang++-18', 'clang++'], env['PATH'])
    # SDK stubs overlap. Locale collation otherwise puts libkernel_stub_weak.so
    # before libkernel.so, selecting libkernel_web's wrong native import namespace.
    env.update(PS5_CLANG=clang, LLVM_CONFIG=llvm_config, CXX=cxx,
               USE_CCACHE='0', LC_ALL='C')

    def run(command, **kwargs):
        subprocess.run([str(part) for part in command], check=True, env=env, **kwargs)

    run([bash, kit / 'tools/setup-native-dependencies.sh'])
    sdk = kit / '.deps/native/ps5-payload-sdk'
    # Only delete the unique staging directory created by this invocation.
    # Existing kit/aurora trees and user files are never migrated or removed.
    with tempfile.TemporaryDirectory(prefix='aurora-stage-', dir=kit) as directory:
        stage = Path(directory)
        (stage / 'src/vendor').mkdir(parents=True)
        for name in NATIVE_SOURCES:
            shutil.copy2(ROOT / name, stage / 'src' / name)
        shutil.copy2(ROOT / 'vendor/stb_truetype.h', stage / 'src/vendor/stb_truetype.h')
        shutil.copytree(ROOT / 'sce_sys', stage / 'sce_sys')
        run([sys.executable, ROOT / 'tools/embed_font.py',
             ROOT / 'assets/Outfit-Light.ttf', stage / 'src/font_data.h'])
        compat = stage / 'compat'
        compat.mkdir()
        # scene/stb require __assert; localtime_r delegates to native localtime_s.
        # assert.o also needs __stderrp, abort and fprintf from the pinned runtime.
        run([llvm_ar, 'x', sdk / 'target/lib/libc.a', 'assert.o', 'localtime.o'], cwd=compat)
        run([llvm_ar, 'rcs', 'compat.a', 'assert.o', 'localtime.o'], cwd=compat)
        # Keep the standalone build independent of unrelated kit app overrides.
        for key in list(env):
            if key.startswith(('APP_', 'PACBREW_')) or key == 'BUILD_LABEL':
                del env[key]
        relative = stage.name
        env.update(APP_SOURCE_DIR=relative + '/src',
                   APP_PARAM=relative + '/sce_sys/param.json',
                   APP_SCE_SYS=relative + '/sce_sys', APP_ASSETS='',
                   APP_INCLUDE_PATHS=relative + '/src',
                   APP_STATIC_ARCHIVES=relative + '/compat/compat.a')
        # The kit validates the pinned libc.prx hash and signs/inspects the title.
        run([bash, kit / 'tools/build.sh'])
    dynamic = subprocess.check_output([llvm_readelf, '-d', str(kit / 'build/llvm-pie.elf')],
                                      text=True, env=env)
    for forbidden in ('libScePosixForWebKit', 'libkernel_web', 'libkernel_sys'):
        if forbidden in dynamic:
            raise SystemExit('Unsafe native title import provider: ' + forbidden)
    title = json.loads((ROOT / 'sce_sys/param.json').read_text(encoding='utf-8'))['titleId']
    dist = ROOT / 'dist'
    dist.mkdir(exist_ok=True)
    shutil.copytree(kit / 'dist' / title, dist / title, dirs_exist_ok=True)
    shutil.copy2(kit / 'dist' / (title + '.zip'), dist / 'AuroraClock-PS5.zip')
    receipt = {str(p.relative_to(dist / title)): hashlib.sha256(p.read_bytes()).hexdigest()
               for p in sorted((dist / title).rglob('*')) if p.is_file()}
    (dist / 'SHA256.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print('Built native Games title:', dist / title)
    print('No console connection or system-setting changes were made.')


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog=f'Verified starter: {STARTER_URL} at {STARTER_COMMIT} (use its kit directory).')
    parser.add_argument('--kit', type=Path, help='Path to the public starter kit directory')
    parser.add_argument('--clang', default=os.environ.get('PS5_CLANG'),
                        help='Clang executable path or name (default: clang-18, then clang)')
    parser.add_argument('--llvm-config', default=os.environ.get('LLVM_CONFIG'),
                        help='LLVM config path or name (default: llvm-config-18, then llvm-config)')
    parser.add_argument('--source-zip', action='store_true',
                        help='Only create dist/AuroraClock-Source.zip; no kit or compiler needed')
    args = parser.parse_args()
    if args.source_zip:
        source_zip()
    elif args.kit is None:
        parser.error('--kit is required unless --source-zip is used')
    else:
        build(args)


if __name__ == '__main__':
    main()

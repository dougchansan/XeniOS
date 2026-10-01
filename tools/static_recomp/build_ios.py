#!/usr/bin/env python3
"""Build a strict, prelinked iOS app on a Mac; does not provision or install it.

Run upstream `./xb setup` first. Game-derived catalogs and resulting binaries
belong outside this public source checkout. No credentials are read or uploaded.
"""
from __future__ import annotations
import argparse
import json
import pathlib
import platform
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]


def commands(build: pathlib.Path, catalog: pathlib.Path | None, jobs: int):
    """Use the upstream host shader tool and the device-only Xcode generator."""
    configure = [
        'cmake', '-S', str(ROOT), '-B', str(build), '-G', 'Xcode',
        '-DCMAKE_SYSTEM_NAME=iOS', '-DCMAKE_SYSTEM_PROCESSOR=arm64',
        '-DCMAKE_OSX_SYSROOT=iphoneos', '-DCMAKE_OSX_ARCHITECTURES=arm64',
        '-DCMAKE_OSX_DEPLOYMENT_TARGET=18.0',
        '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY',
        '-DCMAKE_CONFIGURATION_TYPES=Release', '-DCMAKE_XCODE_GENERATE_SCHEME=ON',
        '-DXENIA_STATIC_ONLY=ON', '-DXENIA_BUILD_TESTS=OFF',
        '-DXENIA_BUILD_MISC=OFF', '-DXENIA_STATIC_SMOKE_TEST=OFF',
        '-DXENIA_ENABLE_LTO=OFF', '-DXENIA_ENABLE_IOS_MOLTENVK=OFF',
        '-DXENIA_STATIC_MODULES_DIR=' + (str(catalog) if catalog else ''),
    ]
    return [
        [str(ROOT / 'xb'), 'build', '--config=release', '--target=xenia-shader-cc'],
        configure,
        ['xcodebuild', '-project', str(build / 'xenia.xcodeproj'),
         '-scheme', 'xenia-app', '-configuration', 'Release',
         '-destination', 'generic/platform=iOS', '-jobs', str(jobs),
         'CODE_SIGNING_ALLOWED=NO', 'build'],
    ]


def external_path(path: pathlib.Path, label: str) -> pathlib.Path:
    resolved = path.expanduser().resolve()
    if resolved == ROOT or ROOT in resolved.parents:
        raise ValueError(label + ' must be outside the public source checkout')
    if ';' in str(resolved) or '\n' in str(resolved):
        raise ValueError(label + ' contains a CMake list separator or newline')
    return resolved


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument('--catalog', type=pathlib.Path)
    selection.add_argument('--empty-catalog', action='store_true',
                           help='Compile/link probe only; no game can run')
    parser.add_argument('--build-dir', type=pathlib.Path, required=True)
    parser.add_argument('--jobs', type=int, default=3)
    args = parser.parse_args(argv)
    try:
        if platform.system() != 'Darwin':
            raise ValueError('The iOS device build requires macOS and the iPhoneOS SDK')
        if not 1 <= args.jobs <= 64:
            raise ValueError('--jobs must be between 1 and 64')
        for tool in ('cmake', 'xcodebuild', 'xcrun'):
            if not shutil.which(tool):
                raise ValueError('Required build tool not found: ' + tool)
        build = external_path(args.build_dir, 'Build directory')
        if build.exists():
            raise ValueError('Use a fresh build directory; existing builds are not overwritten')
        catalog = external_path(args.catalog, 'Catalog') if args.catalog else None
        if catalog:
            info = json.loads((catalog / 'catalog.json').read_text())
            if info.get('abi') != 1 or not info.get('modules') or info.get('cpu_jit') is not False:
                raise ValueError('Not a compatible static source catalog')
            if not (catalog / 'catalog.cmake').is_file():
                raise ValueError('Missing catalog.cmake; regenerate with link.py')
        subprocess.run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], check=True)
        build.mkdir(parents=True)
        (build / '.gitignore').write_text('*\n')
        record = {'build_completed': False, 'device_tested': False,
                  'gameplay_verified': False, 'signed_for_installation': False,
                  'empty_catalog': catalog is None, 'steps': []}
        record_path = build / 'static-build-result.json'
        record_path.write_text(json.dumps(record, indent=2) + '\n')
        for index, command in enumerate(commands(build, catalog, args.jobs)):
            log = build / f'step-{index + 1}.log'
            print(f'Build step {index + 1}/3; log: {log}', flush=True)
            with log.open('w') as output:
                result = subprocess.run(command, cwd=ROOT, stdout=output,
                                        stderr=subprocess.STDOUT)
            record['steps'].append({'command': command, 'returncode': result.returncode})
            record_path.write_text(json.dumps(record, indent=2) + '\n')
            if result.returncode:
                raise ValueError(f'Build step failed; see {log}')
        app = build / 'bin/iOS/Release/XeniOS.app'
        binary = app / 'XeniOS'
        if not binary.is_file():
            raise ValueError('Xcode returned success without the expected app binary')
        symbols = subprocess.run(['xcrun', 'nm', '-C', str(binary)],
                                 check=True, capture_output=True, text=True).stdout
        (build / 'static-symbols.txt').write_text(symbols)
        if 'statik::StaticBackend' not in symbols:
            raise ValueError('Cannot positively identify the static backend in the binary')
        if re.search(r'backend::(?:a64::A64Backend|x64::X64Backend)::', symbols):
            raise ValueError('Unexpected CPU JIT backend symbols in the strict app')
        record['build_completed'] = True
        record['app_bundle'] = str(app)
        record_path.write_text(json.dumps(record, indent=2) + '\n')
        print(f'Unsigned device app: {app}')
        print('Not installed or gameplay-verified. Provision/sign separately before device testing.')
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print('iOS build failed: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())

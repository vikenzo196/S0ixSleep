#!/usr/bin/env python3
"""Build S0ixSleep.kext (x86_64 Lilu plugin).

Needs Xcode command line tools, MacKernelSDK (acidanthera/MacKernelSDK) and the headers of a
Lilu release (Lilu.kext/Contents/Resources). Default locations: ./MacKernelSDK and ./Lilu.kext.
"""
import argparse, plistlib, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NAME, VERSION, IDENT = 'S0ixSleep', '0.1.4', 'io.github.vikenzo196.S0ixSleep'


def run(cmd):
    subprocess.run([str(x) for x in cmd], check=True, cwd=ROOT)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk', type=Path, default=ROOT / 'MacKernelSDK')
    p.add_argument('--lilu', type=Path, default=ROOT / 'Lilu.kext')
    p.add_argument('--output', type=Path, default=ROOT / 'build')
    p.add_argument('--release', action='store_true')
    a = p.parse_args()
    out = a.output.resolve()
    sdk = a.sdk.resolve()
    lilu = a.lilu.resolve() / 'Contents/Resources'
    objdir = out / 'obj'
    objdir.mkdir(parents=True, exist_ok=True)
    # Same flags as other Lilu plugins: the old deployment target keeps the kernel C++ ABI that
    # Lilu and IOKit expect (no DriverKit vtable slots, no userspace stack/fortify imports).
    base = ['xcrun', 'clang++', '-arch', 'x86_64', '-std=c++17', '-O2' if a.release else '-O0', '-g',
            '-fapple-kext', '-fno-exceptions', '-fno-rtti', '-fno-stack-protector', '-fno-builtin',
            '-fno-asynchronous-unwind-tables', '-mno-red-zone', '-nostdinc', '-fno-common',
            '-mmacosx-version-min=10.6', '-DKERNEL', '-DKERNEL_PRIVATE', '-DDRIVER_PRIVATE', '-DAPPLE',
            '-DNeXT', '-DAPPLE_KEXT_ASSERTIONS=1', '-D__APPLE__', '-D__MACH__',
            '-DPRODUCT_NAME=' + NAME, '-DMODULE_VERSION=' + VERSION,
            '-I' + str(sdk / 'Headers'), '-I' + str(lilu), '-I' + str(ROOT / NAME),
            '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-deprecated-declarations', '-Wno-ossharedptr-misuse',
            # keep build machine paths out of the binary
            '-fdebug-compilation-dir=.', '-ffile-prefix-map=' + str(ROOT) + '=/' + NAME,
            '-ffile-prefix-map=' + str(sdk) + '=/MacKernelSDK', '-ffile-prefix-map=' + str(lilu) + '=/Lilu']
    if not a.release:
        base.append('-DDEBUG=1')
    objects = []
    for src in sorted((ROOT / NAME).glob('*.cpp')) + [lilu / 'Library/plugin_start.cpp']:
        obj = objdir / (src.stem + '.o')
        run(base + ['-c', src, '-o', obj])
        objects.append(obj)
    bundle = out / (NAME + '.kext') / 'Contents'
    (bundle / 'MacOS').mkdir(parents=True, exist_ok=True)
    exe = bundle / 'MacOS' / NAME
    run(['xcrun', 'ld', '-arch', 'x86_64', '-kext', '-undefined', 'dynamic_lookup', '-no_fixup_chains',
         '-macos_version_min', '10.6', '-o', exe, *objects, sdk / 'Library/x86_64/libkmod.a'])
    libs = {x: '10.0.0' for x in ['com.apple.kpi.bsd', 'com.apple.kpi.iokit', 'com.apple.kpi.libkern',
                                  'com.apple.kpi.mach', 'com.apple.kpi.unsupported']}
    libs['com.apple.iokit.IOPCIFamily'] = '1.1'
    libs['as.vit9696.Lilu'] = '1.2.0'
    info = dict(CFBundleIdentifier=IDENT, CFBundleExecutable=NAME, CFBundleName=NAME, CFBundlePackageType='KEXT',
                CFBundleInfoDictionaryVersion='6.0', CFBundleVersion=VERSION, CFBundleShortVersionString=VERSION,
                OSBundleCompatibleVersion='0.1.0', OSBundleRequired='Root', OSBundleLibraries=libs,
                IOKitPersonalities={IDENT: dict(CFBundleIdentifier=IDENT, IOClass=NAME, IOMatchCategory=NAME,
                                                IOProviderClass='IOResources', IOResourceMatch='IOKit')})
    (bundle / 'Info.plist').write_bytes(plistlib.dumps(info))
    run(['xcrun', 'dsymutil', exe, '-o', out / (NAME + '.kext.dSYM')])
    run(['xcrun', 'strip', '-S', exe])
    syms = subprocess.check_output(['xcrun', 'nm', '-u', str(exe)], text=True).split()
    assert '_kmod_info' in subprocess.check_output(['xcrun', 'nm', str(exe)], text=True), 'missing kmod_info'
    print('built', bundle.parent, '(%d undefined symbols)' % len(syms))


if __name__ == '__main__':
    main()

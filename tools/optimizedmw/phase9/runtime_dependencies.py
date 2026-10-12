"""Package-local AMD64 PE import closure, checked before launching a game.

This checks direct/delay-import presence and architecture, including installed
OSG plugins. It does not prove exports, arbitrary LoadLibrary names, driver
initialization, or startup. SDK/PATH dependencies cannot qualify a package.
PE layouts: https://learn.microsoft.com/en-us/windows/win32/debug/pe-format
"""
import mmap
import os
from pathlib import Path
import re
import struct


def imported_dlls(path):
    path = Path(path)
    with path.open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        def unpack(fmt, offset):
            size = struct.calcsize(fmt)
            if offset < 0 or offset + size > len(data):
                raise ValueError('Truncated PE metadata: ' + str(path))
            return struct.unpack_from(fmt, data, offset)

        if data[:2] != b'MZ':
            raise ValueError('Not a PE image: ' + str(path))
        pe, = unpack('<I', 0x3c)
        if data[pe:pe+4] != b'PE\0\0':
            raise ValueError('Missing PE signature: ' + str(path))
        machine, sections = unpack('<HH', pe+4)
        optional_size, = unpack('<H', pe+20)
        optional = pe+24
        magic, = unpack('<H', optional)
        if machine != 0x8664 or magic != 0x20b or not 1 <= sections <= 96 or optional_size < 112:
            raise ValueError('Expected bounded AMD64 PE32+ image: ' + str(path))
        image_base, = unpack('<Q', optional+24)
        header_size, = unpack('<I', optional+60)
        directories, = unpack('<I', optional+108)
        if directories > (optional_size-112)//8:
            raise ValueError('Truncated PE directory table: ' + str(path))
        section_table = optional+optional_size
        entries = [unpack('<IIII', section_table+index*40+8) for index in range(sections)]

        def offset(rva, size):
            if 0 <= rva < header_size and rva+size <= min(header_size, len(data)):
                return rva
            for virtual_size, address, raw_size, raw in entries:
                if address <= rva < address+max(virtual_size, raw_size):
                    relative = rva-address
                    if relative+size <= raw_size and raw+relative+size <= len(data):
                        return raw+relative
            raise ValueError('Unbacked PE import RVA: ' + str(path))

        names = set()
        for index, stride in ((1, 20), (13, 32)):
            if index >= directories:
                continue
            rva, size = unpack('<II', optional+112+index*8)
            if not rva and not size:
                continue
            if not rva or size < stride or size > 1024*1024:
                raise ValueError('Invalid PE import directory: ' + str(path))
            terminated = False
            for entry in range(min(size//stride, 512)):
                fields = unpack('<'+'I'*(stride//4), offset(rva+entry*stride, stride))
                if not any(fields):
                    terminated = True
                    break
                name_rva = fields[3] if index == 1 else fields[1]
                if index == 13:
                    if fields[0] & ~1:
                        raise ValueError('Unknown delay-import address mode: ' + str(path))
                    if not fields[0] & 1:
                        name_rva -= image_base
                chars = bytearray()
                for pos in range(256):
                    value = data[offset(name_rva+pos, 1)]
                    if value == 0:
                        break
                    chars.append(value)
                else:
                    raise ValueError('Unterminated PE DLL name: ' + str(path))
                name = chars.decode('ascii')
                if not re.fullmatch(r'[A-Za-z0-9_.-]+\.dll', name, flags=re.I):
                    raise ValueError('Invalid PE DLL basename: ' + str(path))
                names.add(name.lower())
            if not terminated:
                raise ValueError('Unterminated PE import descriptors: ' + str(path))
        return sorted(names)


def runtime_closure(package, system_directory=None, require_plugins=True):
    package = Path(package).resolve(strict=True)
    system = Path(system_directory or Path(os.environ.get('SystemRoot', r'C:\Windows'))/'System32')
    system_names = {p.name.lower(): p for p in system.iterdir() if p.is_file()}
    local_names = {p.name.lower(): p for p in package.iterdir() if p.is_file()}
    seeds = [package/'openmw.exe', *package.glob('*.dll')]
    plugins = sorted(p for d in package.glob('osgPlugins-*') if d.is_dir() for p in d.glob('*.dll'))
    if require_plugins and not plugins:
        raise ValueError('Missing packaged OSG plugin bundle')
    seeds += plugins
    visited, imports, providers = set(), {}, {}
    while seeds:
        path = seeds.pop().resolve(strict=True)
        if path in visited:
            continue
        if not path.is_relative_to(package):
            raise ValueError('Runtime provider escapes package: ' + str(path))
        visited.add(path)
        names = imported_dlls(path)
        imports[str(path.relative_to(package))] = names
        for name in names:
            if name.startswith(('api-ms-win-', 'ext-ms-win-')):
                providers[name] = 'windows_api_set'
                continue
            provider = local_names.get(name)
            if provider:
                providers[name] = str(provider.resolve().relative_to(package))
                seeds.append(provider)
            elif name in system_names:
                providers[name] = 'system32/' + system_names[name].name
            else:
                raise ValueError('Missing runtime DLL '+name+' required by '+str(path.relative_to(package)))
    return {'schema': 1, 'scope': 'package_local_amd64_direct_and_delay_imports_plus_installed_osg_plugins',
            'files': sorted(str(p) for p in visited), 'imports': imports, 'providers': providers,
            'limit': 'No SDK/PATH resolution. Presence and architecture are not export, arbitrary dynamic-load, driver or game-startup proof.'}

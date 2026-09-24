"""Conservative preflight for the manual diagnostic launch, not an INI rewriter.

Ported from the separately delivered startup-fix-1 helper. Normal configuration
is never edited, saves are never copied, and ambiguous autoload fails closed.
"""
from __future__ import annotations
import hashlib
import json
import os
from pathlib import Path
import re

COPY_NAMES = ('input_v3.xml', 'shaders.yaml', 'global_storage.bin', 'player_storage.bin')

def digest(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def normal_default() -> Path:
    if os.name == 'nt':
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                    r'Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders') as key:
                value, _ = winreg.QueryValueEx(key, 'Personal')
            return Path(os.path.expandvars(value)) / 'My Games' / 'OpenMW'
        except (OSError, ImportError):
            pass
    return Path.home() / 'Documents' / 'My Games' / 'OpenMW'


def decode_config_path(raw: str) -> str:
    """Only path-valued config entries; match OpenMW's quote/ampersand escaping."""
    raw = raw.strip()
    if not raw:
        raise ValueError('Empty config= directory; refusing an ambiguous launch.')
    if not raw.startswith('"'):
        return raw
    out, i = [], 1
    while i < len(raw):
        ch = raw[i]
        if ch == '"':
            if raw[i+1:].strip():
                raise ValueError('Unexpected text after quoted config= directory.')
            result = ''.join(out)
            if not result:
                raise ValueError('Empty quoted config= directory.')
            return result
        if ch == '&':
            i += 1
            if i >= len(raw):
                raise ValueError('Unfinished escape in config= directory.')
            ch = raw[i]
        out.append(ch)
        i += 1
    raise ValueError('Unclosed quoted config= directory.')


def config_entries(path: Path) -> list[tuple[str, str, int]]:
    if not path.exists():
        return []
    if not path.is_file() or path.stat().st_size > 4 * 1024 * 1024:
        raise ValueError(f'Cannot safely inspect configuration: {path}')
    entries, section = [], ''
    for line_no, line in enumerate(path.read_text(encoding='utf-8-sig').splitlines(), 1):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        if line.startswith('[') and line.endswith(']'):
            section = line[1:-1]
            if not section:
                raise ValueError(f'Empty configuration section: {path}:{line_no}')
            continue
        if '=' not in line:
            raise ValueError(f'Unrecognized configuration line: {path}:{line_no}')
        key, value = (part.strip() for part in line.split('=', 1))
        full_key = ((section if section.endswith('.') else section + '.') if section else '') + key
        if full_key == 'load-savegame':
            # Even an explicitly EMPTY path resolves to a directory in this binary.
            raise ValueError(f'Inherited load-savegame entry at {path}:{line_no}. '
                             'No game launched. Send this message; do not edit your normal saves.')
        entries.append((full_key, value, line_no))
    return entries


def inspect_chain(package: Path, normal: Path, platform_default: Path) -> tuple[list[Path], dict[str, str]]:
    """Conservative preflight, not a replacement for the engine's config parser.

    --replace=config suppresses automatic children of the package config. The
    explicitly selected user chain remains live and is checked before launch.
    Repeated directory aliases and unknown path tokens fail closed.
    """
    local = package / 'openmw.cfg'
    if not local.is_file() or not (normal / 'openmw.cfg').is_file():
        raise ValueError('Both the build and selected configuration must contain openmw.cfg.')
    config_entries(local)  # Check local autoload, but do not follow its automatic config= entries.
    active = [package]
    hashes = {str(local): digest(local)}
    visited = {os.path.normcase(str(package.resolve()))}
    pending = [normal]
    tokens = {'?local?': package, '?userconfig?': platform_default, '?userdata?': platform_default}
    while pending:
        if len(visited) >= 128:
            raise ValueError('Configuration-chain safety limit reached; nothing launched.')
        directory = pending.pop().resolve()
        canonical = os.path.normcase(str(directory))
        if canonical in visited:
            raise ValueError(f'Repeated configuration directory: {directory}. Nothing launched.')
        visited.add(canonical)
        if not directory.is_dir():
            raise ValueError(f'Configuration directory is missing: {directory}')
        cfg = directory / 'openmw.cfg'
        entries = config_entries(cfg)
        if cfg.exists():
            hashes[str(cfg)] = digest(cfg)
        if any(k == 'replace' and v == 'config' for k, v, _ in entries) and len(active) > 1:
            active = active[:1]
        active.append(directory)
        children = []
        for key, value, line in entries:
            if key != 'config':
                continue
            text = decode_config_path(value)
            if text.startswith('?'):
                match = re.match(r'^\?[^?]*\?', text)
                if match is None or match.group(0) not in tokens:
                    raise ValueError(f'Unsupported configuration token: {cfg}:{line}')
                child = tokens[match.group(0)] / text[match.end():]
            else:
                child = Path(text)
                if not child.is_absolute():
                    child = directory / child
            children.append(child)
        pending.extend(reversed(children))
    for directory in active:
        for name in ('settings.cfg', *COPY_NAMES):
            path = directory / name
            if path.is_file():
                hashes[str(path)] = digest(path)
    return active, hashes


def quoted(path: Path) -> str:
    text = path.resolve().as_posix()
    if '\n' in text or '\r' in text:
        raise ValueError('Paths with newlines are not supported.')
    return '"' + text.replace('&', '&&').replace('"', '&"') + '"'


def build_command(exe: Path, normal: Path, evidence: Path, *, startup_script: Path | None = None) -> list[str]:
    # Do NOT add --load-savegame, even with an empty value.
    # Direct subprocess launches retain the original empty override. Profilers
    # may drop empty argv entries; their caller supplies a private no-op file.
    return [str(exe), '--replace=config', '--config', str(normal), '--config', str(evidence),
            '--user-data', str(evidence / 'user-data'), '--resources', str(exe.parent / 'resources'),
            '--skip-menu=false', '--new-game=false', '--script-run',
            str(startup_script) if startup_script is not None else '']


def validated_build_manifest(executable: Path) -> dict | None:
    path = executable.parent / 'persistent-build-manifest.json'
    if not path.is_file():
        return None
    manifest = json.loads(path.read_text(encoding='utf-8-sig'))
    if (manifest.get('schema') != 1 or not re.fullmatch(r'[0-9a-f]{40}', manifest.get('base_commit', ''))
            or not isinstance(manifest.get('changed_source_files'), dict)):
        raise ValueError('Candidate build identity is malformed. Nothing launched.')
    if manifest.get('executable_sha256') != digest(executable):
        raise ValueError('Executable differs from candidate build hash manifest. Nothing launched.')
    return manifest


def package_identity(executable: Path, requested: str) -> str:
    """Use the package's current identity; never pin the starter to an old EXE."""
    identity = executable.parent / 'CP3E-TEST-IDENTITY.txt'
    build = validated_build_manifest(executable)
    actual = build['base_commit'] if build else None
    if actual and requested != 'unrecorded' and requested != actual:
        raise ValueError('Requested source identity differs from candidate. Nothing launched.')
    if identity.is_file():
        for line in identity.read_text(encoding='utf-8-sig').splitlines():
            if line.startswith('commit='):
                actual = line.split('=', 1)[1]
        if actual is None or not re.fullmatch(r'[0-9a-f]{40}', actual):
            raise ValueError('Package commit identity is malformed. Nothing launched.')
        if requested != 'unrecorded' and requested != actual:
            raise ValueError('Requested source identity differs from package. Nothing launched.')
    manifest = executable.parent / 'CP3E-PACKAGE-SHA256.txt'
    if manifest.is_file():
        matches = []
        for line in manifest.read_text(encoding='utf-8-sig').splitlines():
            fields = line.split(None, 1)
            if len(fields) == 2 and fields[1].replace('\\', '/') == executable.name:
                matches.append(fields[0])
        if len(matches) != 1 or matches[0] != digest(executable):
            raise ValueError('Executable differs from the package hash manifest. Nothing launched.')
    return actual or requested

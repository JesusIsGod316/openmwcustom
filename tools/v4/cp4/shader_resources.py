"""Stage and audit the engine shader package, not user/mod shader content.

The overlay checksum is the existing pinned V3 payload. Verification is a
deployment gate, not a GPU compilation or visual-compatibility claim.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import zipfile

OVERLAY_SHA256 = '6f42a686e2a6a9038bbd4a9e2d0d1be6d8812b9b3e681d8b569560c2fe110255'
MANIFEST = 'shader-package.json'
REFERENCE = re.compile(r'(#\s*include|@link)\s+"([^"\r\n]+)"')
SHADER_SUFFIXES = {'.glsl', '.vert', '.frag', '.geom', '.comp', '.tesc', '.tese'}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def safe_path(root, name):
    relative = PurePosixPath(name)
    if relative.is_absolute() or '..' in relative.parts or '\\' in name or ':' in name:
        raise ValueError(f'Unsafe shader package path: {name}')
    path = root.joinpath(*relative.parts)
    if not path.resolve().is_relative_to(root.resolve()):
        raise ValueError(f'Shader path escapes package: {name}')
    return path


def audit_references(root, names):
    errors = []
    include_graph = {}
    for name in names:
        path = safe_path(root, name)
        if path.suffix not in SHADER_SUFFIXES or not path.is_file():
            continue
        # Comments are not shader dependencies. Check every conditional branch,
        # since another settings combination can enable it at runtime.
        source = re.sub(r'/\*.*?\*/|//[^\n]*', '', path.read_text(encoding='utf-8-sig'), flags=re.S)
        include_graph[name] = []
        for kind, reference in REFERENCE.findall(source):
            if kind == '@link':
                # ShaderManager::getShader prefixes unqualified linked names.
                target = reference if reference.startswith(('lib', 'compatibility', 'core')) else 'compatibility/' + reference
            else:
                # ShaderManager::parseIncludes uses the containing directory
                # only for names without an engine root prefix.
                target = reference if reference.startswith(('lib', 'compatibility', 'core')) else str(PurePosixPath(name).parent / reference)
            if not safe_path(root, target).is_file():
                errors.append(f'{name}: missing {kind} "{reference}" (resolved {target})')
            if kind != '@link':
                include_graph[name].append(target)
    done = set()

    def visit(name, stack):
        if name in stack:
            errors.append('Cyclic shader include: ' + ' -> '.join((*stack, name)))
            return
        if name in done:
            return
        for child in include_graph.get(name, []):
            visit(child, (*stack, name))
        done.add(name)

    for name in include_graph:
        visit(name, ())
    return errors


def verify(root):
    root = Path(root)
    manifest_path = root / MANIFEST
    if not manifest_path.is_file():
        raise ValueError(f'Missing shader package manifest: {manifest_path}; rebuild shader resources')
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    if manifest.get('schema') != 1 or manifest.get('overlay_sha256') != OVERLAY_SHA256 or not manifest.get('files'):
        raise ValueError('Unrecognized or empty shader package manifest')
    errors = []
    for name, expected in manifest['files'].items():
        path = safe_path(root, name)
        if not path.is_file():
            errors.append(f'Missing shader resource: {name}')
        elif digest(path.read_bytes()) != expected:
            errors.append(f'Shader resource differs from staged package: {name}')
    # Include additional engine files too; do not silently overlook a stale file.
    names = [p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()]
    errors.extend(audit_references(root, names))
    if errors:
        raise ValueError('Shader package QC failed:\n' + '\n'.join(errors))
    return {'files': len(manifest['files']), 'manifest_sha256': digest(manifest_path.read_bytes()),
            'overlay_sha256': manifest['overlay_sha256']}


def stage(source, base_list, overlay, destination):
    source, destination, overlay = Path(source), Path(destination), Path(overlay)
    if digest(overlay.read_bytes()) != OVERLAY_SHA256:
        raise ValueError('Pinned shader overlay checksum mismatch')
    # Compute the expected bytes before modifying the destination. Overlay wins
    # deliberately, matching the historical base-then-overlay packaging order.
    payload = {name: safe_path(source, name).read_bytes()
               for name in Path(base_list).read_text(encoding='utf-8').splitlines() if name}
    with zipfile.ZipFile(overlay) as archive:
        for entry in archive.infolist():
            if not entry.is_dir():
                safe_path(destination, entry.filename)
                payload[entry.filename] = archive.read(entry)
    for name, data in payload.items():
        path = safe_path(destination, name)
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.is_file() or path.read_bytes() != data:
            path.write_bytes(data)
    errors = audit_references(destination, payload)
    if errors:
        raise ValueError('Shader deployment has unresolved dependencies:\n' + '\n'.join(errors))
    manifest = {'schema': 1, 'overlay_sha256': OVERLAY_SHA256,
                'files': {name: digest(data) for name, data in sorted(payload.items())}}
    (destination / MANIFEST).write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    return verify(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='mode', required=True)
    check = sub.add_parser('verify')
    check.add_argument('destination')
    copy = sub.add_parser('stage')
    for name in ('source', 'base-list', 'overlay', 'destination'):
        copy.add_argument('--' + name, required=True)
    args = parser.parse_args()
    try:
        result = verify(args.destination) if args.mode == 'verify' else stage(
            args.source, args.base_list, args.overlay, args.destination)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.exit(1, str(error) + '\n')
    print('Shader package QC PASS: ' + json.dumps(result))


if __name__ == '__main__':
    main()

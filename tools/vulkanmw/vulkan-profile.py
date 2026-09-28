"""Build/verify/run a content-preserving, settings-isolated Vulkan profile.

No normal settings, saves, assets, registry entries, or environment are modified.
Preparation never starts the game. Unknown content syntax fails closed.
"""
from __future__ import annotations

import argparse
import base64
from datetime import datetime, timezone
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'tools/v4/cp4'))
import diagnosticconfig as dc

# Copy content definitions, not startup automation or renderer presets.
CONTENT_KEYS = {'data', 'data-local', 'fallback', 'fallback-archive', 'content',
                'groundcover', 'encoding', 'no-sound', 'script-warn', 'script-console',
                'activate-dist'}
STRIPPED_KEYS = {'config', 'user-data', 'resources', 'start', 'script-run',
                 'script-all', 'script-all-dialogue', 'new-game', 'skip-menu',
                 'export-fonts', 'random-seed', 'no-grab'}
COMPOSING = {'config', 'data', 'content', 'groundcover', 'fallback', 'fallback-archive', 'replace'}
QUALITY_SECTIONS = {'General', 'Shaders', 'Shadows', 'Water', 'Groundcover', 'Terrain', 'Fog'}
SEMANTIC_SECTIONS = {'Game', 'Models', 'Input', 'GUI', 'HUD', 'Sound', 'Map'}
CAMERA_KEYS = {'field of view', 'first person field of view', 'viewing distance',
               'small feature culling', 'small feature culling pixel size',
               'v3.21 full body first person', 'v3.21 full body first person forward offset',
               'v3.21 full body first person shadow compatibility',
               'full body first person hybrid animations'}
ENV_PREFIXES = ('OPENMW_', 'OSG', 'VSG', 'VK_', 'MESA_', '__GL_', '__NV_')


def settings(text):
    """The engine uses # comments and case-sensitive category/key pairs, not INI interpolation."""
    result, category = {}, ''
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        if line.startswith('['):
            end = line.find(']')
            if end < 0:
                raise ValueError(f'Unterminated category at line {number}')
            category, line = line[1:end].strip(), line[end + 1:].strip()
            if not line:
                continue
        if not category or '=' not in line:
            raise ValueError(f'Invalid settings line {number}')
        key, value = (part.strip() for part in line.split('=', 1))
        pair = category, key
        if pair in result:
            raise ValueError(f'Duplicate setting {pair}')
        result[pair] = value
    return result


def serialized(values):
    lines, last = [], None
    for (section, key), value in sorted(values.items()):
        if section != last:
            lines += ['', f'[{section}]']
            last = section
        lines.append(f'{key} = {value}')
    return '\n'.join(lines) + '\n'


def content_snapshot(path, package, platform_default):
    out, removed = [], []
    for key, value, line in dc.config_entries(path):
        if key in STRIPPED_KEYS:
            removed.append({'file': str(path), 'line': line, 'key': key})
            continue
        if key == 'replace':
            if value not in COMPOSING:
                raise ValueError(f'Unsupported replace directive: {path}:{line}')
            if value != 'config':
                out.append(f'{key}={value}')
            continue
        if key not in CONTENT_KEYS:
            raise ValueError(f'Unaudited content option {key}: {path}:{line}')
        if key in {'data', 'data-local'}:
            raw = dc.decode_config_path(value)
            for token, base in (('?local?', package), ('?userconfig?', platform_default),
                                ('?userdata?', platform_default)):
                if raw.startswith(token):
                    raw = str(base / raw[len(token):])
                    break
            if raw.startswith('?'):
                raise ValueError(f'Unsupported content path token: {path}:{line}')
            resolved = Path(raw)
            if not resolved.is_absolute():
                resolved = path.parent / resolved
            # Data directories are read-only references, not copied/reordered.
            value = dc.quoted(resolved)
        out.append(f'{key}={value}')
    return '\n'.join(out) + '\n', removed


def environment(parent, controls):
    removed = sorted(k for k in parent if k.upper().startswith(ENV_PREFIXES)
                     or k.upper() == 'DISABLE_RTSS_LAYER')
    child = {k: v for k, v in parent.items() if k not in removed}
    child.update(controls)
    return child, removed


def select_settings(defaults, normal, overrides):
    selected = {}
    for pair, value in normal.items():
        section, key = pair
        permitted = (section in SEMANTIC_SECTIONS or
                     section in QUALITY_SECTIONS and not key.startswith(('v3', 'optimizedmw', 'opimizedmw')) or
                     section == 'Camera' and key in CAMERA_KEYS)
        if permitted and pair in defaults:
            selected[pair] = value
    unknown = sorted(f'[{s}] {k}' for s, k in normal if (s, k) not in defaults)
    missing = set(overrides) - set(defaults)
    if missing:
        raise ValueError(f'Profile settings are not supported by this package: {sorted(missing)}')
    selected.update(overrides)
    return selected, unknown


def prepare(executable, normal, output):
    executable, normal, output = executable.resolve(), normal.resolve(), output.resolve()
    package = executable.parent
    if not executable.is_file() or output.exists():
        raise ValueError('Executable missing or output already exists; nothing overwritten')
    if not (package / 'resources').is_dir():
        raise ValueError('Package resources missing')
    # Package settings are unavoidable defaults in this engine: refuse rather than inherit them.
    if (package / 'settings.cfg').exists():
        raise ValueError('Package has settings.cfg. Use a settings-free runtime package; do not modify it here.')
    chain, hashes = dc.inspect_chain(package, normal, dc.normal_default())
    defaults_path = package / 'defaults.bin'
    defaults = settings(base64.b64decode(defaults_path.read_bytes()).decode('utf-8-sig'))
    normal_values = {}
    snapshots, removed = [], []
    for directory in chain[1:]:
        text, dropped = content_snapshot(directory / 'openmw.cfg', package, dc.normal_default())
        snapshots.append(text)
        removed.extend(dropped)
        path = directory / 'settings.cfg'
        if path.is_file():
            normal_values.update(settings(path.read_text(encoding='utf-8-sig')))
    template = HERE / 'vulkan-clean-settings.cfg'
    selected, unknown = select_settings(defaults, normal_values, settings(template.read_text(encoding='utf-8')))
    spec = importlib.util.spec_from_file_location('vulkan_profile_capture', ROOT / 'tools/v4/cp4/gameplay-diagnostics.py')
    capture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(capture)
    controls = {}
    capture.configure_cpu_fastpaths(controls, 'vulkan', 'retained')
    controls.pop('OPENMW_V4_SUBMIT_BREAKDOWN', None)
    controls.update(OPENMW_V4_STATIC_FRUSTUM='1', OPENMW_V4_TERRAIN_OCCLUSION='1')
    # New resource/pose experiments remain absent (presence tests treat even "0" as enabled).
    command = [str(executable), '--replace=config']
    pinned = {str(executable): dc.digest(executable), str(defaults_path): dc.digest(defaults_path),
              str(package / 'openmw.cfg'): dc.digest(package / 'openmw.cfg'),
              str(Path(__file__).resolve()): dc.digest(Path(__file__).resolve()),
              str(template): dc.digest(template), str(Path(dc.__file__)): dc.digest(Path(dc.__file__))}
    output.mkdir(parents=True)
    for index, text in enumerate(snapshots):
        directory = output / 'content' / str(index)
        directory.mkdir(parents=True)
        path = directory / 'openmw.cfg'
        path.write_text(text, encoding='utf-8')
        pinned[str(path)] = dc.digest(path)
        command += ['--config', str(directory)]
    (output / 'openmw.cfg').write_text('# Private writable configuration; no inherited settings.\n', encoding='utf-8')
    pinned[str(output / 'openmw.cfg')] = dc.digest(output / 'openmw.cfg')
    (output / 'settings.cfg').write_text('# Vulkan-only baseline; original settings are not in the live config chain.\n'
                                         + serialized(selected), encoding='utf-8')
    (output / 'user-data').mkdir()
    # Only keybindings are copied. No saves, Lua state, old postfx settings or shader files.
    for directory in reversed(chain):
        if (directory / 'input_v3.xml').is_file():
            shutil.copy2(directory / 'input_v3.xml', output / 'input_v3.xml')
            break
    command += ['--config', str(output), '--user-data', str(output / 'user-data'),
                '--resources', str(package / 'resources'), '--skip-menu=false', '--new-game=false', '--script-run', '']
    effective = defaults | selected
    (output / 'effective-settings-at-creation.cfg').write_text(serialized(effective), encoding='utf-8')
    manifest = {'schema': 1, 'created': datetime.now(timezone.utc).isoformat(), 'command': command,
                'cwd': str(package), 'controls': controls, 'pinned_files': pinned,
                'original_chain_hashes': hashes, 'original_chain': list(map(str, chain)),
                'settings_sha256_at_creation': dc.digest(output / 'settings.cfg'),
                'unrecognized_normal_settings': unknown, 'excluded_startup_options': removed,
                'old_postfx_chain_reference_only': normal_values.get(('Post Processing', 'chain'), ''),
                'quality_changes': {'render scale': '1.0; original GL upscaling is not shared by Vulkan',
                                   'postfx': 'off; Vulkan OMWFX not implemented',
                                   'legacy_occlusion': 'off; native visibility controls separate'},
                'notes': ['Not benchmarked. Not a complete compatibility certification.',
                          'No old saves or Lua persistent state imported. Game writes only private data.',
                          'Native Vulkan host-pressure policy remains enabled by its engine default.',
                          'External overlays, driver profiles and registered implicit layers are not changed.']}
    (output / 'profile.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    # A transparent normal-user launcher. No elevation or system environment mutation.
    starter = '@echo off\r\n"' + sys.executable + '" "' + str(Path(__file__).resolve()) + '" run "' + str(output) + '"\r\npause\r\n'
    if '\x00' in starter or '%' in starter:
        raise ValueError('Batch launcher path contains an unsupported percent character')
    (output / 'Start-Vulkan-Clean.cmd').write_text(starter, encoding='utf-8')
    (output / 'README.txt').write_text(
        'Start-Vulkan-Clean.cmd runs the existing Vulkan executable with private settings/data.\n'
        'No administrator rights required. Choose New Game. Original saves are intentionally absent.\n'
        'This isolates settings; it does not implement missing post-processing or renderer repairs.\n'
        'You may edit settings.cfg; effective settings and hashes are recorded for each run.\n'
        'Do not start openmw.exe directly for this test: that bypasses isolation.\n'
        'Content paths/load order are frozen snapshots; regenerate after changing your mod list.\n', encoding='utf-8')
    verify(output)
    return manifest


def verify(output):
    # prepare() and CLI launchers resolve paths. Library callers may still pass
    # a Windows 8.3 alias, junction, or a path containing '..'. Compare canonical
    # directories, not their spellings, before classifying writable settings.
    output = output.resolve()
    manifest = json.loads((output / 'profile.json').read_text(encoding='utf-8'))
    if manifest.get('schema') != 1:
        raise ValueError('Unsupported profile schema')
    if (Path(manifest['cwd']) / 'settings.cfg').exists():
        raise ValueError('Runtime acquired a settings.cfg; isolation no longer holds')
    for path, digest in manifest['pinned_files'].items():
        if not Path(path).is_file() or dc.digest(Path(path)) != digest:
            raise ValueError('Pinned runtime/profile changed: ' + path)
    original_directories = {Path(path).resolve() for path in manifest['original_chain'][1:]}
    for index, arg in enumerate(manifest['command']):
        if arg == '--config':
            # Relative command arguments are interpreted from the launch cwd.
            directory = (Path(manifest['cwd']) / manifest['command'][index + 1]).resolve()
            if directory in original_directories:
                raise ValueError('Original config directory leaked into launch')
            if directory != output and (directory / 'settings.cfg').exists():
                raise ValueError('Content-only snapshot acquired settings.cfg: ' + str(directory))
    current = settings((output / 'settings.cfg').read_text(encoding='utf-8-sig'))
    defaults = settings(base64.b64decode((Path(manifest['cwd']) / 'defaults.bin').read_bytes()).decode('utf-8-sig'))
    if set(current) - set(defaults):
        raise ValueError('Unknown settings in isolated profile')
    if current.get(('Video', 'renderer backend')) != 'vulkan' or current.get(('Video', 'renderer fallback')) != 'false':
        raise ValueError('Profile must explicitly require Vulkan without OpenGL fallback')
    return manifest, defaults | current


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest='action', required=True)
    create = actions.add_parser('prepare')
    create.add_argument('--executable', type=Path, required=True)
    create.add_argument('--user-config', type=Path, required=True)
    create.add_argument('--output', type=Path, required=True)
    for name in ('verify', 'run'):
        actions.add_parser(name).add_argument('profile', type=Path)
    args = parser.parse_args()
    if args.action == 'prepare':
        manifest = prepare(args.executable, args.user_config, args.output)
        print(f'Prepared {args.output}; {len(manifest["unrecognized_normal_settings"])} unrecognized original settings excluded.')
        return
    profile = args.profile.resolve()
    manifest, effective = verify(profile)
    env, removed = environment(os.environ, manifest['controls'])
    print('Verified isolated Vulkan profile. Original settings and saves are not in its writable paths.')
    if args.action == 'verify':
        return
    logs = profile / 'runs' / datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    logs.mkdir(parents=True)
    (logs / 'effective-settings.cfg').write_text(serialized(effective), encoding='utf-8')
    record = {'command': manifest['command'], 'controls': manifest['controls'],
              'removed_environment_names': removed, 'settings_sha256': dc.digest(profile / 'settings.cfg'),
              'pinned_files': manifest['pinned_files']}
    path = logs / 'launch.json'
    path.write_text(json.dumps(record, indent=2), encoding='utf-8')
    print('Private run:', logs, flush=True)
    with (logs / 'console.log').open('wb') as console:
        result = subprocess.run(manifest['command'], cwd=manifest['cwd'], env=env,
                                stdout=console, stderr=subprocess.STDOUT)
    record['exit_code'] = result.returncode
    record['original_files_still_match_creation'] = {
        p: Path(p).is_file() and dc.digest(Path(p)) == h for p, h in manifest['original_chain_hashes'].items()}
    path.write_text(json.dumps(record, indent=2), encoding='utf-8')
    print('Exited:', result.returncode, '; log:', logs / 'console.log')
    sys.exit(result.returncode)


if __name__ == '__main__':
    main()

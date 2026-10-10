"""One-way normal-profile import; Vulkan owns all subsequent writable state.

Mod directories remain read-only VFS references. No config directory from the
source is ever passed to the engine. An existing private profile is never reseeded.
"""
from pathlib import Path
import json
import shutil
from datetime import datetime, timezone


def path_value(raw, directory, package, platform):
    for token, base in [('?local?', package), ('?userconfig?', platform), ('?userdata?', platform)]:
        if raw.startswith(token):
            return (base / raw[len(token):]).resolve()
    if raw.startswith('?'):
        raise ValueError('Unsupported path token: ' + raw)
    path = Path(raw)
    return (path if path.is_absolute() else directory / path).resolve()


def validate(package, normal, output, dc):
    output = output.resolve()
    if not output.is_relative_to(package.resolve()) or output.is_relative_to(normal.resolve()):
        raise ValueError('Private profile resolves outside the package or inside normal data')
    marker = json.loads((output / 'profile-import.json').read_text())
    if marker.get('schema') != 1 or marker.get('source_config') != str(normal.resolve()):
        raise ValueError('Existing private profile has a different owner/source; refusing to overwrite it')
    for path in output.rglob('*'):
        if not path.resolve().is_relative_to(output) or path.is_symlink() or path.is_junction():
            raise ValueError('Private profile contains a link/reparse path: ' + str(path))
    required = {'user-data': output / 'user-data', 'data-local': output / 'local-data'}
    entries = dc.config_entries(output / 'openmw.cfg')
    for key in required:
        if sum(k == key for k, _, _ in entries) != 1:
            raise ValueError('Writable path must occur exactly once: ' + key)
    for key, raw, _ in entries:
        if key == 'config' or key in ('script-run', 'start', 'skip-menu', 'new-game'):
            raise ValueError('Private config must not reintroduce config/startup inheritance')
        if key in required:
            if path_value(dc.decode_config_path(raw), output, package, dc.normal_default()) != required.pop(key):
                raise ValueError('Writable path escapes private profile: ' + key)
    if required:
        raise ValueError('Private config is missing explicit writable paths')
    return output


def prepare(package, normal, chain, dc, profile):
    output = package.resolve() / 'UserProfile'
    if output.exists():
        return validate(package, normal, output, dc)
    if output.resolve().is_relative_to(normal.resolve()):
        raise ValueError('Experimental profile must not be inside normal user data')
    snapshots, removed, values = [], [], {}
    local_data, source_data = None, dc.normal_default()
    for directory in chain[1:]:
        text, dropped = profile.content_snapshot(directory / 'openmw.cfg', package, dc.normal_default())
        lines = []
        for line in text.splitlines():
            if line.startswith('data-local='):
                local_data = dc.decode_config_path(line.split('=', 1)[1])
            else:
                lines.append(line)
        snapshots.append('\n'.join(lines))
        removed.extend(dropped)
        for key, raw, _ in dc.config_entries(directory / 'openmw.cfg'):
            if key == 'user-data':
                source_data = path_value(dc.decode_config_path(raw), directory, package, dc.normal_default())
        if (directory / 'settings.cfg').is_file():
            values.update(profile.settings((directory / 'settings.cfg').read_text(encoding='utf-8-sig')))
    # Force the experimental renderer only in its own saved settings.
    values.update({('Video', 'renderer backend'): 'vulkan', ('Video', 'renderer fallback'): 'false'})
    config = '# Vulkan private profile. Source mod paths are read-only references.\n' + '\n'.join(snapshots) + '\n'
    if local_data:
        config += 'data=' + dc.quoted(Path(local_data)) + '\n'
    config += ('replace=config\nuser-data=' + dc.quoted(output / 'user-data') + '\n'
               + 'data-local=' + dc.quoted(output / 'local-data') + '\n'
               + 'resources=' + dc.quoted(package / 'resources') + '\n')
    output.mkdir()
    (output / 'user-data').mkdir()
    (output / 'local-data').mkdir()
    (output / 'openmw.cfg').write_text(config, encoding='utf-8')
    (output / 'settings.cfg').write_text(profile.serialized(values), encoding='utf-8')
    copied = {}
    for name in dc.COPY_NAMES:
        for directory in reversed(chain[1:]):
            source = directory / name
            if source.is_file():
                shutil.copy2(source, output / name)
                copied[str(source)] = dc.digest(source)
                break
    saves = source_data / 'saves'
    if saves.is_dir():
        for source in saves.rglob('*'):
            if source.is_symlink() or source.is_junction() or not source.resolve().is_relative_to(saves.resolve()):
                raise ValueError('Source saves contain linked data; no recursive import allowed')
        shutil.copytree(saves, output / 'user-data/saves')
        copied.update({str(p): dc.digest(p) for p in saves.rglob('*') if p.is_file()})
    marker = {'schema': 1, 'source_config': str(normal.resolve()),
              'created_utc': datetime.now(timezone.utc).isoformat(),
              'source_files_at_import': copied, 'excluded_startup_options': removed,
              'policy': 'One-way initial copy. Never sync experimental files back. Never reseed existing data.'}
    (output / 'profile-import.json').write_text(json.dumps(marker, indent=2) + '\n')
    return validate(package, normal, output, dc)


def command(exe, private, dc):
    return [str(exe), '--replace=config', '--config', str(private),
            '--user-data', str(private / 'user-data'), '--data-local', str(private / 'local-data'),
            '--resources', str(exe.parent / 'resources'), '--skip-menu=false', '--new-game=false', '--script-run', '']

"""Private fixed-scene diagnostic cohort. Never copies saves or replaces an existing package."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--user-config', required=True)
    parser.add_argument('--short', action='store_true', help='One off/on Vulkan pair followed by OpenGL')
    parser.add_argument('--resolution-probe', action='store_true', help='Also diagnose pixel sensitivity at 960x540')
    parser.add_argument('--compare-cached-admission', action='store_true',
                        help='ABBA Vulkan-only comparison of per-animation playback admission cache')
    args = parser.parse_args()
    if args.compare_cached_admission and (args.short or args.resolution_probe):
        raise ValueError('Cached-admission ABBA must use its own unmodified comparison sequence')
    root = Path(__file__).resolve().parents[2]
    executable = root / 'build/phase3c-engine/RelWithDebInfo/openmw.exe'
    package = args.package.resolve()
    if package.exists():
        raise ValueError('Package already exists; preserve it and choose another name')
    if shutil.disk_usage(package.parent).free < 10 * 1024**3:
        raise ValueError('Less than 10 GiB free; refusing additional package/captures')
    package.mkdir()
    for path in args.runtime.iterdir():
        if path.is_file() and (path.suffix.lower() == '.dll' or path.name in (
                'defaults.bin', 'gamecontrollerdb.txt', 'openmw.cfg', 'resources.cfg')):
            shutil.copy2(path, package / path.name)
    for name in ('resources', 'osgPlugins-3.6.5'):
        shutil.copytree(args.runtime / name, package / name)
    shutil.copy2(executable, package / 'openmw.exe')
    base = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
    paths = set(subprocess.check_output(['git', 'diff', '--name-only', 'HEAD'], cwd=root, text=True).splitlines())
    paths.update(subprocess.check_output(['git', 'ls-files', '--others', '--exclude-standard'], cwd=root, text=True).splitlines())
    source = {'source_head': base, 'executable_sha256': sha(executable),
              'changed_source_files': {p: sha(root / p) for p in sorted(paths) if (root / p).is_file()}}
    source_path = package / 'source-changes.json'
    source_path.write_text(json.dumps(source, indent=2), encoding='utf-8')
    source_hash = sha(source_path)
    benchmark = root / 'tools/v4/cp4/architecture-benchmark.py'
    analyzer = root / 'tools/vulkanmw/frame-profile-report.py'
    if args.compare_cached_admission:
        sequence = (
            ('admission-control-a', 'vulkan', True, False),
            ('admission-cached-a', 'vulkan', True, True),
            ('admission-cached-b', 'vulkan', True, True),
            ('admission-control-b', 'vulkan', True, False))
    else:
        sequence = (
            ('vulkan-off-a', 'vulkan', False, False), ('vulkan-profile-a', 'vulkan', True, False),
            ('opengl-profile', 'opengl', True, False), ('vulkan-profile-b', 'vulkan', True, False),
            ('vulkan-off-b', 'vulkan', False, False))
        sequence = sequence[:3] if args.short else sequence
        if args.resolution_probe:
            sequence += (('vulkan-half-resolution-diagnostic', 'vulkan', True, False),)
    for name, renderer, profile, cached_admission in sequence:
        if sha(package / 'openmw.exe') != source['executable_sha256']:
            raise ValueError('Cohort executable changed')
        output = package / 'Benchmarks' / name
        command = [sys.executable, str(benchmark), '--executable', str(package / 'openmw.exe'),
                   '--output', str(output), '--user-config', args.user_config, '--source-head', base + '+local-profile',
                   '--source-diff-sha256', source_hash, '--renderer', renderer,
                   '--fastpaths', 'retained' if renderer == 'vulkan' else 'control']
        if profile:
            command.append('--frame-profile')
        if cached_admission:
            command.append('--cached-object-admission')
        if name == 'vulkan-half-resolution-diagnostic':
            command.append('--diagnostic-half-resolution')
        print('START', name, flush=True)
        subprocess.run(command, cwd=root, check=True)
        evidence = next(p.parent for p in output.rglob('manifest.json'))
        with (output / 'analysis-console.txt').open('w', encoding='utf-8') as log:
            subprocess.run([sys.executable, str(analyzer), str(evidence)], cwd=root, stdout=log, check=True)
        print('COMPLETE', name, evidence, flush=True)


if __name__ == '__main__':
    main()

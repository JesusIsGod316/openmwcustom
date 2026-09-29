"""Vulkan-only, settings-isolated same-binary publication experiments.

Fresh private data per run. No normal save is loaded or overwritten. Each run
quits naturally through the existing fixed-scene Lua benchmark. No GPU quality
changes between arms; diagnostic timings are not a compatibility certification.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


profile = load('isolated_profile', HERE / 'vulkan-profile.py')
reporter = load('frame_report', HERE / 'frame-profile-report.py')
FLAGS = {
    'groups': 'OPENMW_VK_GROUP_PUBLICATION',
    'transactions': 'OPENMW_VK_CHUNK_TRANSACTIONS',
    'inventories': 'OPENMW_VK_RESOURCE_INVENTORIES',
    'tiles': 'OPENMW_VK_TILED_LIGHTS',
    'lod': 'OPENMW_VK_PERSISTENT_EVALUATED_LOD',
    'particles': 'OPENMW_VK_SPLIT_PARTICLE_CAPTURE',
    'actors': 'OPENMW_VK_PERSISTENT_ACTORS',
    'admission': 'OPENMW_VK_CACHED_OBJECT_ADMISSION',
    'placement-frustum': 'OPENMW_VK_PLACEMENT_FRUSTUM',
    'particle-slots': 'OPENMW_VK_RETAIN_PARTICLE_SLOTS',
    'change-driven': 'OPENMW_VK_CHANGE_DRIVEN_OBJECTS',
    'queues': 'OPENMW_VK_PRODUCER_DIRTY_QUEUES',
    'supported-continuous': 'OPENMW_VK_SUPPORTED_CONTINUOUS_PRODUCERS',
    'gpu-tables': 'OPENMW_VK_GPU_SCENE_TABLES',
    'gpu-cull-queue': 'OPENMW_VK_GPU_CULL_QUEUE',
    'gpu-cull-indirect': 'OPENMW_VK_GPU_CULL_INDIRECT',
    'gpu-cull-compact': 'OPENMW_VK_GPU_CULL_COMPACT',
    'shadow-record': 'OPENMW_VK_PARALLEL_SHADOW_RECORD',
    # Process-local diagnostic, not a renderer repair or a global RTSS change.
    'no-overlay': 'DISABLE_RTSS_LAYER',
    # Diagnostic quality arm: clustered attenuation differs from classic.
    'fastlights': 'OPENMW_VK_TILED_LIGHTS',
    'clustered': None,
}


def selected(arm):
    if arm == 'control':
        return {}
    names = ['groups', 'transactions', 'inventories'] if arm == 'combined' else arm.split('+')
    if not names or any(name not in FLAGS for name in names):
        raise ValueError('Unknown publication arm: ' + arm)
    return {FLAGS[name]: '1' for name in names if FLAGS[name]}


def package_runtime(runtime, package):
    if package.exists():
        raise ValueError('Package exists; preserve previous evidence')
    if shutil.disk_usage(package.parent).free < 10 * 1024**3:
        raise ValueError('Less than 10 GiB free; not creating another package')
    package.mkdir()
    for path in runtime.iterdir():
        if path.is_file() and (path.suffix.lower() == '.dll' or path.name in (
                'defaults.bin', 'gamecontrollerdb.txt', 'openmw.cfg', 'resources.cfg')):
            shutil.copy2(path, package / path.name)
    for name in ('resources', 'osgPlugins-3.6.5'):
        shutil.copytree(runtime / name, package / name)
    executable = ROOT / 'build/phase3c-engine/RelWithDebInfo/openmw.exe'
    shutil.copy2(executable, package / 'openmw.exe')
    paths = set(subprocess.check_output(['git', 'diff', '--name-only', 'HEAD'], cwd=ROOT, text=True).splitlines())
    paths.update(subprocess.check_output(['git', 'ls-files', '--others', '--exclude-standard'], cwd=ROOT, text=True).splitlines())
    source = {'source_head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'executable_sha256': profile.dc.digest(executable),
              'changed_source_files': {p: profile.dc.digest(ROOT / p) for p in sorted(paths) if (ROOT / p).is_file()}}
    (package / 'source-changes.json').write_text(json.dumps(source, indent=2), encoding='utf-8')
    return source


def run(package, normal, source, name, arm):
    evidence = package / 'Benchmarks' / name
    manifest = profile.prepare(package / 'openmw.exe', normal, evidence)
    if {'fastlights', 'clustered'} & set(arm.split('+')):
        settings_path = evidence / 'settings.cfg'
        values = profile.settings(settings_path.read_text(encoding='utf-8'))
        values[('Shaders', 'classic falloff')] = 'false'
        values[('Shaders', 'clustered lighting')] = 'true'
        settings_path.write_text(profile.serialized(values), encoding='utf-8')
        profile_path = evidence / 'profile.json'
        private_profile = json.loads(profile_path.read_text(encoding='utf-8'))
        private_profile['settings_sha256_at_creation'] = profile.dc.digest(settings_path)
        private_profile['quality_changes']['light_mode'] = (
            'OpenMW clustered lighting with radius fade; not equal-quality to classic falloff')
        profile_path.write_text(json.dumps(private_profile, indent=2), encoding='utf-8')
    manifest, effective = profile.verify(evidence)
    data = evidence / 'benchmark-data'
    script = data / 'scripts/architecture-benchmark.lua'
    script.parent.mkdir(parents=True)
    shutil.copy2(ROOT / 'tools/v4/cp4/architecture-benchmark.lua', script)
    (data / 'architecture-benchmark.omwscripts').write_text('PLAYER: scripts/architecture-benchmark.lua\n', encoding='utf-8')
    command = list(manifest['command'])
    command[command.index('--skip-menu=false')] = '--skip-menu=true'
    command += ['--start', 'Seyda Neen', '--random-seed', '123456', '--no-grab=true',
                '--data', str(data), '--content', 'architecture-benchmark.omwscripts']
    controls = manifest['controls'] | selected(arm) | {
        'OPENMW_RUNTIME_DIAGNOSTICS': 'standard',
        'OPENMW_RUNTIME_DIAGNOSTICS_FILE': str(evidence / 'runtime.jsonl'),
        'OPENMW_GAMEPLAY_DIAGNOSTICS': '1',
        'OPENMW_GAMEPLAY_DIAGNOSTICS_FILE': str(evidence / 'gameplay.jsonl'),
        'OPENMW_VK_FRAME_PROFILE': '1',
    }
    env, removed = profile.environment(os.environ, controls)
    manifest.update(command=command, controls=controls, sha256=source['executable_sha256'],
                    source_head=source['source_head'], source_manifest_sha256=profile.dc.digest(package / 'source-changes.json'),
                    benchmark_script_sha256=profile.dc.digest(script), benchmark_driver_sha256=profile.dc.digest(Path(__file__)),
                    renderer='vulkan', arm=arm, warmup_seconds=15, sample_seconds=30,
                    removed_environment_names=removed, state='prepared')
    if {'fastlights', 'clustered'} & set(arm.split('+')):
        manifest['quality_change'] = 'clustered radius-faded lights instead of classic falloff'
    if profile.dc.digest(package / 'openmw.exe') != source['executable_sha256']:
        raise ValueError('Cohort executable changed')
    path = evidence / 'manifest.json'
    path.write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print('START', name, flush=True)
    with (evidence / 'console.log').open('wb') as stream:
        process = subprocess.Popen(command, cwd=package, env=env, stdout=stream, stderr=subprocess.STDOUT)
        manifest.update(state='running', pid=process.pid)
        path.write_text(json.dumps(manifest, indent=2), encoding='utf-8')
        code = process.wait()
    lines = (evidence / 'console.log').read_text(encoding='utf-8', errors='replace').splitlines()
    results = [json.loads(line.split('ARCHITECTURE_BENCHMARK_RESULT ', 1)[1]) for line in lines
               if 'ARCHITECTURE_BENCHMARK_RESULT ' in line]
    unchanged = {p: Path(p).is_file() and profile.dc.digest(Path(p)) == h
                 for p, h in manifest['original_chain_hashes'].items()}
    manifest.update(state='exited', exit_code=code, original_chain_unchanged=unchanged,
                    automated_result=results[-1] if results else None)
    path.write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    if code or not results or not all(unchanged.values()):
        raise RuntimeError('Run incomplete; inspect ' + str(evidence))
    rows = [json.loads(line) for line in (evidence / 'gameplay.jsonl').read_text(encoding='utf-8').splitlines()]
    summary = reporter.summarize(rows)
    summary.update(executable_sha256=source['executable_sha256'], wall_clock_result=results[-1],
                   original_chain_unchanged=unchanged, analyzer_sha256=profile.dc.digest(HERE / 'frame-profile-report.py'))
    (evidence / 'frame-profile-summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print('COMPLETE', name, json.dumps(results[-1]), flush=True)
    return results[-1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--user-config', type=Path, required=True)
    parser.add_argument('--arms', nargs='+', default=['control', 'combined', 'combined', 'control'])
    parser.add_argument('--existing-package', action='store_true', help='Append uniquely named runs, without replacing existing evidence')
    parser.add_argument('--prefix', default='publication')
    args = parser.parse_args()
    for arm in args.arms:
        selected(arm)
    package = args.package.resolve()
    source = (json.loads((package / 'source-changes.json').read_text(encoding='utf-8')) if args.existing_package
              else package_runtime(args.runtime.resolve(), package))
    for index, arm in enumerate(args.arms):
        run(package, args.user_config.resolve(), source, f'{args.prefix}-{index}-{arm}', arm)


if __name__ == '__main__':
    main()

"""Run one fixed-scene native OMWFX test in a fresh private configuration.

Only the new package/test directory is written. User settings and saves remain
outside the writable configuration chain. Rendering success is not implied by
normal exit: inspect effect diagnostics and GPU/pixel tests separately.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('cohort', HERE / 'run-publication-cohort.py')
cohort = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cohort)
profile = cohort.profile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--runtime', type=Path)
    parser.add_argument('--user-config', type=Path, required=True)
    parser.add_argument('--effects', default='tonemap')
    parser.add_argument('--name', required=True)
    parser.add_argument('--visual-capture', action='store_true', help='One-shot before/after GPU images; invalidates performance timing')
    parser.add_argument('--stable-shadows', action='store_true', help='Exercise the opt-in world-anchored sun shadow fitter')
    args = parser.parse_args()
    package = args.package.resolve()
    if args.runtime:
        cohort.package_runtime(args.runtime.resolve(), package)
    source = json.loads((package / 'source-changes.json').read_text(encoding='utf-8'))
    if profile.dc.digest(package / 'openmw.exe') != source['executable_sha256']:
        raise ValueError('Package executable no longer matches source manifest')
    evidence = package / 'Benchmarks' / args.name
    manifest = profile.prepare(package / 'openmw.exe', args.user_config, evidence)
    settings_file = evidence / 'settings.cfg'
    values = profile.settings(settings_file.read_text(encoding='utf-8'))
    values[('Post Processing', 'enabled')] = 'true'
    values[('Post Processing', 'chain')] = args.effects
    settings_file.write_text(profile.serialized(values), encoding='utf-8')
    manifest['settings_sha256_at_creation'] = profile.dc.digest(settings_file)
    manifest['quality_changes']['postfx'] = 'Native Vulkan OMWFX experimental: ' + args.effects
    manifest['controls'].update(cohort.selected('combined'))
    manifest['controls']['OPENMW_VK_OMWFX'] = '1'
    if args.stable_shadows:
        manifest['controls']['OPENMW_VK_STABLE_SUN_SHADOWS'] = '1'
    (evidence / 'profile.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    manifest, effective = profile.verify(evidence)
    (evidence / 'effective-settings-at-creation.cfg').write_text(profile.serialized(effective), encoding='utf-8')
    data = evidence / 'benchmark-data'
    script = data / 'scripts/architecture-benchmark.lua'
    script.parent.mkdir(parents=True)
    shutil.copy2(cohort.ROOT / 'tools/v4/cp4/architecture-benchmark.lua', script)
    (data / 'architecture-benchmark.omwscripts').write_text('PLAYER: scripts/architecture-benchmark.lua\n', encoding='utf-8')
    command = list(manifest['command'])
    command[command.index('--skip-menu=false')] = '--skip-menu=true'
    command += ['--start', 'Seyda Neen', '--random-seed', '123456', '--no-grab=true',
                '--data', str(data), '--content', 'architecture-benchmark.omwscripts']
    controls = manifest['controls'] | {
        'OPENMW_RUNTIME_DIAGNOSTICS': 'standard',
        'OPENMW_RUNTIME_DIAGNOSTICS_FILE': str(evidence / 'runtime.jsonl'),
        'OPENMW_GAMEPLAY_DIAGNOSTICS': '1',
        'OPENMW_GAMEPLAY_DIAGNOSTICS_FILE': str(evidence / 'gameplay.jsonl'),
        'OPENMW_VK_FRAME_PROFILE': '1',
    }
    if args.visual_capture:
        controls['OPENMW_VK_FX_CAPTURE_DIR'] = str(evidence / 'images')
    env, removed = profile.environment(os.environ, controls)
    manifest.update(command=command, controls=controls, effects=args.effects, removed_environment_names=removed,
                    performance_qualified=False, visual_capture=args.visual_capture,
                    executable_sha256=source['executable_sha256'], source_head=source['source_head'],
                    source_manifest_sha256=profile.dc.digest(package / 'source-changes.json'),
                    driver_sha256=profile.dc.digest(Path(__file__)), script_sha256=profile.dc.digest(script), state='prepared')
    path = evidence / 'manifest.json'
    path.write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print('START', args.name, args.effects, flush=True)
    with (evidence / 'console.log').open('wb') as console:
        process = subprocess.Popen(command, cwd=package, env=env, stdout=console, stderr=subprocess.STDOUT)
        manifest.update(state='running', pid=process.pid)
        path.write_text(json.dumps(manifest, indent=2), encoding='utf-8')
        code = process.wait()
    lines = (evidence / 'console.log').read_text(encoding='utf-8', errors='replace').splitlines()
    results = [json.loads(line.split('ARCHITECTURE_BENCHMARK_RESULT ', 1)[1]) for line in lines
               if 'ARCHITECTURE_BENCHMARK_RESULT ' in line]
    fx = [line for line in lines if 'Native OMWFX' in line or 'Native Vulkan OMWFX' in line]
    unchanged = {p: Path(p).is_file() and profile.dc.digest(Path(p)) == digest
                 for p, digest in manifest['original_chain_hashes'].items()}
    manifest.update(state='exited', exit_code=code, automated_result=results[-1] if results else None,
                    original_chain_unchanged=unchanged, fx_diagnostics=fx)
    path.write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print('EXIT', code, '\n'.join(fx), results[-1] if results else 'NO RESULT', flush=True)
    if code or not results or not all(unchanged.values()) or any('bypassed' in line or 'rejected' in line for line in fx):
        raise RuntimeError('Incomplete test; evidence preserved in ' + str(evidence))


if __name__ == '__main__':
    main()

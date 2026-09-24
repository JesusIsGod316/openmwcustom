"""Fixed exterior, same-executable local experiment; private data and natural exit.

Does not drive the desktop, kill a game, or read/write any normal save.
The original ordered config chain is checked by the diagnostic launcher.
Not a replacement for the user's route, interaction, or mod-compatibility test.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import statistics
from collections import defaultdict
from types import SimpleNamespace


def run(args):
    here = Path(__file__).resolve().parent
    spec = importlib.util.spec_from_file_location('capture', here / 'gameplay-diagnostics.py')
    capture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(capture)
    root = Path(args.output).resolve()
    root.mkdir(parents=True, exist_ok=False)
    capture.launch(SimpleNamespace(executable=args.executable, user_config=args.user_config,
        evidence_root=str(root), renderer=args.renderer, diagnostics='standard',
        cpu_fastpaths=args.fastpaths, source_head='unrecorded', source_diff_sha256='unrecorded',
        dll_directory=[], osg_library_path=None, prepare_only=True, confirm=False))
    manifest_path, = root.glob('*/manifest.json')
    evidence = manifest_path.parent
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    data = evidence / 'benchmark-data'
    script = data / 'scripts' / 'architecture-benchmark.lua'
    script.parent.mkdir(parents=True)
    shutil.copy2(here / 'architecture-benchmark.lua', script)
    (data / 'architecture-benchmark.omwscripts').write_text(
        'PLAYER: scripts/architecture-benchmark.lua\n', encoding='utf-8')
    with (evidence / 'openmw.cfg').open('a', encoding='utf-8') as stream:
        stream.write('data="' + data.as_posix() + '"\ncontent=architecture-benchmark.omwscripts\n')
    # Match the supplied capture's 1080p. These explicit overrides apply equally
    # to both arms, do not lower scene quality, and only remove artificial caps.
    with (evidence / 'settings.cfg').open('a', encoding='utf-8') as stream:
        stream.write('vsync = false\nframerate limit = 0\n')
    command = list(manifest['command'])
    command[command.index('--skip-menu=false')] = '--skip-menu=true'
    command += ['--start', 'Seyda Neen', '--random-seed', '123456', '--no-grab=true']
    env = os.environ.copy()
    for key in list(env):
        if key.startswith(('OPENMW_', 'VK_')):
            env.pop(key)
    env.update(manifest['controls'])
    if args.renderer == 'vulkan':
        env['OPENMW_V4_STATIC_FRUSTUM'] = '1'
        env['OPENMW_V4_TERRAIN_OCCLUSION'] = '1'
    for key in args.disable_fastpath:
        if key not in capture.CPU_FASTPATHS: raise ValueError('Unknown fast path: ' + key)
        env.pop(capture.CPU_FASTPATHS[key], None)
    manifest.update(command=command, state='running', automated_scene='Seyda Neen',
        benchmark_script_sha256=capture.sha256(script),
        benchmark_driver_sha256=capture.sha256(Path(__file__)), warmup_seconds=15, sample_seconds=30,
        controls={k: v for k, v in env.items() if k.startswith(('OPENMW_', 'VK_'))})
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print('Automated private exterior run:', evidence, flush=True)
    with (evidence / 'console.log').open('wb') as output:
        process = subprocess.Popen(command, cwd=manifest['cwd'], env=env, stdout=output, stderr=subprocess.STDOUT)
        manifest['pid'] = process.pid
        manifest_path.write_text(json.dumps(manifest, indent=2), encoding='utf-8')
        code = process.wait()  # Normal core.quit(); never terminate the user's game.
    manifest.update(state='exited', exit_code=code,
        original_chain_unchanged={p: Path(p).is_file() and capture.sha256(p) == h
                                  for p, h in manifest['original_chain_hashes'].items()})
    results = [line.split('ARCHITECTURE_BENCHMARK_RESULT ', 1)[1] for line in
               (evidence / 'console.log').read_text(encoding='utf-8', errors='replace').splitlines()
               if 'ARCHITECTURE_BENCHMARK_RESULT ' in line]
    manifest['automated_result'] = json.loads(results[-1]) if results else None
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    report = capture.report(evidence)
    rows = [json.loads(line) for line in (evidence / 'gameplay.jsonl').read_text(encoding='utf-8').splitlines()]
    frames = [r for r in rows if r.get('type') == 'frame_end' and r.get('completed') == '1']
    if frames:
        cutoff = max(r.get('time_us', 0) for r in frames) - 30_000_000
        steady = {r['frame'] for r in frames if r.get('time_us',0) >= cutoff}
        stages, work = defaultdict(list), defaultdict(list)
        for row in rows:
            if row.get('frame') not in steady: continue
            if row['type'] == 'stage_end': stages[row['name']].append(float(row['ms']))
            if row['type'] in ('capture_work','capture_phases','native_objects','residency','native_visibility','persistent_draws'):
                for key,value in row.items():
                    if key in ('schema','frame','time_us','type','truncated'): continue
                    try: work[key].append(float(value))
                    except (ValueError,TypeError): pass
        (evidence / 'steady-summary.json').write_text(json.dumps({
            'note': 'Sparse CPU inclusive stages in final 30 seconds; not GPU or additive timings.',
            'frames':len(steady),
            'stages':{k:statistics.median(v) for k,v in stages.items()},
            'counters':{k:statistics.median(v) for k,v in work.items()}}, indent=2), encoding='utf-8')
    print(json.dumps({'exit_code': code, 'result': manifest['automated_result'],
        'original_chain_unchanged': all(manifest['original_chain_unchanged'].values()),
        'findings': report['findings']}, indent=2), flush=True)
    if code or not results or not all(manifest['original_chain_unchanged'].values()):
        raise SystemExit('Automated run incomplete or failed; do not promote.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', required=True)
    parser.add_argument('--output', required=True, help='New experiment directory; will not overwrite')
    parser.add_argument('--user-config')
    parser.add_argument('--renderer', choices=('vulkan', 'opengl'), default='vulkan')
    parser.add_argument('--fastpaths', choices=('control', 'exterior', 'retained', 'previous-retained', 'all'), required=True)
    parser.add_argument('--disable-fastpath', action='append', default=[])
    run(parser.parse_args())

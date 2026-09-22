"""Bounded manual CP4F capture and multi-finding report. Never kills the game.

The launch uses a private writable config/log directory layered over the normal
configuration. User-data is isolated too; regular saves are not exposed by this
launch profile. No saves are read, copied, or deleted by the helper. It waits for natural exit, then summarizes the evidence.
The report subcommand also works on an interrupted/running capture.
"""
import argparse
from collections import Counter, defaultdict, deque
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import traceback


def sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def source_checkout(script_path):
    """Only inspect a real source checkout, never an installed game's ancestors."""
    path = Path(script_path).resolve()
    if len(path.parents) < 4:
        return None
    root = path.parents[3]
    if path != root / 'tools' / 'v4' / 'cp4' / 'gameplay-diagnostics.py' or not (root / '.git').exists():
        return None
    return root


def analyze(path):
    counts = Counter()
    stages = defaultdict(list)
    residency = []
    cell_insertions = []
    allocations = []
    terrain_work = []
    actors = defaultdict(lambda: {'pose': set(), 'placement': set(), 'geometry': set()})
    stack = []
    operations = {}
    findings = []
    frames = set()
    last = None
    with Path(path).open(encoding='utf-8', errors='replace') as stream:
        for line in stream:
            try:
                row = json.loads(line)
                if not isinstance(row, dict) or row.get('schema') != 1:
                    counts['unrecognized_rows'] += 1
                    continue
                kind = row['type']
                frame = row['frame']
                if kind not in ('operation_begin', 'operation_end', 'failure', 'terrain_preload_work', 'dynamic_allocation'):
                    frames.add(frame)
                last = row
                counts[kind] += 1
                if kind == 'failure':
                    findings.append('RUNTIME FAILURE: ' + row['message'])
                elif kind == 'operation_begin':
                    operations[row['id']] = (row['name'], row.get('identity', ''))
                elif kind == 'operation_end':
                    operations.pop(row['id'], None)
                    stages['loading:' + row['name']].append(float(row['ms']))
                    if int(row.get('unwinding', 0)):
                        findings.append(f"RUNTIME FAILURE: loading operation {row['name']} ({row.get('identity', '')}) exited during exception unwinding.")
                elif kind == 'dynamic_allocation':
                    allocations.append({'frame': frame, 'phase': row['phase'], 'summary': row['summary']})
                elif kind == 'terrain_preload_work':
                    terrain_work.append({'queue_ms': float(row['queue_ms']), 'work_ms': float(row['work_ms']),
                                         'views': int(row['views']), 'aborted': bool(int(row['aborted']))})
                elif kind == 'cell_insertion':
                    values = {key: float(value) for key, value in row.items() if key.endswith('_ms')}
                    cell_insertions.append({'cell': row['cell'], 'refs': int(row['refs']), **values})
                    for key, value in values.items():
                        stages['loading:insertion:' + key.removesuffix('_ms')].append(value)
                elif kind == 'residency':
                    residency.append({key: int(row[key]) for key in (
                        'actors_reused', 'actors_rebuilt', 'effects_reused', 'effects_rebuilt')})
                elif kind == 'stage_begin':
                    stack.append((frame, row['name']))
                elif kind == 'stage_end':
                    key = (frame, row['name'])
                    if key in stack:
                        stack.remove(key)
                    stages[row['name']].append(float(row['ms']))
                elif kind == 'frame_end':
                    counts['skipped_cull'] += int(row['skeleton_skipped_cull'])
                    counts['camera_callbacks'] += int(row['camera_callbacks'])
                    counts['detail_limit_frames'] += int(row.get('detail_limit', 0))
                elif kind == 'osg_update':
                    counts['viewer_done_samples'] += int(row['viewer_done'])
                elif kind in ('camera', 'pick'):
                    delta = float(row['cached_view_delta'] if kind == 'camera' else row['view_delta'])
                    counts['camera_invalid' if delta < 0 else 'camera_mismatch'] += int(delta < 0 or delta > 0.001)
                elif kind == 'actor_pose':
                    actors[row['actor']]['pose'].add(row['local_hash'])
                elif kind == 'actor_geometry':
                    actors[row['actor']]['placement'].add(row['placement_hash'])
                    actors[row['actor']]['geometry'].add(row['sample_hash'])
                    counts['stream_mismatches'] += int(row['stream_mismatches'])
                    counts['nonfinite'] += int(row['nonfinite'])
                elif kind == 'submission':
                    counts['submission_failures'] += int(int(row['submit']) != 0 or int(row['present']) != 0)
            except (ValueError, KeyError, TypeError):
                counts['malformed_rows'] += 1
    if counts['skipped_cull']:
        findings.append(f"OBSERVED: {counts['skipped_cull']} sampled skeleton updates skipped by the OSG visibility/cull gate.")
    if counts['viewer_done_samples']:
        findings.append(f"OBSERVED: OSG viewer was marked done in {counts['viewer_done_samples']} sampled update calls; check suppressed update/camera callbacks.")
    if counts['camera_mismatch'] or counts['camera_invalid']:
        findings.append(f"OBSERVED: camera disagreement in {counts['camera_mismatch']} samples; invalid matrices in {counts['camera_invalid']}. Rendering and cached/picking views are not equivalent at these points.")
    if counts['stream_mismatches'] or counts['nonfinite']:
        findings.append(f"OBSERVED: {counts['stream_mismatches']} CPU-to-resident stream discrepancies and {counts['nonfinite']} nonfinite sampled coordinates. This checks CPU-owned VSG arrays, not GPU readback.")
    if counts['submission_failures']:
        findings.append(f"OBSERVED: {counts['submission_failures']} non-success Vulkan submission/presentation results; inspect exact result codes.")
    # Initial resident construction is expected. Rebuilds continuing alongside
    # reuse across several sparse samples deserve investigation, not an automatic
    # failure: births, topology changes and new materials can require rebuilds.
    churn = [r for r in residency if r['effects_reused'] > 0 and r['effects_rebuilt'] > 0]
    if len(churn) >= 3:
        findings.append(f"INVESTIGATE effect resource churn: {len(churn)} sampled frames rebuilt effects while reusing others; median rebuilt count {statistics.median(r['effects_rebuilt'] for r in churn):g}. Check changing bounds/layout/materials and particle births; this is not a GPU timing measurement.")
    for actor, values in actors.items():
        if len(values['placement']) >= 3 and len(values['pose']) == 1:
            findings.append(f"INVESTIGATE actor {actor}: placement changed at least three times while sampled local pose stayed constant. This can explain gliding, but idle/root-only movement must be ruled out.")
    if not counts['actor_pose']:
        findings.append('COVERAGE GAP: no actor pose samples; reaching the menu is not a gameplay test.')
    if not counts['pick']:
        findings.append('COVERAGE GAP: no picking samples; interaction parity is not tested.')
    if stack:
        findings.append('INCOMPLETE STAGES (may be running or interrupted, not automatically a crash): ' + repr(stack[-8:]))
    if operations:
        findings.append('INCOMPLETE LOADING OPERATIONS (may still be running): ' + repr(list(operations.values())[-8:]))
    if counts['malformed_rows']:
        findings.append(f"CAPTURE WARNING: {counts['malformed_rows']} malformed/incomplete lines were skipped.")
    if counts['detail_limit_frames'] or counts['capture_limit']:
        findings.append('CAPTURE WARNING: a sampling/detail limit was reached; absence of further errors is not a pass.')
    timings = {name: {'samples': len(values), 'median_ms': statistics.median(values), 'max_ms': max(values)}
               for name, values in stages.items() if values}
    return {'counts': dict(counts), 'sampled_frames': counts['frame_begin'],
            'distinct_osg_frame_stamps': len(frames), 'last_record': last,
            'findings': findings, 'inclusive_stage_timings': timings,
            'cell_insertions': cell_insertions, 'residency_samples': residency,
            'allocation_samples': allocations, 'terrain_preload_work': terrain_work,
            'actor_variation': {key: {k: len(v) for k, v in values.items()} for key, values in actors.items()}}


def report(directory):
    directory = Path(directory)
    trace = directory / 'gameplay.jsonl'
    result = analyze(trace) if trace.exists() else {'findings': ['CAPTURE FAILURE: no gameplay trace was produced.'], 'counts': {}, 'inclusive_stage_timings': {}}
    log = directory / 'openmw.log'
    selected = deque(maxlen=120)
    log_failures = set()
    loading_seen = False
    error_lines = 0
    if log.exists():
        with log.open(encoding='utf-8', errors='replace') as stream:
            for line in stream:
                loading_seen |= 'Loading cell ' in line
                error_lines += int(' E]' in line)
                if any(term in line for term in ('Fatal error:', 'V4 frame failure:')) and len(log_failures) < 128:
                    log_failures.add(line.rstrip().split('] ', 1)[-1])
                if any(term in line for term in (' E]', 'Renderer backend:', 'Loading cell ', 'Quitting peacefully', 'Failed to load', 'Vulkan record/submit')):
                    selected.append(line.rstrip())
    result['findings'].extend('RUNTIME FAILURE (game log): ' + message for message in sorted(log_failures))
    if error_lines:
        result['findings'].append(f'LOG ERRORS: {error_lines} error-level lines; these include potentially unrelated script/configuration errors. See selected log; not all are fatal.')
    if loading_seen and not result['counts'].get('operation_begin'):
        result['findings'].append('COVERAGE GAP: cell loading occurred without independent loading-operation records; sampled gameplay frames do not cover the transition.')
    manifest = directory / 'manifest.json'
    if manifest.exists():
        try:
            status = json.loads(manifest.read_text(encoding='utf-8'))
            result['run_state'] = status.get('state', 'unknown')
            result['exit_code'] = status.get('exit_code')
            if status.get('diagnostics') == 'standard':
                result['findings'] = [f for f in result['findings'] if not f.startswith('COVERAGE GAP: no actor pose samples')]
                result['findings'].append('STANDARD COVERAGE: detailed actor geometry/pose fingerprints are intentionally omitted; use focused mode for those checks.')
            if status.get('startup_refused'):
                result['findings'].append('STARTUP REFUSED: a startup/load failure was recorded, even if the process returned zero.')
            if status.get('exit_code') not in (None, 0):
                result['findings'].append(f"RUN FAILED: executable exit code {status['exit_code']}; no observed frame invariant violation is not a pass.")
            elif status.get('state') != 'exited':
                result['findings'].append('RUN INCOMPLETE: collector has not recorded process exit.')
        except (ValueError, OSError, AttributeError):
            result['findings'].append('CAPTURE WARNING: unreadable/incomplete run manifest.')
    result['selected_log_lines'] = list(selected)
    (directory / 'report.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    text = ['# CP4F gameplay diagnostic report', '',
            'Observational, sparsely sampled run. Not a benchmark or a complete compatibility pass.', '',
            *result['findings'], '', '## Inclusive sampled stage times', '',
            'Nested stages overlap: do not add their times or interpret them as GPU execution times.', '']
    for name, timing in sorted(result['inclusive_stage_timings'].items(), key=lambda x: x[1]['median_ms'], reverse=True):
        text.append(f"- {name}: median {timing['median_ms']:.3f} ms, max {timing['max_ms']:.3f} ms ({timing['samples']} samples)")
    text += ['', '## Allocation and terrain preparation observations', '',
             'Image payload sizes are not actual VRAM allocation sizes. Pool totals exclude driver/pipeline memory.', '']
    for sample in result.get('allocation_samples', [])[-8:]:
        text.append(f"- Frame {sample['frame']} {sample['phase']}: {sample['summary']}")
    for sample in result.get('terrain_preload_work', []):
        text.append(f"- Terrain: queued {sample['queue_ms']:.3f} ms, worker {sample['work_ms']:.3f} ms, "
                    f"views {sample['views']}, aborted {sample['aborted']} (overlaps terrain wait).")
    text += ['', '## Selected game log', '', '```text', *list(selected), '```', '',
             'Remaining coverage: full canonical-to-neutral skin-space equivalence, GPU-side buffer contents, pixels, texture semantics, map output, and long-duration memory budgets require separate checks.']
    (directory / 'report.md').write_text('\n'.join(text) + '\n', encoding='utf-8')
    runtime_spec = importlib.util.spec_from_file_location('runtime_diagnostics', Path(__file__).with_name('runtime-diagnostics.py'))
    if Path(runtime_spec.origin).exists():
        runtime_module = importlib.util.module_from_spec(runtime_spec)
        runtime_spec.loader.exec_module(runtime_module)
        runtime_module.report(directory)
    return result


def launch(args):
    exe = Path(args.executable).resolve(strict=True)
    spec = importlib.util.spec_from_file_location('shader_resources', Path(__file__).with_name('shader_resources.py'))
    shader_resources = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(shader_resources)
    # Refuse an incomplete build before starting the game or changing any config.
    shader_package = shader_resources.verify(exe.parent / 'resources' / 'shaders')
    config_spec = importlib.util.spec_from_file_location('diagnosticconfig', Path(__file__).with_name('diagnosticconfig.py'))
    config = importlib.util.module_from_spec(config_spec)
    config_spec.loader.exec_module(config)
    user = Path(args.user_config or config.normal_default()).resolve(strict=True)
    source_head = config.package_identity(exe, args.source_head)
    active_config, chain_hashes = config.inspect_chain(exe.parent, user, config.normal_default().resolve())
    root = Path(args.evidence_root or exe.parent / 'Test-Results').resolve()
    stamp = datetime.now().strftime('%Y%m%d-%H%M%S')
    evidence = root / (stamp + '-gameplay-' + str(os.getpid()))
    evidence.mkdir(parents=True, exist_ok=False)
    private_data = evidence / 'user-data'
    private_data.mkdir()
    (evidence / 'openmw.cfg').write_text(
        '# Private diagnostic layer: no repeated content or autoload.\n'
        + 'user-data=' + config.quoted(private_data) + '\n', encoding='utf-8')
    renderer = getattr(args, 'renderer', 'vulkan')
    if renderer not in ('vulkan', 'opengl'):
        raise ValueError('Invalid diagnostic renderer')
    (evidence / 'settings.cfg').write_text(
        '[Video]\nrenderer backend = ' + renderer + '\nrenderer fallback = false\n'
        'resolution x = 1920\nresolution y = 1080\nwindow mode = 2\n', encoding='utf-8')
    fingerprints = {name: sha256(user / name) for name in ('openmw.cfg', 'settings.cfg', *config.COPY_NAMES)
                    if (user / name).is_file()}
    for name in config.COPY_NAMES:
        candidates = [directory / name for directory in active_config if (directory / name).is_file()]
        if candidates:
            shutil.copy2(candidates[-1], evidence / name)
    env = os.environ.copy()
    selected_mode = getattr(args, 'diagnostics', 'standard')
    if selected_mode not in ('off', 'standard', 'focused'):
        raise ValueError('Invalid runtime diagnostic mode')
    env['OPENMW_RUNTIME_DIAGNOSTICS'] = selected_mode
    env['OPENMW_RUNTIME_DIAGNOSTICS_FILE'] = str(evidence / 'runtime.jsonl')
    env['OPENMW_GAMEPLAY_DIAGNOSTICS'] = '0' if selected_mode == 'off' else '1'
    env['OPENMW_GAMEPLAY_DIAGNOSTICS_FILE'] = str(evidence / 'gameplay.jsonl')
    # Do not enable the older per-frame strict logger or GPU validation by
    # default: they have distinct overhead. Record any inherited controls.
    if args.dll_directory:
        env['PATH'] = os.pathsep.join(args.dll_directory + [env.get('PATH', '')])
    if args.osg_library_path:
        env['OSG_LIBRARY_PATH'] = args.osg_library_path
    # Override the default package config expansion, but preserve the selected
    # user's ordered content chain exactly once. No empty load-savegame path.
    command = config.build_command(exe, user, evidence)
    manifest = {'started_utc': datetime.now(timezone.utc).isoformat(), 'executable': str(exe),
                'sha256': sha256(exe), 'shader_package': shader_package, 'command': command, 'cwd': str(exe.parent),
                'source_head': source_head, 'source_diff_sha256': args.source_diff_sha256,
                'original_config_hashes': fingerprints, 'original_chain_hashes': chain_hashes,
                'launcher_revision': 'combined-repair-1', 'requested_renderer': renderer,
                'controls': {k: v for k, v in env.items() if k.startswith(('OPENMW_', 'VK_')) or k == 'OSG_LIBRARY_PATH'},
                'dll_directories': args.dll_directory, 'state': 'starting', 'evidence': str(evidence),
                'diagnostics': selected_mode, 'isolated_user_data': str(private_data), 'regular_saves_copied': False}
    source_root = source_checkout(__file__)
    try:
        if source_root is None:
            raise OSError('Installed diagnostic helper has no source checkout')
        changed = subprocess.check_output(['git', 'ls-files', '-m', '-o', '--exclude-standard', '-z'], cwd=source_root)
        paths = sorted(set(p for p in changed.decode('utf-8').split('\0') if p))
        manifest['source_root'] = str(source_root)
        manifest['source_file_hashes'] = {p: sha256(source_root / p) for p in paths if (source_root / p).is_file()}
    except (OSError, subprocess.SubprocessError):
        manifest['source_file_hashes'] = 'unavailable'
    manifest_path = evidence / 'manifest.json'
    def save_manifest():
        manifest_path.write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    save_manifest()
    print('Private capture:', evidence, flush=True)
    print(f'{renderer} / 1920x1080 / {selected_mode}. Choose NEW GAME only; no normal saves copied.', flush=True)
    if getattr(args, 'prepare_only', False):
        manifest['state'] = 'prepared_only'
        save_manifest()
        return
    if getattr(args, 'confirm', False):
        input('Close other OpenMW instances normally, then press Enter to launch this test: ')
    try:
        with (evidence / 'console.log').open('wb') as output:
            process = subprocess.Popen(command, cwd=exe.parent, env=env, stdout=output, stderr=subprocess.STDOUT)
            manifest.update(pid=process.pid, state='running')
            save_manifest()
            manifest['exit_code'] = process.wait()  # no timeout, input injection or forced termination
            manifest['state'] = 'exited'
    except BaseException:
        manifest['state'] = 'collector_interrupted_or_launch_failed'
        manifest['collector_error'] = traceback.format_exc()
        raise
    finally:
        manifest['finished_utc'] = datetime.now(timezone.utc).isoformat()
        manifest['original_config_unchanged'] = {
            name: (user / name).exists() and sha256(user / name) == digest for name, digest in fingerprints.items()}
        manifest['original_chain_unchanged'] = {
            name: Path(name).is_file() and sha256(name) == digest for name, digest in chain_hashes.items()}
        log = evidence / 'openmw.log'
        if log.is_file():
            # Exit zero is not proof of a usable startup (config aborts use it).
            with log.open(encoding='utf-8', errors='replace') as stream:
                manifest['startup_refused'] = any('Aborting...' in line or 'Failed to start new game:' in line
                                                 or 'Failed to load saved game:' in line for line in stream)
        save_manifest()
        try:
            report(evidence)
        except Exception:
            (evidence / 'report-generation-error.txt').write_text(traceback.format_exc(), encoding='utf-8')
        # Evidence only: never package saves, asset files, storage or original configs.
        import zipfile
        with zipfile.ZipFile(evidence.with_suffix('.zip'), 'w', zipfile.ZIP_DEFLATED, allowZip64=True) as bundle:
            for name in ('manifest.json', 'console.log', 'openmw.log', 'gameplay.jsonl', 'runtime.jsonl',
                         'report.md', 'report.json', 'memory-report.md', 'memory-report.json', 'report-generation-error.txt'):
                path = evidence / name
                if path.is_file():
                    bundle.write(path, arcname=name)
        print('Evidence ZIP:', evidence.with_suffix('.zip'), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='mode', required=True)
    read = sub.add_parser('report')
    read.add_argument('directory')
    run = sub.add_parser('launch')
    run.add_argument('--executable', default=str(Path(__file__).with_name('openmw.exe')))
    run.add_argument('--user-config')
    run.add_argument('--evidence-root')
    run.add_argument('--renderer', choices=('vulkan', 'opengl'), default='vulkan')
    run.add_argument('--confirm', action='store_true')
    run.add_argument('--prepare-only', action='store_true')
    run.add_argument('--dll-directory', action='append', default=[])
    run.add_argument('--osg-library-path')
    run.add_argument('--diagnostics', choices=('off', 'standard', 'focused'), default='standard')
    run.add_argument('--source-head', default='unrecorded')
    run.add_argument('--source-diff-sha256', default='unrecorded')
    args = parser.parse_args()
    if args.mode == 'launch':
        launch(args)
    else:
        result = report(args.directory)
        print('\n'.join(result['findings']) or 'No covered invariant violation observed; this is not full acceptance.')


if __name__ == '__main__':
    main()

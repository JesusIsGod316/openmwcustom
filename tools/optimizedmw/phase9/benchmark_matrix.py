"""Sequential, muted private-desktop comparisons with pinned route/build inputs.

Guided playback is a separate cohort. Functional completion never promotes a
candidate, and deep captures are not used as performance comparisons.
"""
import argparse
import bisect
import csv
from datetime import datetime, timedelta
import hashlib
import json
import math
from pathlib import Path
import re
import statistics
import sys

import benchmark_desktop


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def identity(path):
    path = Path(path).resolve(strict=True)
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    return {'path': str(path), 'sha256': digest, 'bytes': path.stat().st_size}


def verify(pins):
    for pin in pins:
        live = identity(pin['path'])
        if live['sha256'].lower() != pin['sha256'].lower() or live['bytes'] != pin['bytes']:
            raise ValueError('Pinned input changed: ' + pin['path'])


def quantile(values, fraction):
    values = sorted(values)
    if not values:
        return None
    index = (len(values) - 1) * fraction
    lower = int(index)
    return values[lower] + (values[math.ceil(index)] - values[lower]) * (index - lower)


def pacing(values):
    if not values or any(not math.isfinite(v) or v < 0 for v in values):
        raise ValueError('No valid frame-time observations')
    median = statistics.median(values)
    adjacent = [abs(a-b) for a,b in zip(values,values[1:])]
    relative = []
    history, ordered = [], []
    for value in values:
        if len(history) >= 15:
            local = statistics.median(ordered)
            if value > max(local*1.5, local+2):
                relative.append(value-local)
        bisect.insort(ordered,value); history.append(value)
        if len(history) > 31:
            old = history.pop(0); ordered.pop(bisect.bisect_left(ordered,old))
    return {'frames': len(values), 'mean_ms': statistics.mean(values), 'median_ms': median,
            'p90_ms': quantile(values,.90), 'p95_ms': quantile(values,.95),
            'p99_ms': quantile(values,.99), 'p999_ms': quantile(values,.999),
            'maximum_ms': max(values), 'stddev_ms': statistics.pstdev(values),
            'mad_ms': statistics.median(abs(v-median) for v in values),
            'iqr_ms': quantile(values,.75)-quantile(values,.25),
            'adjacent_change_p95_ms': quantile(adjacent,.95),
            'adjacent_change_p99_ms': quantile(adjacent,.99),
            'relative_spikes': len(relative), 'relative_excess_ms': sum(relative),
            'over_16_67': sum(v>16.67 for v in values), 'over_33_33': sum(v>33.33 for v in values),
            'over_50': sum(v>50 for v in values)}


def route_windows(profile, launch_utc):
    """Join log-prefix wall time to frames; never join Lua boot time to epoch."""
    day = datetime.fromisoformat(launch_utc.replace('Z','+00:00')).astimezone().date()
    previous = None
    result = {'start': None, 'movement': None, 'finish': None}
    for line in (profile/'playback-stdout.log').read_text(encoding='utf-8-sig').splitlines():
        if 'OMWPLAY|' not in line:
            continue
        match = re.match(r'^\[(\d\d:\d\d:\d\d\.\d{3}) ',line)
        if not match:
            continue
        timestamp = datetime.combine(day,datetime.strptime(match[1],'%H:%M:%S.%f').time())
        if previous and timestamp < previous-timedelta(hours=12):
            day += timedelta(days=1); timestamp += timedelta(days=1)
        previous = timestamp
        record = json.loads(line.split('OMWPLAY|',1)[1])
        epoch = timestamp.timestamp()*1000
        if record['event'] == 'start':
            result['start'] = epoch
        guidance = record.get('guidance_command') or record.get('last_guidance_command') or {}
        if result['start'] and not result['movement'] and (
                abs(guidance.get('nominal_movement',0))+abs(guidance.get('nominal_side_movement',0)) > .01):
            result['movement'] = epoch
        if record['event'] == 'finish':
            result['finish'] = epoch
    if not result['start'] or not result['finish'] or result['finish'] <= result['start']:
        raise ValueError('Cannot establish route wall-time boundaries')
    return result


def analyze(profile):
    profile = Path(profile)
    manifest = read_json(profile/'RUN-MANIFEST.json')
    windows = route_windows(profile,manifest['process']['launch_utc'])
    context = {}
    with (profile/'benchmark-context.csv').open(newline='',encoding='utf-8-sig') as stream:
        for row in csv.DictReader(stream):
            context[int(row['frame'])] = row
    groups = {'route_all_frames': [], 'route': [], 'fixed_view': [], 'moving': []}
    excluded = 0
    with (profile/'v3-frame.csv').open(newline='',encoding='utf-8-sig') as stream:
        for row in csv.DictReader(stream):
            epoch = float(row['epoch_ms'])
            if not windows['start']+1000 <= epoch < windows['finish']-1000:
                continue
            groups['route_all_frames'].append(float(row['wall_ms']))
            state = context.get(int(row['frame']))
            if not state or any(int(state[k]) != 0 for k in ('begin_flags','end_flags','observed_flags')):
                excluded += 1; continue
            value = float(row['wall_ms']); groups['route'].append(value)
            if windows['movement']:
                groups['fixed_view' if epoch < windows['movement']-1000 else 'moving'].append(value)
    return {'schema': 1, 'phase_clock': 'approximate logged wall-time prefixes joined to system-clock frame starts',
            'phase_limit': 'One-second boundary margins; buffered log delivery is not an exact CPU span join',
            'context_limit': 'route_all_frames retains loading/UI deviations; route/fixed_view/moving require zero context flags',
            'windows_epoch_ms': windows, 'excluded_context_frames': excluded,
            'metrics': {key:pacing(values) for key,values in groups.items() if values}}


def engagement(profile, requested, asset_mask=0):
    text = (profile/'openmw.log').read_text(encoding='utf-8-sig')
    positive = {}
    if asset_mask & 1:
        # A successful cook/cache lookup is not proof that visible LOD draws
        # consumed the reduced product in the real active-grid renderer.
        matches = re.findall(r'active_consumed_draws=(\d+)(?=\s|$)',text)
        positive['active_grid_lod_draws'] = bool(matches and int(matches[-1])>0)
    if requested.get('prepare material images') == 'true':
        matches = re.findall(r'OptimizedMW material image preparation:.*?decoded=(\d+)(?=\s|$)',text)
        positive['material_images'] = bool(matches and int(matches[-1])>0)
    if requested.get('lod cache hit admission') == 'true':
        matches = re.findall(r'cache_fast_hits=(\d+)(?=\s|$)',text)
        positive['lod_cache_hit'] = bool(matches and int(matches[-1])>0)
    if requested.get('lod retain source indices') == 'true':
        matches = re.findall(r'primitive_clone_bytes_avoided=(\d+)(?=\s|$)',text)
        positive['lod_source_index_reuse'] = bool(matches and int(matches[-1])>0)
    if requested.get('ordinary opaque packets') == 'true':
        matches = re.findall(r'ordinary_branches=(\d+)(?=\s|$)',text)
        submitted = re.findall(r'OptimizedMW persistent opaque packets:.*?submitted=(\d+)(?=\s|$)',text)
        positive['ordinary_packets'] = bool(matches and int(matches[-1])>0 and submitted and int(submitted[-1])>0)
    if requested.get('indirect command reuse') == 'true':
        matches = re.findall(r'command_uploads_avoided=(\d+)(?=\s|$)',text)
        positive['indirect_command_reuse'] = bool(matches and int(matches[-1])>0)
    return {'positive_work': positive, 'all_requested_positive': all(positive.values()),
            'limit': 'Preparation/dispatch engagement; visible resource adoption and performance require separate evidence'}


def cell_crossings(profile, expected_count):
    """Audit sampled boundary skew without hiding wrong-cell route excursions.

    A physical follower can cross an exterior boundary a fraction of a second
    before/after its reference. Preserve the mismatch, require the same ordered
    cell itinerary, and bound BOTH positions and the observed mismatch span.
    Bound each unobserved bracketing gap separately; two matching endpoints
    are sampling uncertainty, not observations of a wrong cell. Preserve the
    complete surrounding window rather than claiming exact transition timing.
    Interior transitions, skipped cells, sustained mismatches and wider path
    departures remain failures. This never claims frame-exact cell ownership.
    """
    if not expected_count:
        return {'qualification':'EXACT_SAMPLED_CELLS','mismatch_checks':0}
    if profile is None:
        raise ValueError('Cell mismatch requires the complete route evidence')
    with (Path(profile)/'playback.jsonl').open(encoding='utf-8-sig') as stream:
        checks = [json.loads(line) for line in stream if line.strip()]
    checks = [row for row in checks if row.get('event')=='check']
    mismatches = [i for i,row in enumerate(checks) if not row.get('cell_matches')]
    if len(mismatches)!=expected_count:
        raise ValueError('Cell mismatch count does not match raw route checks')
    def itinerary(key):
        cells=[]
        for row in checks:
            if not cells or cells[-1]!=row[key]: cells.append(row[key])
        return cells
    if itinerary('actual_cell')!=itinerary('expected_cell'):
        raise ValueError('Playback changed the ordered exterior cell itinerary')
    def exterior(value):
        match=re.fullmatch(r'Esm3ExteriorCell:(-?\d+):(-?\d+)',value)
        if not match: raise ValueError('Interior or unknown cell mismatch')
        return tuple(map(int,match.groups()))
    windows=[]
    reference_interpolation_skew=0
    start=0
    while start<len(mismatches):
        end=start
        while end+1<len(mismatches) and mismatches[end+1]==mismatches[end]+1: end+=1
        first,last=mismatches[start],mismatches[end]
        if first==0 or last==len(checks)-1:
            raise ValueError('Unclosed cell-boundary mismatch')
        span=checks[last]['t_wall']-checks[first]['t_wall']
        before_gap=checks[first]['t_wall']-checks[first-1]['t_wall']
        after_gap=checks[last+1]['t_wall']-checks[last]['t_wall']
        duration=checks[last+1]['t_wall']-checks[first-1]['t_wall']
        if not 0<=span<=.5 or not 0<before_gap<=.5 or not 0<after_gap<=.5:
            raise ValueError('Cell mismatch span or bracketing sample gap exceeds half a second')
        for i in mismatches[start:end+1]:
            row=checks[i]; a=row['actual_position']; b=row['expected_position']
            actual=exterior(row['actual_cell']); expected=exterior(row['expected_cell'])
            if sum(abs(x-y) for x,y in zip(actual,expected))!=1:
                raise ValueError('Cell mismatch is not one adjacent exterior boundary')
            coordinates=lambda p:tuple(math.floor(p[axis]/8192) for axis in ('x','y'))
            if coordinates(a)!=actual:
                raise ValueError('Actual cell mismatch is not explained by its exterior coordinates')
            if coordinates(b)!=expected:
                # The replay interpolates reference POSITION between acquired
                # samples but takes CELL ID from the earlier sample. Retain this
                # protocol skew only at that same boundary and within one source
                # sample's travel; the actual physical cell remains exact.
                axis=next(i for i in range(2) if actual[i]!=expected[i])
                name=('x','y')[axis]; boundary=max(actual[axis],expected[axis])*8192
                interval=row.get('sample_interval',0); speed=(row.get('guidance_command') or {}).get('speed',0)
                allowance=min(64,interval*speed+1)
                if coordinates(b)!=actual or not 0<interval<=.1 or not 0<speed<=1000 or abs(b[name]-boundary)>allowance:
                    raise ValueError('Reference cell mismatch exceeds recorded interpolation travel')
                reference_interpolation_skew+=1
            if math.hypot(a['x']-b['x'],a['y']-b['y'])>64 or abs(a['z']-b['z'])>32:
                raise ValueError('Boundary mismatch exceeds the narrow physical path allowance')
        windows.append({'first_seq':checks[first]['seq'],'last_seq':checks[last]['seq'],
                        'observed_mismatch_seconds':span,
                        'before_sample_gap_seconds':before_gap,
                        'after_sample_gap_seconds':after_gap,
                        'surrounding_sample_seconds':duration})
        start=end+1
    return {'qualification':'BOUNDED_EXTERIOR_BOUNDARY_SKEW','mismatch_checks':expected_count,
            'same_ordered_cell_itinerary':True,'windows':windows,
            'reference_interpolation_skew_checks':reference_interpolation_skew,
            'limit':'Mismatch span and each bracketing gap are bounded separately; the complete surrounding window is sampling uncertainty, not frame-exact transition timing'}


def route_quality(receipt, profile=None):
    playback = receipt['playback']; finish = playback.get('finish') or {}
    if not playback.get('playback_complete') or playback.get('errors'):
        raise ValueError('Route did not complete cleanly')
    policy = finish.get('policy') or {}
    if not policy.get('qualified') or policy.get('combat_during_recording') or not finish.get('policy_observer_drained'):
        raise ValueError('Actor/combat policy incomplete')
    if finish.get('outside_position_tolerance_check_count',1):
        raise ValueError('Playback changed the physical path cohort')
    crossing=cell_crossings(profile,finish.get('cell_mismatch_check_count',1))
    return {'lag_seconds':finish['reference_clock_lag'], 'hold_seconds':finish['reference_hold_seconds'],
            'hold_count':finish['reference_hold_count'], 'maximum_xy':finish['maximum_horizontal_error'],
            'maximum_z':finish['maximum_vertical_error'], 'route_wall_seconds':finish['t_wall'],
            'physical_collision':finish['physical_state']['collision_enabled'],
            'cell_crossings':crossing,
            'scope':'guided route; input_replay_complete remains distinct'}


def completed_run_receipts(directory, launcher_code):
    directory = Path(directory)
    desktop_path = directory/'Desktop'/'DESKTOP-RECEIPT.json'
    desktop = read_json(desktop_path)
    receipt_path = directory/'RUN.json'
    if desktop.get('timed_out'):
        raise ValueError('Private launcher timed out after '
            + str(desktop.get('elapsed_seconds', 'unknown')) + ' seconds; preserved '
            + str(desktop_path) + '; automatic run receipt '
            + ('exists but is not qualified' if receipt_path.exists() else 'was not completed'))
    if launcher_code or desktop.get('exit_code') != 0:
        raise ValueError('Private launcher failed with exit code ' + str(launcher_code)
            + '; desktop exit code ' + str(desktop.get('exit_code'))
            + '; preserved ' + str(desktop_path))
    if not receipt_path.exists():
        raise ValueError('Private launcher exited without an automatic run receipt; preserved '
            + str(desktop_path))
    receipt = read_json(receipt_path)
    if receipt.get('launch_failure') or receipt.get('exit_code') != 0:
        raise ValueError('Game/launcher failed; preserved ' + str(receipt_path)
            + '; game exit code ' + str(receipt.get('exit_code'))
            + '; launcher error ' + str(receipt.get('launch_failure')))
    return receipt, desktop


def run_matrix(plan_path, output):
    plan = read_json(plan_path); output = Path(output).resolve()
    if output.exists():
        raise ValueError('Preserving prior matrix output; choose a fresh directory')
    output.mkdir(parents=True)
    game = Path(plan['game_directory']).resolve(strict=True)
    catalog = read_json(plan['route_catalog'])
    routes = {route['id']:route for route in catalog['routes']}
    tools = ('OptimizedMW_Test.ps1','OptimizedMW_RoutePlayback.ps1','OptimizedMW_RouteRecorder.ps1')
    pins = [identity(game/'openmw.exe'),identity(plan['symbols']),identity(game/'BUILD-IDENTITY.json'),
            identity(plan_path),identity(plan['route_catalog']),identity(__file__),identity(benchmark_desktop.__file__)]
    pins += [identity(path) for path in sorted(game.glob('OptimizedMW_*.ps1'))]
    pins += [identity(path) for path in sorted((game/'RoutePlayback').rglob('*')) if path.is_file()]
    for route in routes.values():
        pins += route['inputs']
    verify(pins)
    state = {'schema':1,'scope':'OFFSCREEN_MUTED_GUIDED_ROUTE_COMPARISON','pins':pins,'runs':[],
             'performance_status':'CALIBRATION_REQUIRED','state':'running'}
    benchmark_desktop.save(output/'MATRIX.json',state)
    current_case = None
    try:
        for index,case in enumerate(plan['cases']):
            if not re.fullmatch(r'[A-Za-z0-9_-]{1,80}',case['id']):
                raise ValueError('Invalid case name')
            verify(pins)
            directory = output/f'{index+1:02}_{case["id"]}'; directory.mkdir()
            current_case = {'id':case['id'], 'route':case['route'], 'directory':str(directory),
                'stage':'setup', 'receipt':str(directory/'RUN.json'),
                'desktop_receipt':str(directory/'Desktop'/'DESKTOP-RECEIPT.json')}
            route = routes[case['route']]
            settings = case.get('settings',[])
            settings_path = directory/'requested-settings.json'; benchmark_desktop.save(settings_path,settings)
            receipt_path = directory/'RUN.json'
            args = ['-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',str(game/'OptimizedMW_Test.ps1'),
                    '-GameDirectory',str(game),'-AutomatedRouteCapture',route['pinned_acquisition'],
                    '-AutomatedRouteName','balmora' if case['route']=='Balmora' else 'existing',
                    '-AutomatedResultFile',str(receipt_path),'-ExperimentSettingsFile',str(settings_path),
                    '-AutomatedDerivedCacheDirectory',str(output/'DerivedAssets'),
                    '-NoPrompt','-AttackMask','1','-RamMask','4','-MemoryProfile','Headroom',
                    '-ArchitectureMask',str(case.get('architecture_mask',0)),
                    '-AssetMask',str(case.get('asset_mask',0)),'-TraceLevel',case.get('depth','Normal'),'-Candidate','CONTROL']
            print(json.dumps({'case':case['id'],'route':case['route'],'state':'started'}),flush=True)
            current_case['stage'] = 'private_launcher'
            code = benchmark_desktop.launch(plan['powershell'],args,game,directory/'Desktop',plan.get('deadline',600))
            current_case['stage'] = 'run_qualification'
            receipt, desktop = completed_run_receipts(directory, code)
            if not desktop.get('owned_render_window_observed') or not desktop['input_desktop_unchanged'] or desktop.get('input_desktop_owned_visible_windows'):
                raise ValueError('Private rendering isolation incomplete')
            if not receipt['settings_restore_verified'] or not receipt['environment_restore_verified']:
                raise ValueError('Restore qualification incomplete')
            verify(pins+[receipt['zip']])
            profile = Path(receipt['profile_directory'])
            text = (profile/'openmw.log').read_text(encoding='utf-8-sig')
            if 'OptimizedMW automatic benchmark: master_volume=0' not in text:
                raise ValueError('Engine did not confirm actual muted settings')
            quality = route_quality(receipt,profile)
            requested = {entry['key']:entry['value'] for entry in settings
                         if entry['section'] in ('Cells', 'Optimizations')}
            engaged = engagement(profile,requested,case.get('asset_mask',0))
            metrics = analyze(profile)
            benchmark_desktop.save(directory/'ANALYSIS.json',metrics)
            result = {'id':case['id'],'route':case['route'],'depth':case.get('depth','Normal'),
                      'receipt':str(receipt_path),'zip':receipt['zip'],'route_quality':quality,
                      'engagement':engaged,'analysis':metrics}
            state['runs'].append(result); benchmark_desktop.save(output/'MATRIX.json',state)
            print(json.dumps({'case':case['id'],'state':'completed','positive':engaged}),flush=True)
            if not engaged['all_requested_positive']:
                raise ValueError('Candidate was inert; stop broad comparisons and inspect '+str(receipt_path))
            current_case = None
        state['state']='completed'
        # Calibration must be reviewed across repeats before accepting numerical
        # A/B results. Deep rows are never pooled with ordinary performance.
        state['performance_status']='RAW_COMPARISONS_PENDING_REPEATABILITY_REVIEW'
    except BaseException as error:
        state['state']='failed'; state['error']=str(error)
        if current_case:
            failure = dict(current_case, state='FAILED_NOT_QUALIFIED', error=str(error),
                receipt_exists=Path(current_case['receipt']).exists(),
                desktop_receipt_exists=Path(current_case['desktop_receipt']).exists(),
                performance_status='No performance acceptance for this failed case')
            state['failed_run'] = failure
            benchmark_desktop.save(Path(current_case['directory'])/'FAILED-RUN.json',failure)
        raise
    finally:
        benchmark_desktop.save(output/'MATRIX.json',state)


if __name__ == '__main__':
    parser=argparse.ArgumentParser(); parser.add_argument('--plan',required=True);parser.add_argument('--output',required=True)
    args=parser.parse_args()
    run_matrix(args.plan,args.output)

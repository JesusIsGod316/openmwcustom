"""Cumulative draw-work observations within qualified benchmark intervals.

These snapshots include all participating views and, in the extended format,
completed paging-work counters. Their frame is the main-thread
observation point, never the origin frame of an asynchronous draw or GPU query.
"""
import bisect
import csv
import math
from pathlib import Path

COUNTERS = ('indirect_draws', 'commands', 'gpu_dispatches', 'command_uploads',
            'command_uploads_avoided', 'capability_draws', 'gpu_function_resolutions_avoided',
            'opportunity_original_draws', 'requested_dispatches_avoided')
CONSUMPTION_COUNTERS = ('projection_proof_reuses', 'early_admission_fallbacks', 'early_busy_fallbacks',
                        'paging_score_table_runs', 'paging_pow_reuses', 'paging_pow_computations',
                        'paging_uncached_pow_computations')
EXTENDED_COUNTERS = (*COUNTERS, *CONSUMPTION_COUNTERS)
PREPARATION_COUNTERS = ('preload_priority_submitted', 'preload_priority_promoted',
                        'preload_priority_started', 'preload_priority_fairness')
PREPARATION_EXTENDED_COUNTERS = (*EXTENDED_COUNTERS, *PREPARATION_COUNTERS)


def interval_work(profile, windows):
    profile = Path(profile)
    status = dict(line.split('=', 1) for line in
                  (profile/'p8g4-render.capture-status.txt').read_text(encoding='utf-8-sig').splitlines()
                  if '=' in line)
    for key, expected in (('normal_finish', '1'), ('output_ok', '1'), ('allocation_failed', '0'),
                          ('indirect_work_dropped', '0')):
        if status.get(key) != expected:
            raise ValueError('Incomplete indirect-work transport: ' + key)
    epochs = {}
    with (profile/'v3-frame.csv').open(newline='', encoding='utf-8-sig') as stream:
        for row in csv.DictReader(stream):
            frame = int(row['frame']); epoch = float(row['epoch_ms'])
            if frame in epochs or not math.isfinite(epoch):
                raise ValueError('Duplicate/non-finite indirect-work frame clock')
            epochs[frame] = epoch
    context = {}
    with (profile/'benchmark-context.csv').open(newline='', encoding='utf-8-sig') as stream:
        for row in csv.DictReader(stream):
            frame = int(row['frame'])
            if frame in context:
                raise ValueError('Duplicate indirect-work context frame')
            context[frame] = all(int(row[key]) == 0 for key in ('begin_flags', 'end_flags', 'observed_flags'))
    snapshots = []
    with (profile/'p9-indirect-work.csv').open(newline='', encoding='utf-8-sig') as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames == ['snapshot_frame', *COUNTERS]:
            counters = COUNTERS
        elif reader.fieldnames == ['snapshot_frame', *EXTENDED_COUNTERS]:
            counters = EXTENDED_COUNTERS
        elif reader.fieldnames == ['snapshot_frame', *PREPARATION_EXTENDED_COUNTERS]:
            counters = PREPARATION_EXTENDED_COUNTERS
        else:
            raise ValueError('Unknown indirect-work snapshot header')
        for row in reader:
            frame = int(row['snapshot_frame']); values = tuple(int(row[key]) for key in counters)
            if any(value < 0 for value in values):
                raise ValueError('Negative indirect-work count')
            if snapshots:
                if frame <= snapshots[-1][0] or any(value < old for value, old in zip(values, snapshots[-1][1])):
                    raise ValueError('Repeated frame or reset indirect-work counters')
            snapshots.append((frame, values))
    if len(snapshots) != int(status.get('indirect_work_rows', '-1')) or not snapshots:
        raise ValueError('Missing/truncated indirect-work snapshots')
    frames = sorted(epochs)
    bad_context_prefix = [0]
    for frame in frames:
        bad_context_prefix.append(bad_context_prefix[-1] + (not context.get(frame, False)))

    def phase(frame):
        epoch = epochs.get(frame)
        if epoch is None or not windows['start'] + 1000 <= epoch < windows['finish'] - 1000:
            return None
        if not context.get(frame, False):
            return None
        return 'fixed_view' if windows.get('movement') and epoch < windows['movement'] - 1000 else 'moving'

    phases = {name: {'counts': dict.fromkeys(counters, 0), 'observation_pairs': 0,
                     'observed_epoch_start_ms': None, 'observed_epoch_end_ms': None,
                     'maximum_observation_gap_ms': 0.0} for name in ('fixed_view', 'moving')}
    excluded = 0
    for (before_frame, before), (frame, current) in zip(snapshots, snapshots[1:]):
        name = phase(frame)
        if name is None or phase(before_frame) != name:
            excluded += 1
            continue
        # A zero-state endpoint does not hide an intervening recorded loading/UI
        # frame. Missing main-frame observations are retained as uncertainty.
        first_between = bisect.bisect_right(frames, before_frame)
        after_between = bisect.bisect_left(frames, frame)
        if bad_context_prefix[after_between] != bad_context_prefix[first_between]:
            excluded += 1
            continue
        entry = phases[name]
        start, end = epochs[before_frame], epochs[frame]
        if not end > start:
            raise ValueError('Non-increasing indirect-work clock')
        if entry['observed_epoch_start_ms'] is None:
            entry['observed_epoch_start_ms'] = start
        entry['observed_epoch_end_ms'] = end
        entry['observation_pairs'] += 1
        entry['maximum_observation_gap_ms'] = max(entry['maximum_observation_gap_ms'], end - start)
        for key, value, previous in zip(counters, current, before):
            entry['counts'][key] += value - previous
    if not any(value['observation_pairs'] for value in phases.values()):
        raise ValueError('No qualified indirect-work observation intervals')
    return {'schema': 3 if counters == PREPARATION_EXTENDED_COUNTERS else 2 if counters == EXTENDED_COUNTERS else 1,
            'scope': 'cumulative_all_views_at_main_thread_observation_points',
            'counter_columns': list(counters),
            'limit': 'Same logged phase margins/context exclusions as pacing; asynchronous draw origins and earlier paging execution within a completed work item are not assigned to exact observation frames',
            'snapshots': len(snapshots), 'excluded_pairs': excluded, 'phases': phases}


def positive_counts(work, key):
    # A legacy capture cannot positively qualify a newer mechanism.
    return any(phase['counts'].get(key, 0) > 0 for phase in work['phases'].values())

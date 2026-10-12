import csv
import io
import os
from pathlib import Path
import subprocess
import sys

if len(sys.argv) not in (3, 4):
    raise SystemExit('usage: verify-compile-attribution.py preparation-exe output [progress-policy-exe]')
exe, output = sys.argv[1:3]
progress_exe = sys.argv[3] if len(sys.argv) == 4 else None
root = Path(output)
root.mkdir(parents=True, exist_ok=True)


def read_capture(path):
    return list(csv.DictReader(io.StringIO('\n'.join(
        line for line in path.read_text().splitlines() if not line.startswith('#')))))


def verify_span_bijection(rows, hitch):
    assert rows and all(None not in row and None not in row.values() for row in rows), 'schema field count mismatch'
    by_id = {row['span']: row for row in hitch}
    assert len(by_id) == len(hitch), 'duplicate retained CPU span identity'
    attribution_ids = {row['span'] for row in rows}
    assert len(attribution_ids) == len(rows), 'duplicate compile attribution span identity'
    compile_ids = {row['span'] for row in hitch if row['phase'] in ('compile.operation', 'compile.scheduler')}
    assert attribution_ids == compile_ids, (
        'compile attribution/span bijection failed: '
        f'{len(compile_ids - attribution_ids)} retained compile spans without attribution '
        f'(first {sorted(compile_ids - attribution_ids)[:5]}), '
        f'{len(attribution_ids - compile_ids)} attribution rows without compile spans')
    for row in rows:
        span = by_id[row['span']]
        assert span['frame'] == row['frame'] and row['schema'] == '1'
        assert int(row['context']) >= 0 and int(row['scheduler']) > 0
        assert float(row['actual_ms']) >= 0
        assert span['phase'] == ('compile.operation' if row['event'] == 'op' else 'compile.scheduler')
    for row in hitch:
        if row['parent'] != '0':
            parent = by_id[row['parent']]
            assert parent['thread'] == row['thread']
            assert int(parent['start_ns']) <= int(row['start_ns']) <= int(row['end_ns']) <= int(parent['end_ns'])
    return by_id


for enabled, fault, unknown in ((False, False, False), (True, False, False), (True, True, False), (True, False, True)):
    case = root / ('unknown' if unknown else 'fault' if fault else 'on' if enabled else 'off')
    case.mkdir(exist_ok=True)
    attribution = case / 'attribution.csv'
    spans = case / 'spans.csv'
    legacy = case / 'p4.csv'
    for path in (attribution, spans, legacy):
        if path.exists():
            path.unlink()
    env = os.environ.copy()
    env.update(OPENMW_P9_COMPILE_ATTRIBUTION=str(int(enabled)),
               OPENMW_P9_COMPILE_ATTRIBUTION_FILE=str(attribution),
               OPENMW_P9_HITCH_SPANS_FILE=str(spans),
               OPENMW_P4_COMPILE_FILE=str(legacy),
               OPENMW_P9_CAPTURE_TRANSPORT='1', P9_COMPILE_METADATA_FAILURE=str(int(fault)),
               P9_COMPILE_NOFRAME=str(int(unknown)))
    subprocess.run([exe], check=True, env=env, timeout=20)
    assert legacy.read_text().splitlines()[0] == (
        'frame,epoch_ms,event,kind,compile_class,queue_depth,oldest_age_frames,budget_ms,credit_ms,'
        'predicted_ms,actual_ms,headroom_ms,last_handoff_ms,objects,detail')
    if not enabled:
        assert not attribution.exists(), 'OFF created attribution output'
        continue
    rows = read_capture(attribution)
    hitch = read_capture(spans)
    by_id = verify_span_bijection(rows, hitch)
    status = dict(line.split('=', 1) for line in Path(str(spans) + '.capture-status.txt').read_text().splitlines())
    assert status['dropped'] == status['active_at_finish'] == status['allocation_failed'] == '0'
    assert status['output_ok'] == status['normal_finish'] == '1'
    for row in rows:
        span = by_id[row['span']]
        assert span['frame'] == row['frame'] and row['schema'] == '1'
        assert int(row['context']) >= 0 and int(row['scheduler']) > 0
        assert float(row['actual_ms']) >= 0
        if row['event'] != 'op':
            assert span['phase'] == 'compile.scheduler'
            continue
        assert span['phase'] == 'compile.operation'
        assert int(row['selected_set_age_frames']) <= int(row['global_oldest_age_frames'])
        assert int(row['op_age_observed_frames']) <= int(row['selected_set_age_frames'])
        assert row['tracking_status'] == 'observed' and int(row['set_generation']) > 0
        assert int(row['set']) and int(row['op'])
        if row['forcing_reason'] == 'forced_by_queue_age':
            assert int(row['selected_set_age_frames']) >= int(row['age_threshold_frames'])
        if row['forcing_reason'] == 'forced_by_hard_queue_age':
            assert int(row['selected_set_age_frames']) >= int(row['hard_age_threshold_frames'])
        if row['urgency'] == 'near_future' and row['required'] == '0':
            assert row['demand_first_observed_frame'] == ''
    stock = [row for row in rows if row['image0_filename'].startswith('attribution,image"')]
    assert len(stock) == (1 if fault else 2)
    assert len({row['image0_filename'] for row in stock}) == len(stock)
    assert all(row['image0_known'] == '1' and int(row['resource_bytes']) == 64
               and row['texture_object_before'] == '0' and int(row['texture_object_after']) > 0 for row in stock)
    assert any(row['forcing_reason'].startswith('forced_by_') for row in rows)
    assert max(int(row['metadata_allocation_failures']) for row in rows) == int(fault)
    assert all(row['format_allocation_failures'] == '0' for row in rows)
    assert all(row['registry_capacity_failures'] == row['registry_allocation_failures'] == '0' for row in rows)
    assert sum(row['frame_known'] == '0' for row in rows) == int(unknown)

if progress_exe:
    # The actual ICO fixture asserts that withdrawn/delayed bootstrap proof
    # and a no-GL budget exhausted between selection and dispatch perform no
    # operation; it also proves retained ownership permits next-frame drain.
    # Check every CPU compile span, not just rows that happened to be written.
    for enabled in (False, True):
        case = root / ('progress-on' if enabled else 'progress-off')
        case.mkdir(exist_ok=True)
        attribution, spans, legacy = (case / name for name in ('attribution.csv', 'spans.csv', 'p4.csv'))
        for path in (attribution, spans, legacy):
            if path.exists():
                path.unlink()
        env = os.environ.copy()
        env.update(OPENMW_P9_COMPILE_ATTRIBUTION=str(int(enabled)),
                   OPENMW_P9_COMPILE_ATTRIBUTION_FILE=str(attribution),
                   OPENMW_P9_HITCH_SPANS_FILE=str(spans), OPENMW_P4_COMPILE_FILE=str(legacy),
                   OPENMW_P9_CAPTURE_TRANSPORT='1')
        subprocess.run([progress_exe], check=True, env=env, timeout=20)
        if not enabled:
            assert not attribution.exists(), 'OFF progress fixture created attribution output'
            continue
        rows = read_capture(attribution)
        hitch = read_capture(spans)
        verify_span_bijection(rows, hitch)
        status = dict(line.split('=', 1) for line in Path(str(spans) + '.capture-status.txt').read_text().splitlines())
        assert status['dropped'] == status['active_at_finish'] == status['allocation_failed'] == '0'
        assert status['output_ok'] == status['normal_finish'] == '1'
        assert all(row['metadata_allocation_failures'] == row['format_allocation_failures'] == '0'
                   and row['registry_capacity_failures'] == row['registry_allocation_failures'] == '0' for row in rows)
        # The fixture intentionally removes FrameStamp once. It must report
        # unknown ownership honestly while preserving the exact span join.
        assert sum(row['frame_known'] == '0' for row in rows) == 1
        assert any(row['forcing_reason'] == 'composite_no_gl_completion' for row in rows)
        assert any(row['forcing_reason'] == 'composite_no_gl_not_ready' for row in rows)

print('PASS production GL/control pixels, front-op destruction, metadata CSV, exact span/frame links, '
      'full span/attribution bijection, queue/op/demand ages, forced progress, bounded observation history '
      'and OFF output' + ('; actual bootstrap/no-GL refusal and next-frame completion' if progress_exe else ''))

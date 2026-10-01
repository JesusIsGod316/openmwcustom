import csv
import io
import os
from pathlib import Path
import subprocess
import sys

exe, output = sys.argv[1:]
root = Path(output)
root.mkdir(parents=True, exist_ok=True)
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
    rows = list(csv.DictReader(io.StringIO('\n'.join(
        line for line in attribution.read_text().splitlines() if not line.startswith('#')))))
    assert rows and all(None not in row and None not in row.values() for row in rows), 'schema field count mismatch'
    hitch = list(csv.DictReader(io.StringIO('\n'.join(
        line for line in spans.read_text().splitlines() if not line.startswith('#')))))
    by_id = {row['span']: row for row in hitch}
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
    for row in hitch:
        if row['parent'] != '0':
            parent = by_id[row['parent']]
            assert parent['thread'] == row['thread']
            assert int(parent['start_ns']) <= int(row['start_ns']) <= int(row['end_ns']) <= int(parent['end_ns'])
print('PASS production GL/control pixels, front-op destruction, metadata CSV, exact span/frame links, '
      'queue/op/demand ages, forced progress, bounded observation history and OFF output')

#!/usr/bin/env python3
"""Check real Lua storage/serialization semantics with diagnostics off and on."""
import csv
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    executable = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2]).resolve()
    output.mkdir(parents=True, exist_ok=True)
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith(('OPENMW_HITCH_', 'OPENMW_P9_'))}
    selected = 'LuaUtilStorageTest.*:LuaSerializationTest.*'
    for level in ('normal', 'deep'):
        directory = output / level
        directory.mkdir(parents=True, exist_ok=True)
        spans = directory / 'api-spans.csv'
        capture_environment = environment.copy()
        if level == 'deep':
            capture_environment['OPENMW_P9_HITCH_SPANS_FILE'] = str(spans)
            capture_environment['OPENMW_P9_HITCH_TRANSPORT'] = 'stream'
        result = subprocess.run([str(executable), f'--gtest_filter={selected}'],
                                cwd=directory, env=capture_environment,
                                capture_output=True, text=True, timeout=45)
        (directory / 'tests.log').write_text(result.stdout + result.stderr, encoding='utf-8')
        assert result.returncode == 0, result.stdout + result.stderr
        assert '[  PASSED  ] 17 tests.' in result.stdout, 'Production Lua test set changed'
        if level == 'normal':
            assert not spans.exists(), 'Unarmed API scopes created a diagnostic file'
            continue
        status = dict(line.split('=', 1) for line in Path(str(spans) + '.capture-status.txt')
                      .read_text(encoding='utf-8').splitlines())
        for field in ('dropped', 'allocation_failed', 'active_at_finish'):
            assert status[field] == '0', status
        assert status['normal_finish'] == '1' and status['output_ok'] == '1', status
        with spans.open(encoding='utf-8', newline='') as source:
            rows = list(csv.DictReader(line for line in source if not line.startswith('#')))
        assert rows and len(rows) == int(status['rows']), 'Capture row accounting failed'
        by_id = {row['span']: row for row in rows}
        assert all(row['parent'] == '0' or row['parent'] in by_id for row in rows), 'Lost API nesting parent'
        phases = {row['phase'] for row in rows}
        # Cheap scopes still follow the recorder's existing <250us filter.
        # The large real API fixture makes these services observably expensive
        # without forcing every ordinary getter to produce a CSV row.
        required = {'lua.api.storage.get', 'lua.api.storage.get_copy', 'lua.api.storage.readonly',
                    'lua.api.storage.set', 'lua.api.storage.set_all',
                    'lua.api.storage.as_table', 'lua.api.serialize', 'lua.api.deserialize'}
        assert required <= phases, required - phases
        api_rows = [row for row in rows if row['phase'].startswith('lua.api.')]
        assert all(float(row['thread_cpu_ms']) == -1 for row in api_rows), 'API scope made an OS CPU-time query'
        assert any(row['phase'] == 'lua.api.deserialize' and row['parent'] in by_id
                   and by_id[row['parent']]['phase'] == 'lua.api.storage.get_copy' for row in rows), \
            'Storage copy no longer exposes its actual decode child'
        assert any(row['phase'] == 'lua.api.serialize' and row['parent'] in by_id
                   and by_id[row['parent']]['phase'] == 'lua.api.storage.set' for row in rows), \
            'Storage writes no longer expose their actual encode child'
    report = {'status': 'passed', 'production_tests_per_level': 17,
              'normal_capture_absent': True, 'deep_capture_complete': True,
              'storage_and_serialization_children_verified': True,
              'fast_scope_retention': 'Existing 250us filter; absence does not imply an API was not called',
              'scope': 'Real Lua API value/copy/readonly/callback/serialization semantics; no game performance claim'}
    (output / 'LUA-API-VALIDATION.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()

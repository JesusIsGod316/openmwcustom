#!/usr/bin/env python3
"""Emit an exact GL-P0 cleanup candidate without editing the source checkout.

Only explicitly reviewed literal #if 0 regions without an outer alternative
are removed. Active shadow code, platform branches and public APIs stay intact.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re

SOURCE_PATH = 'components/sceneutil/mwshadowtechnique.cpp'
SOURCE_BLOB = '381fdbea063b8620fe324bf7650d3c307b10542d'
SOURCE_COMMIT = 'be2869dd00a49774ff2642ec238d98f1456b2b8a'
ARCHIVE_TAG = 'opimizedmw-gl-p0-precleanup-20260924'
BUNDLE_SHA256 = '26b829e48bb70957cfb0889c5c77d30e54e3712535a8473320f83396abad2762'
SPANS = [(517,535),(1375,1410),(2070,2074),(2910,2916),
         (2936,2942),(2944,2950),(2952,3017),(3019,3025),
         (3046,3050),(3072,3075),(3099,3105),(3145,3149),
         (3167,3172),(3174,3206),(3211,3218),(3253,3258)]
DIRECTIVE = re.compile(r'^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)')


def blob_sha(data: bytes) -> str:
    return hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()


def transform(data: bytes) -> tuple[bytes, list[dict]]:
    if blob_sha(data) != SOURCE_BLOB:
        raise ValueError('Source blob differs from reviewed pre-cleanup archive')
    lines = data.decode('utf-8').splitlines(keepends=True)
    records = []
    last_end = 0
    for start, end in SPANS:
        if start <= last_end or end < start or end > len(lines):
            raise ValueError('Overlapping or invalid removal range')
        block = lines[start-1:end]
        if not re.fullmatch(r'\s*#if 0\s*', block[0]) or block[-1].strip() != '#endif':
            raise ValueError('Not a reviewed literal disabled block')
        depth = 0
        for index, line in enumerate(block):
            match = DIRECTIVE.match(line)
            if not match:
                continue
            kind = match.group(1)
            if kind in ('if','ifdef','ifndef'):
                depth += 1
            elif kind in ('elif','else') and depth == 1:
                raise ValueError('Outer alternative must not be removed')
            elif kind == 'endif':
                depth -= 1
                if depth == 0 and index != len(block)-1:
                    raise ValueError('Range contains live code after its endif')
            if depth < 0:
                raise ValueError('Unbalanced preprocessor range')
        if depth != 0:
            raise ValueError('Unbalanced preprocessor range')
        payload = ''.join(block).encode('utf-8')
        records.append({'original_lines': [start,end], 'lines_removed': len(block),
                        'removed_text_sha256': hashlib.sha256(payload).hexdigest(),
                        'reason': 'Literal #if 0, no outer elif/else; obsolete shadow diagnostic/experimental code'})
        last_end = end
    for start, end in reversed(SPANS):
        del lines[start-1:end]
    return ''.join(lines).encode('utf-8'), records


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    args = parser.parse_args()
    source = args.source_root / SOURCE_PATH
    candidate, entries = transform(source.read_bytes())
    if args.candidate.resolve() == source.resolve():
        raise ValueError('This tool never overwrites the source checkout')
    with args.candidate.open('xb') as output:
        output.write(candidate)
    manifest = {'project':'OpimizedMW', 'stage':'GL-P0', 'source_commit':SOURCE_COMMIT,
                'archive_tag':ARCHIVE_TAG, 'archive_bundle_sha256':BUNDLE_SHA256,
                'path':SOURCE_PATH, 'old_blob':SOURCE_BLOB, 'new_blob':blob_sha(candidate),
                'ranges':entries, 'total_lines_removed':sum(x['lines_removed'] for x in entries),
                'public_api_settings_shaders':'UNCHANGED', 'performance_claim':'NONE',
                'restoration':f'git restore --source={ARCHIVE_TAG} -- {SOURCE_PATH}',
                'related_archive_events':['evt.project.opengl_performance_ram_cleanup_plan.126'],
                'preserved_live_helper':'ObjectPaging::DebugVisitor is used by mDebugBatches and is not removed'}
    with args.manifest.open('x', encoding='utf-8') as output:
        json.dump(manifest, output, indent=2)
        output.write('\n')
    print(json.dumps({'new_blob':manifest['new_blob'], 'blocks':len(entries),
                      'lines_removed':manifest['total_lines_removed']}))

if __name__ == '__main__':
    main()

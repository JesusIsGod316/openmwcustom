"""Matched producer A/B/B/A on one installed VulkanMW executable.

Default mode changes only the dirty-queue switch. --supported-continuous keeps
queues enabled in both arms and changes only supported actor/particle producer
classification plus the clean intrinsic-particle body reuse path. Each run has
fresh isolated settings and user storage, preserves content order, and exits
naturally. Normal saves are not loaded.
"""
from datetime import datetime
import argparse
import importlib.util
import json
from pathlib import Path
import re
import uuid
import zipfile

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('producer_cohort', HERE / 'run-publication-cohort.py')
cohort = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cohort)
BASE = 'groups+transactions+inventories+actors+admission+change-driven'
CANDIDATE = BASE + '+queues'
# Split-particle capture must be identical in both supported-producer arms.
# Otherwise the candidate cannot ever exercise the supported particle lane and
# the comparison conflates feature availability with scheduling ownership.
SUPPORTED_BASE = CANDIDATE + '+particles'
SUPPORTED_CANDIDATE = SUPPORTED_BASE + '+supported-continuous'
GPU_TABLE_BASE = SUPPORTED_CANDIDATE
GPU_TABLE_CANDIDATE = GPU_TABLE_BASE + '+gpu-tables'
GPU_CULL_BASE = GPU_TABLE_CANDIDATE + '+gpu-cull-queue'
GPU_CULL_CANDIDATE = GPU_CULL_BASE + '+gpu-cull-indirect'


def source_identity(package):
    """Use package provenance; never infer source identity from a filename."""
    executable_hash = cohort.profile.dc.digest(package / 'openmw.exe')
    manifest = package / 'source-changes.json'
    if manifest.is_file():
        source = json.loads(manifest.read_text(encoding='utf-8-sig'))
        if source.get('executable_sha256', '').lower() != executable_hash.lower():
            raise ValueError('Package executable no longer matches source-changes.json')
        return source
    labels = ('VULKANMW-PRODUCER-TEST.txt', 'VULKANMW-PHASE3C-TEST.txt')
    for label in labels:
        path = package / label
        if not path.is_file():
            continue
        fields = dict(line.split('=', 1) for line in path.read_text(encoding='utf-8-sig').splitlines() if '=' in line)
        source_head = fields.get('source_head', fields.get('source_phase3c', ''))
        if re.fullmatch(r'[0-9a-fA-F]{40}', source_head):
            source = {'source_head': source_head, 'executable_sha256': executable_hash,
                      'source_identity_basis': label, 'changed_source_files': {}}
            manifest.write_text(json.dumps(source, indent=2), encoding='utf-8')
            return source
    raise ValueError('Package lacks source provenance; preserve the build identity file beside openmw.exe')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, default=HERE.parents[1])
    parser.add_argument('--user-config', type=Path, default=cohort.profile.dc.normal_default())
    parser.add_argument('--single', choices=('candidate', 'control'), help='One smoke run, not a performance verdict')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--supported-continuous', action='store_true',
                      help='A/B supported actor/particle producers with dirty queues enabled in both arms')
    mode.add_argument('--gpu-scene-tables', action='store_true',
                      help='A/B P2 direct-delta GPU scene tables with repaired P1 producer stack in both arms')
    mode.add_argument('--gpu-cull-indirect', action='store_true',
                      help='A/B P3 GPU population cull + indirect draw with the complete P2 stack in both arms')
    args = parser.parse_args()
    package = args.package.resolve()
    source = source_identity(package)
    if args.gpu_cull_indirect:
        baseline, candidate = GPU_CULL_BASE, GPU_CULL_CANDIDATE
    elif args.gpu_scene_tables:
        baseline, candidate = GPU_TABLE_BASE, GPU_TABLE_CANDIDATE
    elif args.supported_continuous:
        baseline, candidate = SUPPORTED_BASE, SUPPORTED_CANDIDATE
    else:
        baseline, candidate = BASE, CANDIDATE
    sequence = [('control', baseline), ('candidate', candidate), ('candidate', candidate), ('control', baseline)]
    if args.single:
        sequence = [(args.single, candidate if args.single == 'candidate' else baseline)]
    prefix = 'producer-' + datetime.now().strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:6]
    evidence = []
    try:
        for index, (label, arm) in enumerate(sequence):
            name = f'{prefix}-{index}-{label}'
            evidence.append(package / 'Benchmarks' / name)
            cohort.run(package, args.user_config.resolve(), source, name, arm)
    finally:
        present = [path for path in evidence if path.is_dir()]
        if present:
            target = package / 'Benchmarks' / (prefix + '.zip')
            with zipfile.ZipFile(target, 'x', compression=zipfile.ZIP_DEFLATED) as output:
                for directory in present:
                    for path in sorted(directory.rglob('*')):
                        if path.is_file():
                            output.write(path, path.relative_to(directory.parent))
            print('BENCHMARK ZIP:', target, flush=True)


if __name__ == '__main__':
    main()

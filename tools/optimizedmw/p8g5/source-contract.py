#!/usr/bin/env python3
"""Reconstruction guard; no claim of full DLSS integration."""
from pathlib import Path
root = Path(__file__).resolve().parents[3]
for path in (root / '.github/workflows').glob('*'):
    assert 'p8g5' not in path.name.lower(), 'no build workflow before complete integration'
assert not list((root / 'tools/optimizedmw/p8g5').glob('*.bat')), 'keep one existing launcher'
frame = (root / 'components/rendercore/temporalframe.hpp').read_text()
contract = (root / 'components/rendercore/temporalinputcontract.hpp').read_text()
for token in ('bool commit(', 'bool abort(', 'mPending', 'InvalidPreviousInput', 'clipToPreviousClip', 'jitterPixels'):
    assert token in frame, token
assert 'bool dynamicMotionComplete = false' in contract
assert 'InputStatus::IncompleteMotion' in contract
assert 'temporal upscaling inputs' not in (root / 'files/settings-default.cfg').read_text(), 'do not expose unintegrated input mode'
print('PASS: explicit temporal transactions and incomplete-motion rejection; no new workflow/BAT/game mode')

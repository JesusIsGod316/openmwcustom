#!/usr/bin/env python3
"""Integration guards complement, never replace, compile and pixel tests."""
from pathlib import Path
ROOT = Path(__file__).resolve().parents[3]
def require(path, *tokens):
    text = (ROOT / path).read_text(encoding='utf-8')
    for token in tokens:
        if token not in text:
            raise SystemExit(f'{path}: rendering integration contract missing {token!r}')
require('apps/openmw/mwrender/v4engine-sources.cmake', 'v4previewbridge.cpp', 'v4skycapture.cpp')
require('components/render/backend/vsg/runtime-sources.cmake', 'nativesky.cpp')
require('apps/openmw/mwrender/v4previewbridge.cpp',
        'prepareNativePreviewFrame', 'nativePreviewFramePresented', 'prepareNativeSkyFrame',
        'OPENMW_V4_LEGACY_PREVIEW_CONTROL', 'OPENMW_V4_LEGACY_SKY_CONTROL',
        'TRAVERSE_ACTIVE_CHILDREN', 'preview.root->accept(visitor)', 'entry.renderedRevision && !entry.published',
        'host.retireAuxiliarySurface', 'view.isolatedScene = std::move(scene)')
require('apps/openmw/mwrender/v4engineframecoordinator.cpp',
        'prepareNativePreviewFrame', 'prepareNativeSkyFrame')
require('apps/openmw/mwrender/v4localmapbridge.cpp', 'nativePreviewFramePresented(source)', 'input.nativeSky = source.nativeSky', 'input.auxiliaryViews = source.previewViews')
require('components/render/backend/vsg/vsgruntimehost.cpp',
        'realizeIsolatedScene', 'prepareSky(mNativeSky', 'mNativeSky.markSubmitted',
        'if (frame.nativeSky()) backdropEnvironment.skyEnabled = false;')
require('components/render/backend/vsg/uipipeline.cpp',
        'VK_BLEND_FACTOR_ONE', 'sampling.flags.x > 0.5 ? 1.0 : sampled.a',
        'imageLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL')
require('components/render/backend/vsg/legacymaterialshader.cpp',
        'OPENMW_V4_LEGACY_NORMAL_MAPPING_CONTROL')
require('components/render/backend/vsg/watersurface.cpp', 'OPENMW_V4_LEGACY_WATER_OPTICS_CONTROL')
require('apps/openmw/CMakeLists.txt', 'openmw-v4-rendering-capture-tests',
        'openmw-v4-rendering-uniform-tests', 'openmw-v4-rendering-pixel-tests', 'RENDERING-REPAIR.md')
for path in ('legacymaterialshader.hpp', 'legacybumpmaterialshader.hpp', 'enchantedmaterialshader.hpp'):
    text = (ROOT / 'components/render/backend/vsg' / path).read_text()
    if 'alignas(16)' in text:
        raise SystemExit(f'{path}: unsupported VSG Value host over-alignment returned')
    if 'offsetof' not in text or 'is_standard_layout' not in text:
        raise SystemExit(f'{path}: explicit GPU upload-layout proof missing')
print('Rendering integration guards: PASS; full compile and pixel/gameplay gates remain required')

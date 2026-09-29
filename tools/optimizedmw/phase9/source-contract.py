#!/usr/bin/env python3
"""Focused Phase 9 source/staging guards, not runtime or performance proof."""
from pathlib import Path
import argparse
import hashlib
import json


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument('--staged-shaders', type=Path)
    args = parser.parse_args()
    root = args.root
    def read(path: str) -> str:
        return (root / path).read_text(encoding='utf-8')
    def need(condition: bool, message: str) -> None:
        if not condition:
            raise SystemExit('Phase 9 source contract FAILED: ' + message)

    stream = read('components/sceneutil/dynamicstream.hpp')
    need('OPENMW_P9_DYNAMIC_STREAM' in stream and 'std::strcmp(v, "1") == 0' in stream,
         'dynamic stream must remain explicit opt-in')
    for token in ('profile._size', 'data->getModifiedCount() == 0xffffffu', 'BudgetFallback',
                  'getNumBufferData() != mCount', 'glBuffer->compileBuffer()', 'CatalogCapacity = 65536'):
        need(token in stream, 'missing stream ownership/budget guard: ' + token)
    for path, kind in (('riggeometry.cpp', 'rig_geometry'), ('morphgeometry.cpp', 'morph_geometry')):
        code = read('components/sceneutil/' + path)
        install = 'DynamicStream::install(to, vbo, "' + kind + '")'
        need(install in code, 'missing production installation on the actual Geometry reference')
        need(code.index(install) < code.index('Debug::P8DynamicDrawTelemetry::install'),
             'old outer draw callback must include the stream experiment')

    trace = read('components/sceneutil/drawphasetrace.hpp')
    engine = read('apps/openmw/engine.cpp')
    need(engine.index('DrawPhaseTrace::installBeforeRealize(*mViewer)') < engine.index('mViewer->realize();'),
         'root-cause tracer must install before realize can start rendering threads')
    need('DrawPhaseTrace::install(*mViewer)' not in read('apps/openmw/mwrender/postprocessor.cpp'),
         'late PostProcessor trace installation is forbidden')
    for token in ('valid_leaf_capture=', '.frames.csv', '.renderer.csv', 'installBeforeRealize', 'GLCallTrace::Scope'):
        need(token in trace, 'missing root-cause coverage or scope: ' + token)
    for path in ('riggeometry.cpp', 'morphgeometry.cpp'):
        production = read('components/sceneutil/' + path)
        need('StaticGeometryPrewarm::prepare' not in production,
             'crash-correlated static actor prewarm must not remain wired to production')
        need('staticgeometryprewarm.hpp' not in production,
             'production actor/morph source must not include the rejected prewarm helper')
    prewarm = read('components/sceneutil/staticgeometryprewarm.hpp')
    need('OPENMW_P9_STATIC_PREWARM' in prewarm,
         'rejected helper may remain as isolated historical source but must stay explicit opt-in')
    need('viewer.areThreadsRunning()' in trace and '"3.6.5"' in trace, 'trace installation guard missing')
    need('typeid(*cv) != typeid(osgUtil::CullVisitor)' in trace, 'unknown cull visitor must remain unchanged')
    need('osgUtil::RenderLeaf::render(info, previous)' in trace, 'stock trace-off leaf fallback missing')
    leaf = trace.split('class Leaf final', 1)[1].split('class CullVisitor final', 1)[0]
    need(leaf.index('_drawable->getName()') < leaf.index('state.decrementDynamicObjectCount()'),
         'metadata cannot be read after the dynamic safe point releases update')
    need('capture.append(row);' in leaf, 'post-safe-point record must own its metadata')

    temporal = read('apps/openmw/mwrender/temporalmotion.cpp')
    need('input.jitterEnabled = false;' in temporal, 'do not jitter gameplay without a reconstruction consumer')
    need('PendingTicket' in temporal and 'history.abort(ticket)' in temporal, 'uncommitted history must abort')
    need('depth == c.motion.get()' in temporal, 'motion output/depth input alias must be rejected')
    need('RestoreDrawState' in temporal and 'GL_READ_FRAMEBUFFER_BINDING' in temporal,
         'independent framebuffer/read/viewport restoration missing')
    need('c.status.denseDynamicMotion = false;' in temporal,
         'camera/static submission must not advertise dense dynamic motion')
    need('consumerFrame(unsigned context)' in read('apps/openmw/mwrender/temporalmotion.hpp')
         and 'RenderCore::Temporal::rowMajor(frame->currentViewProjection)' in temporal,
         'consumer-ready temporal matrix contract missing')
    # GPU timing lives at the canvas seam so the standalone temporal math/render
    # target does not inherit the full legacy diagnostics header graph.
    for forbidden in ('glFinish(', 'glClientWaitSync(', 'glReadPixels(', 'glGetTexImage('):
        need(forbidden not in stream + temporal + trace, 'production path contains a wait/readback: ' + forbidden)

    post = read('apps/openmw/mwrender/postprocessor.cpp')
    need('camera.projection = cv->getProjectionMatrix()' in post, 'must retain cull-owned final projection')
    need('camera.cameraEpoch = mRendering.getCamera()->temporalEpoch()' in post, 'camera identity reset missing')
    need('catch (const std::exception& error)' in post, 'shader setup must preserve normal fallback')
    need('mPostProcessor->captureTemporalCamera(cv);' in read('apps/openmw/mwrender/pingpongcull.cpp'),
         'real camera capture hook missing')
    canvas = read('apps/openmw/mwrender/pingpongcanvas.cpp')
    need('mTemporalMotion->render(renderInfo, mTemporalCamera, depth, *this)' in canvas,
         'real presentation hook missing')
    need('setDataVariance(osg::Object::DYNAMIC)' in canvas, 'temporal frame slot must retain CPU ownership barrier')
    need('TemporalMotion::debugView()' in canvas, 'normal color must not be replaced by the debug view')
    need('p9TemporalInputWriter()' in canvas and 'requiredForDlss' in canvas
         and 'status.denseDynamicMotion' in canvas,
         'DLSS input-readiness telemetry must fail closed until dense motion exists')
    temporal_test = read('tools/optimizedmw/phase9/temporal-render-tests.cpp')
    need('consumer-ready matrix/motion contract' in temporal_test
         and 'consumerFrame(state->getContextID())' in temporal_test,
         'consumer-frame runtime regression coverage missing')
    need('Debug::V36GpuProfiler::ScopedPass' in canvas and '"temporal/camera_motion"' in canvas,
         'nonblocking GPU timing for temporal input pass missing')
    need('luminancecalculator pingpongcanvas nisscaler temporalmotion' in read('apps/openmw/CMakeLists.txt'),
         'new production implementation is not in the game target')

    composite_h = read('components/terrain/compositemaprenderer.hpp')
    composite_cpp = read('components/terrain/compositemaprenderer.cpp')
    terrain_world = read('components/terrain/world.cpp')
    need('return "CompositeMapRenderer"' in composite_h
         and 'setName("TerrainCompositeMapRenderer")' in composite_cpp
         and 'setName("TerrainCompositeMapCamera")' in terrain_world,
         'persistent terrain composite drawable/camera identity is missing')
    need('p9CompositeWriter()' in composite_cpp and 'telemetry->stateMs' in composite_cpp
         and 'telemetry->drawMs' in composite_cpp,
         'terrain composite state/draw attribution missing')
    gltrace = read('components/sceneutil/glcalltrace.hpp')
    need('std::array<std::uint64_t, 8> args' in gltrace and 'mBreadcrumbs' in gltrace
         and 'breadcrumb(unsigned context)' in gltrace and '.gl-last.csv' in gltrace,
         'extended GL arguments/crash breadcrumb contract missing')
    gltest = read('tools/optimizedmw/phase9/gl-hook-contract-tests.cpp')
    need('active crash breadcrumb' in gltest and '8388608' in gltest and 'CompressedTexImage2D' in gltest,
         'active breadcrumb/texture-upload argument regression coverage missing')
    for token in ('nodePathHash', 'ownerName', 'cameraBucket', 'texture0Bytes', 'camera_bucket='):
        need(token in trace, 'draw identity/camera/resource telemetry missing: ' + token)

    launcher = read('tools/optimizedmw/phase9/OptimizedMW_Test.ps1')
    for token in ('Get-P9Mode', 'OPENMW_P9_DYNAMIC_STREAM=$mode.Stream',
                  'OPENMW_P9_TEMPORAL_INPUTS=$mode.Temporal', 'OPENMW_P9_MOTION_VIEW=$mode.View',
                  'OPENMW_P9_LEAF_TRACE_FILE', 'OPENMW_P9_DYNAMIC_TRACE_FILE',
                  'OPENMW_P9_TEMPORAL_FILE', 'OPENMW_P9_COMPOSITE_FILE', 'OPENMW_V36_GPU_PASS_FILE',
                  'phase9_dense_dynamic_motion=false', 'phase9_scene_jitter=false',
                  'phase9_static_prewarm=disabled_after_optimized_trace_driver_crash',
                  'phase9_temporal_contract=consumer_frame_v1',
                  'Test-Phase9TraceCapture -ProfileDir'):
        need(token in launcher, 'missing actual launcher control/provenance: ' + token)
    need('$mode.Prewarm' not in launcher and 'OPENMW_P9_STATIC_PREWARM=$mode.Prewarm' not in launcher,
         'rejected prewarm must not be selectable from the shipped launcher')
    need(launcher.index('settings_restore_verified=$restoreVerified') < launcher.index('Complete-Phase9Profile -ProfileDir'),
         'restore settings before packaging')
    archive = read('tools/optimizedmw/phase9/OptimizedMW_ProfileArchive.ps1')
    complete = archive.split('function Complete-Phase9Profile', 1)[1]
    need(complete.index('New-VerifiedProfileZip -SourceDir') < complete.index('Invoke-BoundedOfflineReport -ReportScript'),
         'a report must never block creation of the raw evidence archive')
    for token in ('ComputeHash($stream)', 'missing_expected_files=$missing',
                  '$report.WaitForExit($TimeoutSeconds*1000)',
                  '[IO.File]::Replace($temp,$destination,[System.Management.Automation.Language.NullString]::Value)',
                  '[IO.Compression.ZipFileExtensions]::CreateEntryFromFile'):
        # Behavioral ZIP/timeout/replacement fixtures run on both real PS hosts.
        need(token in archive, 'archive reliability guard missing: ' + token)
    installation = read('CMakeLists.txt')
    need('tools/optimizedmw/phase9/OptimizedMW_Test.ps1' in installation
         and 'tools/optimizedmw/phase9/OptimizedMW_ProfileArchive.ps1' in installation,
         'the actual installed launcher must expose Phase 9, not old P8U1 modes')
    need('P9_REQUIRE_GAME_SDK' in read('tools/optimizedmw/phase9/CMakeLists.txt'),
         'production integration compile gate is missing')

    cmake = read('files/shaders/CMakeLists.txt')
    names = ('temporal_camera_motion.vert', 'temporal_camera_motion.frag',
             'temporal_motion_view.vert', 'temporal_motion_view.frag')
    for name in names:
        relative = 'compatibility/' + name
        need(relative in cmake, 'shader absent from actual staging list: ' + relative)
        source = root / 'files/shaders' / relative
        need(source.is_file(), 'missing shader source: ' + relative)
        if args.staged_shaders:
            staged = args.staged_shaders / relative
            need(staged.is_file() and source.read_bytes() == staged.read_bytes(),
                 'deployed shader differs from tested source: ' + relative)
    culling = json.loads(read('tools/optimizedmw/phase9/culling-preserved.json'))
    for path, digest in culling['sha256'].items():
        need(hashlib.sha256((root/path).read_bytes()).hexdigest() == digest,
             'this root-cause checkpoint must preserve audited culling: '+path)
    print(json.dumps({'source_contract': 'PASS', 'deployed_temporal_shaders':
                      'PASS' if args.staged_shaders else 'NOT_CHECKED',
                      'dlss_runtime': 'NOT_IMPLEMENTED', 'performance': 'NOT_MEASURED'}))


if __name__ == '__main__':
    main()

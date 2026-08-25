if (NOT DEFINED OPENMW_RESOURCES_ROOT)
    return()
endif()

# Copy resource files into the build directory
set(SDIR ${CMAKE_CURRENT_SOURCE_DIR})
set(DDIRRELATIVE resources/shaders)

set(SHADER_FILES
    lib/water/fresnel.glsl
    lib/water/rain_ripples.glsl
    lib/water/ripples.glsl
    lib/view/depth.glsl
    lib/luminance/constants.glsl
    lib/particle/soft.glsl
    lib/particle/occlusion.glsl
    lib/util/quickstep.glsl
    lib/util/coordinates.glsl
    lib/util/distortion.glsl
    lib/core/fragment.glsl
    lib/core/fragment.h.glsl
    lib/core/fragment_multiview.glsl
    lib/core/lighting_fragment.glsl
    lib/core/lighting_vertex.glsl
    lib/core/lighting_fragment_legacy.glsl
    lib/core/lighting_vertex_legacy.glsl
    lib/core/lighting_fragment_impl.glsl
    lib/core/lighting_vertex_impl.glsl
    lib/core/vertex.glsl
    lib/core/vertex.h.glsl
    lib/core/vertex_multiview.glsl
    lib/light/bindings.glsl
    lib/light/bindings-legacy.glsl
    lib/light/clamp.glsl
    lib/light/util.glsl
    lib/light/struct.glsl
    lib/sky/passes.glsl
    lib/material/parallax.glsl
    lib/material/alpha.glsl
    lib/material/struct.glsl
    lib/material/vertexcolors.glsl
    lib/material/colormodes.glsl
    compatibility/ripples_blobber.frag
    compatibility/ripples_simulate.frag
    compatibility/fog.glsl
    compatibility/groundcover.vert
    compatibility/groundcover.frag
    compatibility/globalmap.vert
    compatibility/globalmap.frag
    compatibility/water.vert
    compatibility/water.frag
    compatibility/objects.vert
    compatibility/objects.frag
    compatibility/terrain.vert
    compatibility/terrain.frag
    compatibility/terrain_composite.vert
    compatibility/terrain_composite.frag
    compatibility/shadows_vertex.glsl
    compatibility/shadows_fragment.glsl
    compatibility/shadowcasting.vert
    compatibility/shadowcasting.frag
    compatibility/normals.glsl
    compatibility/multiview_resolve.vert
    compatibility/multiview_resolve.frag
    compatibility/outline.frag
    compatibility/outline.vert
    compatibility/depthclipped.vert
    compatibility/depthclipped.frag
    compatibility/gui.vert
    compatibility/gui.frag
    compatibility/debug.vert
    compatibility/debug.frag
    compatibility/sky.vert
    compatibility/sky.frag
    compatibility/fullscreen_tri.vert
    compatibility/fullscreen_tri.frag
    compatibility/bs/default.vert
    compatibility/bs/default.frag
    compatibility/bs/nolighting.vert
    compatibility/bs/nolighting.frag
    compatibility/luminance/resolve.frag
    compatibility/luminance/luminance.frag
    core/ripples_blobber.comp
    core/ripples_simulate.comp
    core/gui.frag
    core/gui.vert
    core/lighting/cluster.comp
    core/lighting/cull.comp
)

copy_all_resource_files(${CMAKE_CURRENT_SOURCE_DIR} ${OPENMW_RESOURCES_ROOT} ${DDIRRELATIVE} "${SHADER_FILES}")

# V3: apply the exact Rafael Enhanced PBR/OpenMW 0.52 shader overlay used by the
# known-good CustomV2 installation. Keeping this as a deterministic archive lets
# us verify that future builds receive the same resource payload byte-for-byte.
set(V3_RAF_PBR_PARTS
    part00.b64
    part01.b64
    part02.b64
    part03.b64
    part04.b64
    part05.b64
    part06.b64
    part07.b64
)
set(V3_RAF_PBR_PART_LENGTHS
    8000
    8000
    8000
    8000
    8000
    8000
    8000
    4992
)

# Each chunk has a fixed expected payload length. Truncate any accidental bytes
# appended by transport, while still rejecting truncated chunks. The SHA-256 of
# the decoded archive below remains the authoritative integrity check, so any
# modification inside the real payload still fails configuration.
set(_v3_raf_pbr_base64 "")
list(LENGTH V3_RAF_PBR_PARTS _v3_raf_pbr_part_count)
math(EXPR _v3_raf_pbr_last_part "${_v3_raf_pbr_part_count} - 1")
foreach(_v3_raf_pbr_index RANGE 0 ${_v3_raf_pbr_last_part})
    list(GET V3_RAF_PBR_PARTS ${_v3_raf_pbr_index} _v3_raf_pbr_part)
    list(GET V3_RAF_PBR_PART_LENGTHS ${_v3_raf_pbr_index} _v3_raf_pbr_expected_length)
    set(_v3_raf_pbr_part_path "${CMAKE_CURRENT_SOURCE_DIR}/v3overlay/${_v3_raf_pbr_part}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_v3_raf_pbr_part_path}")
    file(READ "${_v3_raf_pbr_part_path}" _v3_raf_pbr_part_data)
    string(LENGTH "${_v3_raf_pbr_part_data}" _v3_raf_pbr_actual_length)
    if (_v3_raf_pbr_actual_length LESS _v3_raf_pbr_expected_length)
        message(FATAL_ERROR
            "V3 Rafael PBR overlay chunk ${_v3_raf_pbr_part} is truncated: "
            "expected at least ${_v3_raf_pbr_expected_length} bytes, got ${_v3_raf_pbr_actual_length}")
    endif()
    string(SUBSTRING "${_v3_raf_pbr_part_data}" 0 ${_v3_raf_pbr_expected_length} _v3_raf_pbr_part_payload)
    string(APPEND _v3_raf_pbr_base64 "${_v3_raf_pbr_part_payload}")
endforeach()

set(_v3_raf_pbr_b64 "${CMAKE_CURRENT_BINARY_DIR}/v3-rafael-pbr-0.52-overlay.b64")
set(_v3_raf_pbr_zip "${CMAKE_CURRENT_BINARY_DIR}/v3-rafael-pbr-0.52-overlay.zip")
file(WRITE "${_v3_raf_pbr_b64}" "${_v3_raf_pbr_base64}")

if (WIN32)
    find_program(V3_RAF_POWERSHELL NAMES pwsh powershell)
    if (NOT V3_RAF_POWERSHELL)
        message(FATAL_ERROR "V3 Rafael PBR overlay requires PowerShell to decode the embedded archive")
    endif()

    file(TO_NATIVE_PATH "${_v3_raf_pbr_b64}" _v3_raf_pbr_b64_native)
    file(TO_NATIVE_PATH "${_v3_raf_pbr_zip}" _v3_raf_pbr_zip_native)
    execute_process(
        COMMAND "${V3_RAF_POWERSHELL}" -NoProfile -NonInteractive -Command
            "[IO.File]::WriteAllBytes('${_v3_raf_pbr_zip_native}', [Convert]::FromBase64String([IO.File]::ReadAllText('${_v3_raf_pbr_b64_native}')))"
        RESULT_VARIABLE _v3_raf_pbr_decode_result
    )
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -c
            "import base64,pathlib; pathlib.Path(r'${_v3_raf_pbr_zip}').write_bytes(base64.b64decode(pathlib.Path(r'${_v3_raf_pbr_b64}').read_text()))"
        RESULT_VARIABLE _v3_raf_pbr_decode_result
    )
endif()

if (NOT _v3_raf_pbr_decode_result EQUAL 0)
    message(FATAL_ERROR "Failed to decode the V3 Rafael PBR shader overlay")
endif()

file(SHA256 "${_v3_raf_pbr_zip}" _v3_raf_pbr_sha256)
if (NOT _v3_raf_pbr_sha256 STREQUAL "6f42a686e2a6a9038bbd4a9e2d0d1be6d8812b9b3e681d8b569560c2fe110255")
    message(FATAL_ERROR "V3 Rafael PBR shader overlay checksum mismatch: ${_v3_raf_pbr_sha256}")
endif()

file(ARCHIVE_EXTRACT
    INPUT "${_v3_raf_pbr_zip}"
    DESTINATION "${OPENMW_RESOURCES_ROOT}/${DDIRRELATIVE}"
)

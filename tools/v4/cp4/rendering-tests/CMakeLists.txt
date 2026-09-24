cmake_minimum_required(VERSION 3.20)
project(OpenMWRenderingRepairTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(OPENMW_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(CP4F_RENDER_SDK "" CACHE PATH "Optional verified offline SDK root (prefix/sysroot/mygui)")
set(CP4F_OSG_ROOT "" CACHE PATH "Optional OSG installation prefix")
set(CP4F_MYGUI_INCLUDE_DIR "" CACHE PATH "MyGUIEngine include directory")
option(CP4F_CAPTURE_SANITIZERS "Instrument the CPU capture fixture with ASan and UBSan" OFF)
find_package(Threads REQUIRED)
add_library(rendering-deps INTERFACE)
if(CP4F_RENDER_SDK)
    if(NOT UNIX OR APPLE)
        message(FATAL_ERROR "The offline SDK is Linux x86-64 only; use native package configs instead")
    endif()
    set(L "${CP4F_RENDER_SDK}/sysroot/usr/lib/x86_64-linux-gnu")
    target_include_directories(rendering-deps SYSTEM INTERFACE "${CP4F_RENDER_SDK}/prefix/include" "${CP4F_RENDER_SDK}/sysroot/usr/include")
    target_link_libraries(rendering-deps INTERFACE
        "${CP4F_RENDER_SDK}/prefix/lib/libvsgXchange.a" "${CP4F_RENDER_SDK}/prefix/lib/libvsg.a"
        -Wl,--start-group "${L}/libglslang.a" "${L}/libglslang-default-resource-limits.a"
        "${L}/libSPIRV.a" "${L}/libMachineIndependent.a" "${L}/libOSDependent.a"
        "${L}/libGenericCodeGen.a" "${L}/libSPIRV-Tools-opt.a" "${L}/libSPIRV-Tools.a" -Wl,--end-group)
    find_library(CP4F_VULKAN_LIBRARY NAMES vulkan libvulkan.so.1 REQUIRED)
    target_link_libraries(rendering-deps INTERFACE "${CP4F_VULKAN_LIBRARY}")
    set(CP4F_MYGUI_INCLUDE_DIR "${CP4F_RENDER_SDK}/mygui/MyGUIEngine/include")
else()
    find_package(vsg 1.1.15 CONFIG REQUIRED)
    find_package(vsgXchange CONFIG REQUIRED)
    target_link_libraries(rendering-deps INTERFACE vsg::vsg vsgXchange::vsgXchange)
endif()
if(CP4F_OSG_ROOT)
    list(PREPEND CMAKE_PREFIX_PATH "${CP4F_OSG_ROOT}")
endif()
find_package(OpenSceneGraph 3.6 REQUIRED COMPONENTS osg osgDB osgUtil osgParticle)
find_path(CP4F_GLM_INCLUDE_DIR glm/glm.hpp HINTS "${CP4F_RENDER_SDK}/sysroot/usr/include" "${CP4F_OSG_ROOT}/include" REQUIRED)
if(NOT EXISTS "${CP4F_MYGUI_INCLUDE_DIR}/MyGUI.h")
    message(FATAL_ERROR "Set CP4F_MYGUI_INCLUDE_DIR to the pinned MyGUIEngine/include directory")
endif()
set(CP4F_VIEW_STATE_SOURCE "${OPENMW_ROOT}/components/render/backend/vsg/openmwviewdependentstate.cpp"
    CACHE FILEPATH "View state implementation; original source may be supplied for an expected-failure regression")
add_library(rendering-source STATIC
    "${OPENMW_ROOT}/components/debug/debuglog.cpp"
    "${OPENMW_ROOT}/components/files/conversion.cpp"
    "${OPENMW_ROOT}/components/render/backend/vsg/legacymaterialshader.cpp"
    "${CP4F_VIEW_STATE_SOURCE}"
    "${OPENMW_ROOT}/components/render/backend/vsg/offscreenrendertarget.cpp"
    "${OPENMW_ROOT}/components/render/backend/vsg/nativepostprocess.cpp"
    "${OPENMW_ROOT}/components/render/backend/vsg/staticassetconformance.cpp"
    "${OPENMW_ROOT}/components/render/backend/vsg/staticassetrealizer.cpp"
    "${OPENMW_ROOT}/components/render/backend/vsg/statictexturedecode.cpp"
    "${OPENMW_ROOT}/components/render/backend/vsg/uipipeline.cpp"
    "${OPENMW_ROOT}/components/render/backend/vsg/watersurface.cpp"
    "${OPENMW_ROOT}/components/render/backend/vsg/skybackdrop.cpp"
    "${OPENMW_ROOT}/components/render/backend/vsg/nativesky.cpp")
target_include_directories(rendering-source PUBLIC "${OPENMW_ROOT}" "${CP4F_GLM_INCLUDE_DIR}"
    PRIVATE "${OPENMW_ROOT}/components/render/backend/vsg")
target_link_libraries(rendering-source PUBLIC rendering-deps Threads::Threads ${CMAKE_DL_LIBS})
add_executable(rendering-pixels "${OPENMW_ROOT}/tools/v4/cp4/rendering-pixel-tests.cpp")
target_link_libraries(rendering-pixels PRIVATE rendering-source)
target_sources(rendering-pixels PRIVATE
    "${OPENMW_ROOT}/tools/v4/cp4/authored-material-fixture.cpp"
    "${OPENMW_ROOT}/components/vfs/manager.cpp"
    "${OPENMW_ROOT}/components/misc/resourcehelpers.cpp"
    "${OPENMW_ROOT}/components/files/hash.cpp"
    "${OPENMW_ROOT}/extern/smhasher/MurmurHash3.cpp"
    "${OPENMW_ROOT}/components/sceneutil/material.cpp")
target_include_directories(rendering-pixels PRIVATE "${OPENMW_ROOT}/extern"
    "${OPENMW_ROOT}/extern/smhasher/include" "${OPENMW_ROOT}/extern/smhasher/include/smhasher")
target_include_directories(rendering-pixels SYSTEM PRIVATE "${CP4F_MYGUI_INCLUDE_DIR}" ${OPENSCENEGRAPH_INCLUDE_DIRS})
target_link_libraries(rendering-pixels PRIVATE ${OPENSCENEGRAPH_LIBRARIES})
add_executable(rendering-capture
    "${OPENMW_ROOT}/tools/v4/cp4/rendering-capture-tests.cpp"
    "${OPENMW_ROOT}/components/nifosg/matrixtransform.cpp"
    "${OPENMW_ROOT}/components/sceneutil/riggeometry.cpp"
    "${OPENMW_ROOT}/components/sceneutil/morphgeometry.cpp"
    "${OPENMW_ROOT}/components/sceneutil/skeleton.cpp"
    "${OPENMW_ROOT}/components/sceneutil/positionattitudetransform.cpp"
    "${OPENMW_ROOT}/apps/openmw/mwrender/v4skycapture.cpp"
    "${OPENMW_ROOT}/components/debug/debuglog.cpp"
    "${OPENMW_ROOT}/components/files/conversion.cpp"
    "${OPENMW_ROOT}/components/vfs/manager.cpp"
    "${OPENMW_ROOT}/components/misc/resourcehelpers.cpp"
    "${OPENMW_ROOT}/components/files/hash.cpp"
    "${OPENMW_ROOT}/extern/smhasher/MurmurHash3.cpp"
    "${OPENMW_ROOT}/components/sceneutil/material.cpp")
target_include_directories(rendering-capture PRIVATE "${OPENMW_ROOT}" "${OPENMW_ROOT}/extern"
    "${OPENMW_ROOT}/extern/smhasher/include" "${OPENMW_ROOT}/extern/smhasher/include/smhasher")
target_include_directories(rendering-capture SYSTEM PRIVATE "${CP4F_GLM_INCLUDE_DIR}" "${CP4F_MYGUI_INCLUDE_DIR}" ${OPENSCENEGRAPH_INCLUDE_DIRS})
target_link_libraries(rendering-capture PRIVATE ${OPENSCENEGRAPH_LIBRARIES} Threads::Threads)
if(MSVC)
    # These fixtures intentionally link only the exercised engine functions,
    # just as the ELF build below uses function sections and --gc-sections.
    foreach(target rendering-source rendering-capture rendering-pixels)
        target_compile_options(${target} PRIVATE /Gy /Gw)
    endforeach()
    foreach(target rendering-pixels rendering-capture)
        target_link_options(${target} PRIVATE /OPT:REF /OPT:ICF)
    endforeach()
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(target rendering-source rendering-capture rendering-pixels)
        target_compile_options(${target} PRIVATE -ffunction-sections -fdata-sections)
    endforeach()
    foreach(target rendering-pixels rendering-capture)
        target_link_options(${target} PRIVATE -Wl,--gc-sections)
    endforeach()
    if(CP4F_CAPTURE_SANITIZERS)
        target_compile_options(rendering-capture PRIVATE -O0 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(rendering-capture PRIVATE -fsanitize=address,undefined)
    endif()
endif()
add_executable(rendering-uniforms "${OPENMW_ROOT}/tools/v4/cp4/rendering-uniform-tests.cpp")
target_link_libraries(rendering-uniforms PRIVATE rendering-source)
add_executable(rendering-view-features "${OPENMW_ROOT}/tools/v4/cp4/rendering-view-features-tests.cpp")
target_link_libraries(rendering-view-features PRIVATE rendering-source)
if(CP4F_CAPTURE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rendering-source PRIVATE -O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_compile_options(rendering-view-features PRIVATE -O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(rendering-source INTERFACE -fsanitize=address,undefined)
endif()
enable_testing()
add_subdirectory("${OPENMW_ROOT}/extern/maskedoc" maskedoc)
target_link_libraries(rendering-pixels PRIVATE maskedoc)
add_executable(native-visibility "${OPENMW_ROOT}/tools/v4/cp4/native-visibility-tests.cpp")
target_link_libraries(native-visibility PRIVATE rendering-source maskedoc)
add_test(NAME native-visibility COMMAND native-visibility)
add_executable(frame-handoff "${OPENMW_ROOT}/tools/v4/cp4/frame-handoff-tests.cpp")
target_include_directories(frame-handoff PRIVATE "${OPENMW_ROOT}" "${CP4F_GLM_INCLUDE_DIR}")
target_link_libraries(frame-handoff PRIVATE Threads::Threads)
add_test(NAME frame-handoff COMMAND frame-handoff)
add_executable(actor-preparation "${OPENMW_ROOT}/tools/v4/cp4/actor-preparation-tests.cpp")
target_include_directories(actor-preparation PRIVATE "${OPENMW_ROOT}" "${CP4F_GLM_INCLUDE_DIR}")
target_link_libraries(actor-preparation PRIVATE Threads::Threads)
add_test(NAME actor-preparation COMMAND actor-preparation)
add_executable(effect-stream-fastpath "${OPENMW_ROOT}/tools/v4/cp4/effect-stream-fastpath-tests.cpp")
target_link_libraries(effect-stream-fastpath PRIVATE rendering-source)
add_test(NAME effect-stream-fastpath COMMAND effect-stream-fastpath)
add_test(NAME effect-stream-incremental COMMAND effect-stream-fastpath)
set_tests_properties(effect-stream-incremental PROPERTIES ENVIRONMENT "OPENMW_V4_INCREMENTAL_CAPTURE=1")
foreach(test static-population-smoke static-sync-state-tests)
    add_executable(${test} "${OPENMW_ROOT}/tools/v4/cp4/${test}.cpp")
    target_include_directories(${test} PRIVATE "${OPENMW_ROOT}" "${CP4F_GLM_INCLUDE_DIR}")
    add_test(NAME ${test} COMMAND ${test})
endforeach()
add_test(NAME rendering-view-features COMMAND rendering-view-features)
add_test(NAME static-population-linear-control COMMAND static-population-smoke linear)
add_test(NAME rendering-uniforms COMMAND rendering-uniforms)
add_test(NAME rendering-capture COMMAND rendering-capture)
add_test(NAME rendering-capture-persistent COMMAND rendering-capture)
set_tests_properties(rendering-capture-persistent PROPERTIES ENVIRONMENT "OPENMW_V4_PERSISTENT_CAPTURE=1")
add_test(NAME rendering-capture-incremental COMMAND rendering-capture)
set_tests_properties(rendering-capture-incremental PROPERTIES ENVIRONMENT "OPENMW_V4_PERSISTENT_CAPTURE=1;OPENMW_V4_INCREMENTAL_CAPTURE=1")
add_test(NAME rendering-capture-exterior COMMAND rendering-capture)
set_tests_properties(rendering-capture-exterior PROPERTIES ENVIRONMENT "OPENMW_V4_PERSISTENT_CAPTURE=1;OPENMW_V4_INCREMENTAL_CAPTURE=1;OPENMW_V4_ACTIVE_SWITCH_CAPTURE=1")
add_test(NAME static-sync-revision-gate COMMAND static-sync-state-tests)
set_tests_properties(static-sync-revision-gate PROPERTIES ENVIRONMENT "OPENMW_V4_STATIC_REVISION_GATE=1")
add_executable(persistent-resource-tests "${OPENMW_ROOT}/tools/v4/cp4/persistent-resource-tests.cpp")
add_executable(persistent-draw-tests "${OPENMW_ROOT}/tools/v4/cp4/persistent-draw-tests.cpp")
target_link_libraries(persistent-draw-tests PRIVATE rendering-source)
add_test(NAME persistent-draw-tests COMMAND persistent-draw-tests)
add_test(NAME persistent-draw-inventories COMMAND persistent-draw-tests)
set_tests_properties(persistent-draw-inventories PROPERTIES ENVIRONMENT "OPENMW_V4_PIPELINE_INVENTORIES=1")
target_link_libraries(persistent-resource-tests PRIVATE rendering-source)
add_test(NAME persistent-resource-tests COMMAND persistent-resource-tests)
add_test(NAME persistent-resource-audit-dedup COMMAND persistent-resource-tests)
set_tests_properties(persistent-resource-audit-dedup PROPERTIES ENVIRONMENT "OPENMW_V4_DEDUP_PIPELINE_AUDIT=1")
add_test(NAME persistent-resource-audit-flat COMMAND persistent-resource-tests)
add_test(NAME persistent-resource-inventories COMMAND persistent-resource-tests)
set_tests_properties(persistent-resource-inventories PROPERTIES ENVIRONMENT "OPENMW_V4_PIPELINE_INVENTORIES=1;OPENMW_V4_FLAT_PIPELINE_AUDIT=1")
set_tests_properties(persistent-resource-audit-flat PROPERTIES ENVIRONMENT "OPENMW_V4_FLAT_PIPELINE_AUDIT=1")
add_test(NAME rendering-capture-material-values COMMAND rendering-capture)
set_tests_properties(rendering-capture-material-values PROPERTIES ENVIRONMENT "OPENMW_V4_PERSISTENT_CAPTURE=1;OPENMW_V4_INCREMENTAL_CAPTURE=1;OPENMW_V4_MATERIAL_VALUE_CACHE=1;OPENMW_V4_ACTIVE_SWITCH_CAPTURE=1")
add_test(NAME rendering-capture-architecture COMMAND rendering-capture)
set_tests_properties(rendering-capture-architecture PROPERTIES ENVIRONMENT "OPENMW_V4_PERSISTENT_CAPTURE=1;OPENMW_V4_INCREMENTAL_CAPTURE=1;OPENMW_V4_MATERIAL_VALUE_CACHE=1;OPENMW_V4_ACTIVE_SWITCH_CAPTURE=1;OPENMW_V4_STARTUP_FLAG_CACHE=1;OPENMW_V4_BATCH_TEXTURE_METADATA=1")
add_test(NAME rendering-capture-native-producers COMMAND rendering-capture)
set_tests_properties(rendering-capture-native-producers PROPERTIES ENVIRONMENT "OPENMW_V4_PERSISTENT_CAPTURE=1;OPENMW_V4_INCREMENTAL_CAPTURE=1;OPENMW_V4_NATIVE_OBJECT_PRODUCERS=1;OPENMW_V4_STARTUP_FLAG_CACHE=1")
add_test(NAME rendering-capture-native-loaded-textures COMMAND rendering-capture)
set_tests_properties(rendering-capture-native-loaded-textures PROPERTIES ENVIRONMENT "OPENMW_V4_PERSISTENT_CAPTURE=1;OPENMW_V4_INCREMENTAL_CAPTURE=1;OPENMW_V4_NATIVE_OBJECT_PRODUCERS=1;OPENMW_V4_LOAD_BOUND_TEXTURES=1;OPENMW_V4_STARTUP_FLAG_CACHE=1")
add_test(NAME persistent-resource-startup-flags COMMAND persistent-resource-tests)
set_tests_properties(persistent-resource-startup-flags PROPERTIES ENVIRONMENT "OPENMW_V4_STARTUP_FLAG_CACHE=1")
add_executable(host-memory-tests "${OPENMW_ROOT}/tools/v4/cp4/host-memory-tests.cpp"
    "${OPENMW_ROOT}/components/misc/hostmemory.cpp")
target_include_directories(host-memory-tests PRIVATE "${OPENMW_ROOT}")
target_link_libraries(host-memory-tests PRIVATE Threads::Threads)
add_test(NAME host-memory-tests COMMAND host-memory-tests)
# Pixel execution is explicit: missing Vulkan hardware is an error, never a PASS.

option(CP4F_CHECK_ENGINE_TUS "Compile changed production translation units (supplemental, not a full engine link)" OFF)
set(CP4F_ENGINE_INCLUDE_DIRS "" CACHE STRING "Native SDL3, Bullet, LuaJIT and Recast include paths")
if(CP4F_CHECK_ENGINE_TUS)
    foreach(route vulkan opengl)
        add_library(engine-tus-${route} OBJECT
            "${OPENMW_ROOT}/apps/openmw/mwrender/characterpreview.cpp"
            "${OPENMW_ROOT}/apps/openmw/mwworld/cellpreloader.cpp"
            "${OPENMW_ROOT}/components/resource/resourcesystem.cpp"
            "${OPENMW_ROOT}/components/sceneutil/util.cpp")
        target_include_directories(engine-tus-${route} PRIVATE "${OPENMW_ROOT}"
            "${OPENMW_ROOT}/extern/sol_config" "${OPENMW_ROOT}/extern/sol3" "${OPENMW_ROOT}/extern/oics")
        target_include_directories(engine-tus-${route} SYSTEM PRIVATE ${CP4F_ENGINE_INCLUDE_DIRS}
            "${CP4F_GLM_INCLUDE_DIR}" "${CP4F_MYGUI_INCLUDE_DIR}" ${OPENSCENEGRAPH_INCLUDE_DIRS})
        target_link_libraries(engine-tus-${route} PRIVATE rendering-deps)
        if(MSVC)
            target_compile_options(engine-tus-${route} PRIVATE /bigobj /permissive-)
            target_compile_definitions(engine-tus-${route} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        endif()
    endforeach()
    target_compile_definitions(engine-tus-vulkan PRIVATE OPENMW_ENABLE_V4_VULKAN_RUNTIME=1)
    target_sources(engine-tus-vulkan PRIVATE
        "${OPENMW_ROOT}/apps/openmw/mwrender/v4engineframecoordinator.cpp"
        "${OPENMW_ROOT}/apps/openmw/mwrender/v4enginerenderbridge.cpp"
        "${OPENMW_ROOT}/apps/openmw/mwrender/v4localmapbridge.cpp"
        "${OPENMW_ROOT}/apps/openmw/mwrender/v4previewbridge.cpp"
        "${OPENMW_ROOT}/components/render/backend/vsg/vsgsemanticsession.cpp"
        "${OPENMW_ROOT}/components/render/backend/vsg/vsgruntimehost.cpp")
endif()

set(CP4F_UNIFORM_BASELINE_INCLUDE "" CACHE PATH "Optional original uniform headers for an expected-failure control")
if(CP4F_UNIFORM_BASELINE_INCLUDE)
    add_executable(rendering-uniforms-baseline "${OPENMW_ROOT}/tools/v4/cp4/rendering-uniform-tests.cpp")
    target_include_directories(rendering-uniforms-baseline BEFORE PRIVATE "${CP4F_UNIFORM_BASELINE_INCLUDE}")
    target_link_libraries(rendering-uniforms-baseline PRIVATE rendering-source)
endif()

# Explicit old-capture control, not a different implementation hidden by include order.
set(CP4F_CAPTURE_BASELINE_INCLUDE "" CACHE PATH "Original capture header tree for regression control")
if(CP4F_CAPTURE_BASELINE_INCLUDE)
    target_include_directories(rendering-capture BEFORE PRIVATE "${CP4F_CAPTURE_BASELINE_INCLUDE}")
    target_include_directories(rendering-capture PRIVATE "${OPENMW_ROOT}/apps/openmw/mwrender")
endif()

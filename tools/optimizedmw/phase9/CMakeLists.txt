cmake_minimum_required(VERSION 3.20)
project(OptimizedMWPhase9 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
include("${ROOT}/cmake/OpenMWAssetDependencies.cmake")
openmw_asset_dependencies()
find_package(OpenSceneGraph 3.6 REQUIRED COMPONENTS osg osgUtil osgViewer osgDB osgGA)
find_package(OpenGL REQUIRED)
find_package(glm CONFIG QUIET)
if(NOT TARGET glm::glm)
  include(FetchContent)
  FetchContent_Declare(p9_glm GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG bf71a834948186f4097caa076cd2663c69a10e1e SOURCE_SUBDIR openmw-headers-only)
  FetchContent_MakeAvailable(p9_glm)
  add_library(p9-glm INTERFACE)
  target_include_directories(p9-glm SYSTEM INTERFACE "${p9_glm_SOURCE_DIR}")
  add_library(glm::glm ALIAS p9-glm)
endif()
option(P9_SANITIZERS "Address and undefined behavior sanitizer checks" OFF)
if(MSVC)
  add_compile_options(/W4 /EHsc)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()
if(P9_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
include_directories(${ROOT})
include_directories(SYSTEM ${OPENSCENEGRAPH_INCLUDE_DIRS})
add_library(p9-hitch-platform STATIC "${ROOT}/components/debug/hitchtrace.cpp"
  "${ROOT}/components/debug/diagnosticobserver.cpp"
  "${ROOT}/components/debug/debuglog.cpp"
  "${ROOT}/components/files/conversion.cpp"
  "${ROOT}/components/shader/programvariant.cpp"
  "${ROOT}/components/shader/assetgeometrycontract.cpp")
target_link_libraries(p9-hitch-platform PUBLIC ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL)
link_libraries(p9-hitch-platform)
enable_testing()
add_executable(p9-static-vao-runtime static-vao-runtime-tests.cpp)
target_link_libraries(p9-static-vao-runtime PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL)
if(MSVC)
  target_compile_options(p9-static-vao-runtime PRIVATE /Zi /Zo)
  target_link_options(p9-static-vao-runtime PRIVATE /DEBUG)
  target_link_libraries(p9-static-vao-runtime PRIVATE dbghelp)
endif()
add_test(NAME p9-static-vao-runtime COMMAND p9-static-vao-runtime)
set_tests_properties(p9-static-vao-runtime PROPERTIES TIMEOUT 60)
add_executable(p9-buffer-preparation-state buffer-preparation-state-tests.cpp)
target_link_libraries(p9-buffer-preparation-state PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL)
add_test(NAME p9-buffer-preparation-state COMMAND p9-buffer-preparation-state)
set_tests_properties(p9-buffer-preparation-state PROPERTIES TIMEOUT 45)
add_executable(p9-derived-asset-cache derived-asset-cache-tests.cpp)
target_link_libraries(p9-derived-asset-cache PRIVATE Threads::Threads)
add_test(NAME p9-derived-asset-cache COMMAND p9-derived-asset-cache)
set_tests_properties(p9-derived-asset-cache PROPERTIES TIMEOUT 60)
add_executable(p9-asset-gpu-budget asset-gpu-budget-tests.cpp)
add_executable(p9-experimental-cache-budget experimental-cache-budget-tests.cpp)
add_executable(p9-settings-aliases settings-alias-tests.cpp ${ROOT}/components/settings/parser.cpp)
target_include_directories(p9-settings-aliases PRIVATE ${ROOT}/extern/Base64)
add_test(NAME p9-settings-aliases COMMAND p9-settings-aliases
  ${CMAKE_CURRENT_BINARY_DIR}/settings-alias-fixture ${ROOT}/files/settings-default.cfg)
set_tests_properties(p9-settings-aliases PROPERTIES TIMEOUT 45)
target_link_libraries(p9-experimental-cache-budget PRIVATE Threads::Threads)
add_test(NAME p9-experimental-cache-budget COMMAND p9-experimental-cache-budget)
set_tests_properties(p9-experimental-cache-budget PROPERTIES TIMEOUT 45)
target_link_libraries(p9-asset-gpu-budget PRIVATE Threads::Threads)
add_test(NAME p9-asset-gpu-budget COMMAND p9-asset-gpu-budget)
set_tests_properties(p9-asset-gpu-budget PROPERTIES TIMEOUT 30)
add_library(p9-texture-runtime STATIC
  ${ROOT}/components/resource/nativetexture.cpp ${ROOT}/components/resource/texturestreaming.cpp
  ${ROOT}/components/vfs/manager.cpp ${ROOT}/components/debug/debuglog.cpp ${ROOT}/components/files/conversion.cpp)
target_sources(p9-texture-runtime PRIVATE ${ROOT}/components/vfs/filesystemarchive.cpp
  ${ROOT}/components/files/constrainedfilestreambuf.cpp)
if(WIN32)
  target_sources(p9-texture-runtime PRIVATE ${ROOT}/components/platform/filewin32.cpp)
else()
  target_sources(p9-texture-runtime PRIVATE ${ROOT}/components/platform/fileposix.cpp)
endif()
target_compile_definitions(p9-texture-runtime PRIVATE OPENMW_NATIVE_TEXTURE_TEST_HOOKS=1)
target_link_libraries(p9-texture-runtime PUBLIC ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL Threads::Threads openmw-asset-backends)
add_executable(p9-native-texture native-texture-tests.cpp
  ${ROOT}/components/terrain/terraindrawable.cpp ${ROOT}/components/terrain/compositemaprenderer.cpp
  ${ROOT}/components/resource/openmwcompileoperation.cpp)
target_compile_definitions(p9-native-texture PRIVATE OPENMW_NATIVE_TEXTURE_TEST_HOOKS=1)
target_link_libraries(p9-native-texture PRIVATE p9-texture-runtime)
add_test(NAME p9-native-texture COMMAND p9-native-texture)
add_test(NAME p9-native-texture-control COMMAND p9-native-texture --control)
set_tests_properties(p9-native-texture p9-native-texture-control PROPERTIES TIMEOUT 90
  ENVIRONMENT "OPENMW_P9_DERIVED_CACHE_DIR=${CMAKE_CURRENT_BINARY_DIR}/native-texture-cache")
add_executable(p9-selective-hlod selective-hlod-tests.cpp
  ${ROOT}/components/sceneutil/selectivehlod.cpp ${ROOT}/components/sceneutil/generatedstaticlod.cpp
  ${ROOT}/components/nifosg/matrixtransform.cpp)
target_link_libraries(p9-selective-hlod PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL
  Threads::Threads openmw-asset-backends)
add_test(NAME p9-selective-hlod COMMAND p9-selective-hlod)
set_tests_properties(p9-selective-hlod PROPERTIES TIMEOUT 90 ENVIRONMENT "OPENMW_P9_ASSET_MASK=2")
add_executable(p9-exact-paging-work exact-paging-work-tests.cpp)
add_executable(p9-owned-cell-cull owned-cell-cull-tests.cpp ${ROOT}/components/sceneutil/positionattitudetransform.cpp)
add_executable(p9-temporal-history-lookup temporal-history-lookup-tests.cpp)
add_test(NAME p9-temporal-history-lookup COMMAND p9-temporal-history-lookup)
set_tests_properties(p9-temporal-history-lookup PROPERTIES TIMEOUT 45)
add_executable(p9-diagnostic-namespace diagnostic-namespace-tests.cpp)
target_link_libraries(p9-diagnostic-namespace PRIVATE Threads::Threads)
add_test(NAME p9-diagnostic-namespace COMMAND p9-diagnostic-namespace)
set_tests_properties(p9-diagnostic-namespace PROPERTIES TIMEOUT 30)
target_link_libraries(p9-owned-cell-cull PRIVATE ${OPENSCENEGRAPH_LIBRARIES})
add_test(NAME p9-owned-cell-cull COMMAND p9-owned-cell-cull)
set_tests_properties(p9-owned-cell-cull PROPERTIES TIMEOUT 45)
add_executable(p9-preload-priority preload-priority-tests.cpp ${ROOT}/components/sceneutil/workqueue.cpp)
target_link_libraries(p9-preload-priority PRIVATE Threads::Threads)
add_test(NAME p9-preload-priority COMMAND p9-preload-priority)
set_tests_properties(p9-preload-priority PROPERTIES TIMEOUT 45)
add_test(NAME p9-exact-paging-work COMMAND p9-exact-paging-work)
add_test(NAME p9-paging-score-values COMMAND p9-exact-paging-work --score-values)
set_tests_properties(p9-exact-paging-work PROPERTIES TIMEOUT 90)
set_tests_properties(p9-paging-score-values PROPERTIES TIMEOUT 90)
if(WIN32)
  find_program(P9_MENU_POWERSHELL NAMES powershell.exe REQUIRED)
  add_test(NAME p9-numeric-menu COMMAND ${P9_MENU_POWERSHELL}
    -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_CURRENT_SOURCE_DIR}/numeric-menu-tests.ps1)
  add_test(NAME p9-settings-editor COMMAND ${P9_MENU_POWERSHELL}
    -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_CURRENT_SOURCE_DIR}/settings-editor-tests.ps1)
  add_test(NAME p9-memory-guard COMMAND ${P9_MENU_POWERSHELL}
    -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_CURRENT_SOURCE_DIR}/memory-guard-tests.ps1)
  # CTest can inherit a PS7-only module path from the build shell. The installed
  # launcher uses PS5.1; isolate this hermetic fixture with its native modules.
  set_tests_properties(p9-numeric-menu PROPERTIES TIMEOUT 30
    ENVIRONMENT "PSModulePath=$ENV{WINDIR}/System32/WindowsPowerShell/v1.0/Modules")
endif()
add_executable(p9-delete-admission delete-admission-tests.cpp
  ${ROOT}/components/resource/openmwcompileoperation.cpp)
target_link_libraries(p9-delete-admission PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL Threads::Threads)
target_link_libraries(p9-delete-admission PRIVATE p9-texture-runtime)
add_test(NAME p9-delete-admission COMMAND p9-delete-admission)
set_tests_properties(p9-delete-admission PROPERTIES TIMEOUT 45)
add_executable(p9-benchmark-context benchmark-context-tests.cpp)
add_test(NAME p9-benchmark-context COMMAND p9-benchmark-context benchmark-context-fixture)
set_tests_properties(p9-benchmark-context PROPERTIES TIMEOUT 45)
find_package(Threads REQUIRED)
add_executable(p9-shared-context-preparation shared-context-preparation-tests.cpp)
target_link_libraries(p9-shared-context-preparation PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL Threads::Threads)
add_test(NAME p9-shared-context-preparation COMMAND p9-shared-context-preparation)
set_tests_properties(p9-shared-context-preparation PROPERTIES TIMEOUT 45 ENVIRONMENT "OPENMW_EXACT_PROGRAM_VARIANTS=1")
add_executable(p9-persistent-opaque-packets persistent-opaque-packets-tests.cpp)
target_link_libraries(p9-persistent-opaque-packets PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL Threads::Threads)
add_test(NAME p9-persistent-opaque-packets COMMAND p9-persistent-opaque-packets)
add_executable(p9-retained-material-bucket-prototype retained-material-bucket-tests.cpp)
target_link_libraries(p9-retained-material-bucket-prototype PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL Threads::Threads)
add_test(NAME p9-retained-material-bucket-prototype COMMAND p9-retained-material-bucket-prototype)
set_tests_properties(p9-retained-material-bucket-prototype PROPERTIES TIMEOUT 60)
add_test(NAME p9-persistent-opaque-packets-deep COMMAND p9-persistent-opaque-packets --deep)
add_executable(p9-gpu-indirect-packets gpu-indirect-packets-tests.cpp)
target_compile_definitions(p9-gpu-indirect-packets PRIVATE OPENMW_INDIRECT_PACKET_TEST_HOOKS)
target_link_libraries(p9-gpu-indirect-packets PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL Threads::Threads)
add_test(NAME p9-gpu-indirect-packets COMMAND p9-gpu-indirect-packets)
add_test(NAME p9-gpu-indirect-command-reuse COMMAND p9-gpu-indirect-packets --reuse-commands)
add_test(NAME p9-gpu-indirect-capability-reuse COMMAND p9-gpu-indirect-packets --reuse-capabilities)
add_test(NAME p9-gpu-indirect-opportunity-filter COMMAND p9-gpu-indirect-packets --filter-opportunities)
add_test(NAME p9-gpu-indirect-overhead-combined COMMAND p9-gpu-indirect-packets --reuse-commands --reuse-capabilities --filter-opportunities)
add_test(NAME p9-indirect-projection-proof COMMAND p9-gpu-indirect-packets --reuse-projection-proof)
add_test(NAME p9-indirect-early-admission COMMAND p9-gpu-indirect-packets --early-admission-fallback)
add_test(NAME p9-indirect-consumption-combined COMMAND p9-gpu-indirect-packets --reuse-capabilities --filter-opportunities --reuse-projection-proof --early-admission-fallback)
set_tests_properties(p9-indirect-projection-proof p9-indirect-early-admission p9-indirect-consumption-combined PROPERTIES TIMEOUT 90)
set_tests_properties(p9-gpu-indirect-capability-reuse p9-gpu-indirect-opportunity-filter p9-gpu-indirect-overhead-combined PROPERTIES TIMEOUT 90)
set_tests_properties(p9-gpu-indirect-command-reuse PROPERTIES TIMEOUT 90)
set_tests_properties(p9-gpu-indirect-packets PROPERTIES TIMEOUT 90)
set_tests_properties(p9-persistent-opaque-packets p9-persistent-opaque-packets-deep PROPERTIES TIMEOUT 60)
set_tests_properties(p9-persistent-opaque-packets-deep PROPERTIES ENVIRONMENT
  "OPENMW_P9_LEAF_TRACE_FILE=${CMAKE_CURRENT_BINARY_DIR}/persistent-packets-deep.csv")
add_executable(p9-composite-progress-policy composite-progress-policy-tests.cpp
  ${ROOT}/components/resource/openmwcompileoperation.cpp)
target_link_libraries(p9-composite-progress-policy PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL Threads::Threads)
target_link_libraries(p9-composite-progress-policy PRIVATE p9-texture-runtime)
add_test(NAME p9-composite-progress-policy COMMAND p9-composite-progress-policy)
set_tests_properties(p9-composite-progress-policy PROPERTIES TIMEOUT 45)
add_executable(p9-composite-cpu-mips composite-cpu-mips-tests.cpp)
target_compile_definitions(p9-composite-cpu-mips PRIVATE OPENMW_COMPOSITE_MIPS_TEST_HOOKS)
target_link_libraries(p9-composite-cpu-mips PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL Threads::Threads)
add_test(NAME p9-composite-cpu-mips COMMAND p9-composite-cpu-mips)
add_test(NAME p9-composite-cpu-mips-gl COMMAND p9-composite-cpu-mips --gl ${ROOT}/files/shaders/compatibility)
add_test(NAME p9-composite-cpu-mips-pool COMMAND p9-composite-cpu-mips --gl ${ROOT}/files/shaders/compatibility)
set_tests_properties(p9-composite-cpu-mips-pool PROPERTIES TIMEOUT 90 ENVIRONMENT "OPENMW_P9_UPLOAD_POOL=1")
set_tests_properties(p9-composite-cpu-mips p9-composite-cpu-mips-gl PROPERTIES TIMEOUT 90)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME p9-benchmark-supervisor COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/benchmark-supervisor-tests.py)
add_test(NAME p9-indirect-interval-work COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/indirect-work-tests.py)
add_test(NAME p9-runtime-dependencies COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/runtime-dependencies-tests.py)
set_tests_properties(p9-runtime-dependencies PROPERTIES TIMEOUT 30)
set_tests_properties(p9-indirect-interval-work PROPERTIES TIMEOUT 30)
add_test(NAME p9-optimization-aliases COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/generate-optimization-aliases.py --check)
set_tests_properties(p9-optimization-aliases PROPERTIES TIMEOUT 30)
if(WIN32)
  add_test(NAME p9-route-protected-inputs COMMAND ${P9_MENU_POWERSHELL}
    -NoProfile -NonInteractive -ExecutionPolicy Bypass -File ${CMAKE_CURRENT_SOURCE_DIR}/route-protected-input-tests.ps1)
  set_tests_properties(p9-route-protected-inputs PROPERTIES TIMEOUT 30
    ENVIRONMENT "PSModulePath=$ENV{WINDIR}/System32/WindowsPowerShell/v1.0/Modules")
  add_test(NAME p9-memory-profiles COMMAND ${P9_MENU_POWERSHELL}
    -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_CURRENT_SOURCE_DIR}/memory-profile-tests.ps1)
  add_test(NAME p9-setting-writer COMMAND ${P9_MENU_POWERSHELL}
    -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_CURRENT_SOURCE_DIR}/setting-names-tests.ps1)
endif()
foreach(asset_gate asset-paging-package asset-shader-release)
  add_test(NAME p9-${asset_gate} COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/${asset_gate}-tests.py)
  set_tests_properties(p9-${asset_gate} PROPERTIES TIMEOUT 45)
endforeach()
add_test(NAME p9-asset-engagement COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/asset-engagement-tests.py)
set_tests_properties(p9-asset-engagement PROPERTIES TIMEOUT 45)
if(WIN32)
  add_test(NAME p9-asset-engagement-wrapper COMMAND ${P9_MENU_POWERSHELL}
    -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_CURRENT_SOURCE_DIR}/asset-engagement-wrapper-tests.ps1)
  set_tests_properties(p9-asset-engagement-wrapper PROPERTIES TIMEOUT 45
    ENVIRONMENT "PSModulePath=$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/Modules")
endif()
add_executable(p9-animation-preparation animation-preparation-tests.cpp
  ${ROOT}/components/vfs/manager.cpp ${ROOT}/components/files/conversion.cpp
  ${ROOT}/components/misc/hostmemory.cpp)
target_link_libraries(p9-animation-preparation PRIVATE ${OPENSCENEGRAPH_LIBRARIES} Threads::Threads)
if(WIN32)
  target_link_libraries(p9-animation-preparation PRIVATE psapi)
endif()
foreach(mode control lifetime plan both)
  add_test(NAME p9-animation-preparation-${mode} COMMAND p9-animation-preparation ${mode})
  set_tests_properties(p9-animation-preparation-${mode} PROPERTIES TIMEOUT 45)
endforeach()
set_tests_properties(p9-animation-preparation-control PROPERTIES ENVIRONMENT
  "OPENMW_HITCH_ANIMATION_PRELOAD=0;OPENMW_HITCH_ANIMATION_LIFETIME=0;OPENMW_HITCH_ANIMATION_PLAN=0")
set_tests_properties(p9-animation-preparation-lifetime PROPERTIES ENVIRONMENT
  "OPENMW_HITCH_ANIMATION_PRELOAD=1;OPENMW_HITCH_ANIMATION_LIFETIME=1;OPENMW_HITCH_ANIMATION_PLAN=0")
set_tests_properties(p9-animation-preparation-plan PROPERTIES ENVIRONMENT
  "OPENMW_HITCH_ANIMATION_PRELOAD=0;OPENMW_HITCH_ANIMATION_LIFETIME=0;OPENMW_HITCH_ANIMATION_PLAN=1")
set_tests_properties(p9-animation-preparation-both PROPERTIES ENVIRONMENT
  "OPENMW_HITCH_ANIMATION_PRELOAD=1;OPENMW_HITCH_ANIMATION_LIFETIME=1;OPENMW_HITCH_ANIMATION_PLAN=1")
add_executable(p9-terrain-target-readiness terrain-target-readiness-tests.cpp
  ${ROOT}/components/loadinglistener/reporter.cpp)
target_link_libraries(p9-terrain-target-readiness PRIVATE Threads::Threads)
add_test(NAME p9-terrain-target-readiness COMMAND p9-terrain-target-readiness)
set_tests_properties(p9-terrain-target-readiness PROPERTIES TIMEOUT 30)
add_executable(p9-hitch-trace hitch-trace-tests.cpp)
target_link_libraries(p9-hitch-trace PRIVATE Threads::Threads)
add_test(NAME p9-hitch-trace COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/verify-hitch-trace.py $<TARGET_FILE:p9-hitch-trace>
  ${CMAKE_CURRENT_BINARY_DIR}/hitch-trace-fixture)
set_tests_properties(p9-hitch-trace PROPERTIES TIMEOUT 180)
add_executable(p9-diagnostic-observer diagnostic-observer-tests.cpp)
target_link_libraries(p9-diagnostic-observer PRIVATE Threads::Threads)
foreach(mode normal deep overflow normal-local deep-local overflow-local active-local independent-local late-tls-local undeclared-local)
  add_test(NAME p9-diagnostic-observer-${mode} COMMAND p9-diagnostic-observer ${mode}
    ${CMAKE_CURRENT_BINARY_DIR}/diagnostic-observer-${mode})
  set_tests_properties(p9-diagnostic-observer-${mode} PROPERTIES TIMEOUT 45)
endforeach()
find_library(P9_CLOUD_OSGDB_LIBRARY NAMES osgDB REQUIRED)
list(APPEND CMAKE_MODULE_PATH "${ROOT}/cmake")
find_package(LZ4 MODULE REQUIRED)
add_executable(p9-ram-retention ram-retention-tests.cpp ${ROOT}/components/resource/multiobjectcache.cpp)
target_link_libraries(p9-ram-retention PRIVATE ${OPENSCENEGRAPH_LIBRARIES} Threads::Threads)
add_test(NAME p9-ram-retention COMMAND p9-ram-retention)
set_tests_properties(p9-ram-retention PROPERTIES TIMEOUT 45)
add_executable(p9-cold-image-cache cold-image-cache-tests.cpp
  ${ROOT}/components/resource/coldimage.cpp ${ROOT}/components/resource/imagemanager.cpp
  ${ROOT}/components/resource/cachestats.cpp ${ROOT}/components/vfs/manager.cpp
  ${ROOT}/components/files/conversion.cpp ${ROOT}/components/sceneutil/workqueue.cpp
  ${ROOT}/components/sceneutil/glextensions.cpp ${ROOT}/components/debug/debuglog.cpp
  ${ROOT}/components/misc/hostmemory.cpp)
target_compile_definitions(p9-cold-image-cache PRIVATE OPENMW_COLD_IMAGE_TEST_HOOKS)
target_link_libraries(p9-cold-image-cache PRIVATE
  ${OPENSCENEGRAPH_LIBRARIES} ${P9_CLOUD_OSGDB_LIBRARY} OpenGL::GL Threads::Threads LZ4::LZ4)
add_test(NAME p9-cold-image-cache COMMAND p9-cold-image-cache)
# Match the production double-precision Bullet ABI, including Debian's explicit
# float64 package rather than silently selecting its ordinary float library.
find_package(Bullet CONFIGS BulletConfig-float64.cmake QUIET COMPONENTS BulletCollision LinearMath)
if(BULLET_FOUND)
  if(NOT IS_ABSOLUTE "${BULLET_INCLUDE_DIRS}")
    set(BULLET_INCLUDE_DIRS "${BULLET_ROOT_DIR}/${BULLET_INCLUDE_DIRS}")
  endif()
else()
  find_package(Bullet REQUIRED)
endif()
include("${ROOT}/cmake/CheckBulletPrecision.cmake")
if(NOT HAS_DOUBLE_PRECISION_BULLET)
  message(FATAL_ERROR "Parsed NIF fixture requires the production double-precision Bullet ABI")
endif()
add_executable(p9-loading-nif-maintenance loading-nif-maintenance-tests.cpp
  ${ROOT}/components/resource/niffilemanager.cpp ${ROOT}/components/resource/cachestats.cpp
  ${ROOT}/components/resource/bulletshape.cpp
  ${ROOT}/components/vfs/manager.cpp ${ROOT}/components/sceneutil/workqueue.cpp
  ${ROOT}/components/files/hash.cpp ${ROOT}/components/files/conversion.cpp
  ${ROOT}/components/files/constrainedfilestreambuf.cpp
  ${ROOT}/extern/smhasher/MurmurHash3.cpp ${ROOT}/components/toutf8/toutf8.cpp)
foreach(nif_source base controller data effect extra niffile nifstream node particle physics property texture)
  target_sources(p9-loading-nif-maintenance PRIVATE ${ROOT}/components/nif/${nif_source}.cpp)
endforeach()
if(WIN32)
  target_sources(p9-loading-nif-maintenance PRIVATE ${ROOT}/components/platform/filewin32.cpp)
else()
  target_sources(p9-loading-nif-maintenance PRIVATE ${ROOT}/components/platform/fileposix.cpp)
endif()
target_include_directories(p9-loading-nif-maintenance PRIVATE
  ${BULLET_INCLUDE_DIRS} ${ROOT}/extern/smhasher/include ${ROOT}/extern/smhasher/include/smhasher)
target_compile_definitions(p9-loading-nif-maintenance PRIVATE OPENMW_WORKQUEUE_PUBLICATION_TEST_HOOKS=1 BT_USE_DOUBLE_PRECISION)
target_link_libraries(p9-loading-nif-maintenance PRIVATE ${OPENSCENEGRAPH_LIBRARIES} ${BULLET_LIBRARIES} OpenGL::GL Threads::Threads)
if(NOT MSVC)
  target_compile_options(p9-loading-nif-maintenance PRIVATE -Wno-error=unused-parameter)
endif()
add_test(NAME p9-loading-nif-maintenance COMMAND p9-loading-nif-maintenance)
set_tests_properties(p9-loading-nif-maintenance PROPERTIES TIMEOUT 60)
add_executable(p9-compact-static-collision compact-static-collision-tests.cpp)
target_include_directories(p9-compact-static-collision SYSTEM PRIVATE ${BULLET_INCLUDE_DIRS})
target_compile_definitions(p9-compact-static-collision PRIVATE BT_USE_DOUBLE_PRECISION)
target_link_libraries(p9-compact-static-collision PRIVATE ${BULLET_LIBRARIES} Threads::Threads)
add_test(NAME p9-compact-static-collision COMMAND p9-compact-static-collision)
set_tests_properties(p9-compact-static-collision PROPERTIES TIMEOUT 60)
# Native Windows fixture only. Keep the game SDL owner unchanged and require
# an explicit pinned SDK location rather than choosing an ambient SDL library.
set(P9_SDL3_ROOT "" CACHE PATH "Pinned SDL3 SDK for the standalone namespace fixture")
if(WIN32 AND P9_SDL3_ROOT)
  if(NOT EXISTS "${P9_SDL3_ROOT}/include/SDL3/SDL.h" OR
     NOT EXISTS "${P9_SDL3_ROOT}/lib/x64/SDL3.lib")
    message(FATAL_ERROR "The native SDL namespace fixture requires the pinned x64 SDL3 SDK")
  endif()
  add_executable(p9-diagnostic-sdl-namespace diagnostic-sdl-namespace-tests.cpp diagnostic-sdl-window-owner.cpp)
  target_include_directories(p9-diagnostic-sdl-namespace SYSTEM PRIVATE "${P9_SDL3_ROOT}/include")
  target_compile_definitions(p9-diagnostic-sdl-namespace PRIVATE SDL_DISABLE_OLD_NAMES)
  target_link_libraries(p9-diagnostic-sdl-namespace PRIVATE "${P9_SDL3_ROOT}/lib/x64/SDL3.lib"
    ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL Threads::Threads user32 gdi32)
  add_test(NAME p9-diagnostic-sdl-namespace COMMAND p9-diagnostic-sdl-namespace)
  set_tests_properties(p9-diagnostic-sdl-namespace PROPERTIES TIMEOUT 60)
endif()
if(MSVC)
  target_compile_options(p9-compact-static-collision PRIVATE /Zi /WX)
  target_link_options(p9-compact-static-collision PRIVATE /DEBUG)
  if(TARGET p9-diagnostic-sdl-namespace)
    target_compile_options(p9-diagnostic-sdl-namespace PRIVATE /Zi /WX)
    target_link_options(p9-diagnostic-sdl-namespace PRIVATE /DEBUG)
  endif()
endif()
add_executable(p9-image-publication image-publication-tests.cpp
  ${ROOT}/components/resource/coldimage.cpp ${ROOT}/components/resource/imagemanager.cpp
  ${ROOT}/components/resource/cachestats.cpp ${ROOT}/components/vfs/manager.cpp
  ${ROOT}/components/files/conversion.cpp ${ROOT}/components/sceneutil/workqueue.cpp
  ${ROOT}/components/sceneutil/glextensions.cpp ${ROOT}/components/debug/debuglog.cpp
  ${ROOT}/components/misc/hostmemory.cpp)
target_link_libraries(p9-image-publication PRIVATE
  ${OPENSCENEGRAPH_LIBRARIES} ${P9_CLOUD_OSGDB_LIBRARY} OpenGL::GL Threads::Threads LZ4::LZ4)
if(WIN32)
  target_link_libraries(p9-image-publication PRIVATE psapi)
endif()
add_test(NAME p9-image-publication COMMAND p9-image-publication)
set_tests_properties(p9-image-publication PROPERTIES TIMEOUT 90)
set_tests_properties(p9-cold-image-cache PROPERTIES TIMEOUT 90)
if(WIN32)
  target_link_libraries(p9-cold-image-cache PRIVATE psapi)
endif()
add_executable(p9-cloud-image-preparation cloud-image-preparation-tests.cpp
  ${ROOT}/components/resource/coldimage.cpp
  ${ROOT}/components/resource/imagemanager.cpp ${ROOT}/components/resource/cachestats.cpp
  ${ROOT}/components/vfs/manager.cpp ${ROOT}/components/files/conversion.cpp
  ${ROOT}/components/sceneutil/workqueue.cpp ${ROOT}/components/sceneutil/glextensions.cpp
  ${ROOT}/components/debug/debuglog.cpp ${ROOT}/components/misc/hostmemory.cpp)
target_link_libraries(p9-cloud-image-preparation PRIVATE
  ${OPENSCENEGRAPH_LIBRARIES} ${P9_CLOUD_OSGDB_LIBRARY} OpenGL::GL Threads::Threads LZ4::LZ4)
if(WIN32)
  target_link_libraries(p9-cloud-image-preparation PRIVATE psapi)
endif()
if(NOT MSVC)
  target_compile_options(p9-cloud-image-preparation PRIVATE -Wno-error=unused-parameter)
endif()
add_test(NAME p9-cloud-image-preparation COMMAND p9-cloud-image-preparation)
set_tests_properties(p9-cloud-image-preparation PROPERTIES TIMEOUT 45)
add_executable(p9-loading-attribution loading-attribution-tests.cpp
  ${ROOT}/components/files/hash.cpp ${ROOT}/components/files/constrainedfilestreambuf.cpp
  ${ROOT}/components/files/conversion.cpp ${ROOT}/extern/smhasher/MurmurHash3.cpp)
if(WIN32)
  target_sources(p9-loading-attribution PRIVATE ${ROOT}/components/platform/filewin32.cpp)
else()
  target_sources(p9-loading-attribution PRIVATE ${ROOT}/components/platform/fileposix.cpp)
endif()
target_include_directories(p9-loading-attribution PRIVATE
  ${ROOT}/extern/smhasher/include ${ROOT}/extern/smhasher/include/smhasher)
target_link_libraries(p9-loading-attribution PRIVATE Threads::Threads)
add_test(NAME p9-loading-attribution COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/verify-loading-attribution.py $<TARGET_FILE:p9-loading-attribution>
  ${CMAKE_CURRENT_BINARY_DIR}/loading-attribution-fixture)
set_tests_properties(p9-loading-attribution PROPERTIES TIMEOUT 45)
add_executable(p9-constrained-stream constrained-stream-tests.cpp
  ${ROOT}/components/bsa/bsafile.cpp
  ${ROOT}/components/files/hash.cpp ${ROOT}/components/files/constrainedfilestreambuf.cpp
  ${ROOT}/components/files/conversion.cpp ${ROOT}/extern/smhasher/MurmurHash3.cpp)
if(WIN32)
  target_sources(p9-constrained-stream PRIVATE ${ROOT}/components/platform/filewin32.cpp)
else()
  target_sources(p9-constrained-stream PRIVATE ${ROOT}/components/platform/fileposix.cpp)
endif()
target_compile_definitions(p9-constrained-stream PRIVATE P9_FILE_STREAM_TEST_HOOKS)
target_include_directories(p9-constrained-stream PRIVATE
  ${ROOT}/extern/smhasher/include ${ROOT}/extern/smhasher/include/smhasher)
target_link_libraries(p9-constrained-stream PRIVATE Threads::Threads)
add_test(NAME p9-constrained-stream COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/verify-constrained-stream.py $<TARGET_FILE:p9-constrained-stream>
  ${CMAKE_CURRENT_BINARY_DIR}/constrained-stream-fixture)
set_tests_properties(p9-constrained-stream PROPERTIES TIMEOUT 180)
option(P9_REAL_READER_FIXTURE "Validate constrained I/O against actual DDS/TGA/NIF/BSA parsers" OFF)
set(P9_READER_BSA_FIXTURE "" CACHE FILEPATH "Read-only installed Morrowind BSA used by real-reader fixture")
if(P9_REAL_READER_FIXTURE)
  if(NOT EXISTS "${P9_READER_BSA_FIXTURE}")
    message(FATAL_ERROR "P9_REAL_READER_FIXTURE requires an existing P9_READER_BSA_FIXTURE")
  endif()
  # The parsed-NIF fixture above has already selected and checked the exact ABI.
  find_library(P9_READER_OSGDB_LIBRARY NAMES osgDB REQUIRED)
  add_executable(p9-constrained-readers constrained-reader-tests.cpp
    ${ROOT}/components/files/hash.cpp ${ROOT}/components/files/constrainedfilestreambuf.cpp
    ${ROOT}/components/files/conversion.cpp ${ROOT}/extern/smhasher/MurmurHash3.cpp
    ${ROOT}/components/bsa/bsafile.cpp ${ROOT}/components/debug/debuglog.cpp
    ${ROOT}/components/toutf8/toutf8.cpp ${ROOT}/components/resource/bulletshape.cpp)
  foreach(nif_source base controller data effect extra niffile nifstream node particle physics property texture)
    target_sources(p9-constrained-readers PRIVATE ${ROOT}/components/nif/${nif_source}.cpp)
  endforeach()
  if(WIN32)
    target_sources(p9-constrained-readers PRIVATE ${ROOT}/components/platform/filewin32.cpp)
  else()
    target_sources(p9-constrained-readers PRIVATE ${ROOT}/components/platform/fileposix.cpp)
  endif()
  target_include_directories(p9-constrained-readers PRIVATE
    ${BULLET_INCLUDE_DIRS} ${ROOT}/extern/smhasher/include ${ROOT}/extern/smhasher/include/smhasher)
  # Match the authoritative components target and its pinned Bullet ABI.
  target_compile_definitions(p9-constrained-readers PRIVATE BT_USE_DOUBLE_PRECISION)
  target_link_libraries(p9-constrained-readers PRIVATE
    ${OPENSCENEGRAPH_LIBRARIES} ${P9_READER_OSGDB_LIBRARY} ${BULLET_LIBRARIES} Threads::Threads)
  if(NOT MSVC)
    target_compile_options(p9-constrained-readers PRIVATE -Wno-error=unused-parameter)
  endif()
  add_test(NAME p9-constrained-readers COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/verify-constrained-readers.py $<TARGET_FILE:p9-constrained-readers>
    ${CMAKE_CURRENT_BINARY_DIR}/constrained-reader-fixture ${ROOT} ${P9_READER_BSA_FIXTURE})
  set_tests_properties(p9-constrained-readers PROPERTIES TIMEOUT 90)
endif()
add_executable(p9-capture-transport capture-transport-tests.cpp)
target_link_libraries(p9-capture-transport PRIVATE Threads::Threads)
add_test(NAME p9-capture-transport COMMAND p9-capture-transport
  ${CMAKE_CURRENT_BINARY_DIR}/capture-transport-output)
set_tests_properties(p9-capture-transport PROPERTIES TIMEOUT 45)
add_executable(p9-gpu-owner gpu-owner-tests.cpp)
target_link_libraries(p9-gpu-owner PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL Threads::Threads)
add_test(NAME p9-gpu-owner COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/verify-gpu-owner.py
  $<TARGET_FILE:p9-gpu-owner> ${CMAKE_CURRENT_BINARY_DIR}/gpu-owner.csv)
set_tests_properties(p9-gpu-owner PROPERTIES TIMEOUT 45)
add_executable(p9-gpu-query-contract gpu-query-tests.cpp)
target_link_libraries(p9-gpu-query-contract PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL Threads::Threads)
add_test(NAME p9-gpu-query-contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/verify-gpu-query.py $<TARGET_FILE:p9-gpu-query-contract>
  ${CMAKE_CURRENT_BINARY_DIR}/gpu-query-fixture)
set_tests_properties(p9-gpu-query-contract PROPERTIES TIMEOUT 60)
option(OPENMW_PHASE9_INTEROP_FIXTURE "Build the separate GL/Vulkan shared-image fixture" OFF)
if(OPENMW_PHASE9_INTEROP_FIXTURE)
  add_subdirectory(interop)
endif()
add_executable(p9-dynamic-stream dynamic-stream-tests.cpp)
add_executable(p9-upload-pool upload-pool-tests.cpp)
target_link_libraries(p9-upload-pool PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL Threads::Threads)
add_test(NAME p9-upload-pool COMMAND p9-upload-pool)
set_tests_properties(p9-upload-pool PROPERTIES TIMEOUT 45 ENVIRONMENT "OPENMW_P9_UPLOAD_POOL=1")
target_link_libraries(p9-dynamic-stream PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL)
add_test(NAME p9-dynamic-stream COMMAND p9-dynamic-stream)
set_tests_properties(p9-dynamic-stream PROPERTIES TIMEOUT 30)
add_library(p9-temporal-production STATIC "${ROOT}/apps/openmw/mwrender/temporalmotion.cpp"
  "${ROOT}/components/sceneutil/generatedstaticlod.cpp" "${ROOT}/components/nifosg/matrixtransform.cpp")
target_link_libraries(p9-temporal-production PRIVATE glm::glm ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL Threads::Threads openmw-asset-backends)
# Catch Windows-only enum gaps even on hosts providing newer core GL headers.
add_library(p9-temporal-header-compat OBJECT temporal-header-compat-tests.cpp)
target_link_libraries(p9-temporal-header-compat PRIVATE glm::glm ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL)
add_executable(p9-temporal-render temporal-render-tests.cpp)
target_link_libraries(p9-temporal-render PRIVATE p9-temporal-production glm::glm ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL)
target_compile_definitions(p9-temporal-render PRIVATE
  P9_VERTEX="${ROOT}/files/shaders/compatibility/temporal_camera_motion.vert"
  P9_FRAGMENT="${ROOT}/files/shaders/compatibility/temporal_camera_motion.frag")
add_test(NAME p9-temporal-render COMMAND p9-temporal-render)
set_tests_properties(p9-temporal-render PROPERTIES TIMEOUT 30)
add_executable(p9-temporal-owner temporal-owner-tests.cpp)
target_link_libraries(p9-temporal-owner PRIVATE p9-temporal-production glm::glm ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL Threads::Threads)
target_compile_definitions(p9-temporal-owner PRIVATE
  P9_VERTEX="${ROOT}/files/shaders/compatibility/temporal_camera_motion.vert"
  P9_FRAGMENT="${ROOT}/files/shaders/compatibility/temporal_camera_motion.frag")
add_test(NAME p9-temporal-owner COMMAND p9-temporal-owner)
add_test(NAME p9-temporal-owner-threaded COMMAND p9-temporal-owner --threaded)
set_tests_properties(p9-temporal-owner p9-temporal-owner-threaded PROPERTIES TIMEOUT 45)
if(NOT MSVC)
  # Retained StateSetUpdater/depth headers contain unused virtual parameters.
  # Keep their diagnostics visible when compiling the production owner fixture.
  target_compile_options(p9-temporal-owner PRIVATE -Wno-error=unused-parameter)
endif()
add_executable(p9-composite-preparation composite-preparation-tests.cpp
  ${ROOT}/components/terrain/compositemaprenderer.cpp ${ROOT}/components/resource/openmwcompileoperation.cpp)
target_link_libraries(p9-composite-preparation PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL Threads::Threads)
target_link_libraries(p9-composite-preparation PRIVATE p9-texture-runtime)
target_sources(p9-composite-preparation PRIVATE ${ROOT}/components/debug/debuglog.cpp
  ${ROOT}/components/files/conversion.cpp)
target_compile_definitions(p9-composite-preparation PRIVATE P9_COMPILE_TRACE_TEST_HOOKS)
add_test(NAME p9-composite-preparation COMMAND p9-composite-preparation)
set_tests_properties(p9-composite-preparation PROPERTIES TIMEOUT 45)
add_test(NAME p9-composite-step COMMAND p9-composite-preparation --step
  ${CMAKE_CURRENT_BINARY_DIR}/composite-step-fixture)
set_tests_properties(p9-composite-step PROPERTIES TIMEOUT 180)
add_test(NAME p9-composite-step-batch COMMAND p9-composite-preparation --step-batch
  ${CMAKE_CURRENT_BINARY_DIR}/composite-step-batch-fixture)
set_tests_properties(p9-composite-step-batch PROPERTIES TIMEOUT 180)
add_test(NAME p9-composite-progress COMMAND p9-composite-preparation --progress
  ${CMAKE_CURRENT_BINARY_DIR}/composite-progress-fixture)
add_test(NAME p9-composite-step-progress COMMAND p9-composite-preparation --step-progress
  ${CMAKE_CURRENT_BINARY_DIR}/composite-step-progress-fixture)
set_tests_properties(p9-composite-progress p9-composite-step-progress PROPERTIES TIMEOUT 180)
add_test(NAME p9-compile-attribution COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/verify-compile-attribution.py $<TARGET_FILE:p9-composite-preparation>
  ${CMAKE_CURRENT_BINARY_DIR}/compile-attribution-fixture $<TARGET_FILE:p9-composite-progress-policy>)
set_tests_properties(p9-compile-attribution PROPERTIES TIMEOUT 45)
add_executable(p9-draw-phases draw-phase-tests.cpp ${ROOT}/components/terrain/terraindrawable.cpp
  ${ROOT}/components/terrain/compositemaprenderer.cpp ${ROOT}/components/resource/openmwcompileoperation.cpp)
if(NOT MSVC)
  target_compile_options(p9-draw-phases PRIVATE -Wno-error=unused-parameter)
endif()
target_link_libraries(p9-draw-phases PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL Threads::Threads)
target_link_libraries(p9-draw-phases PRIVATE p9-texture-runtime)
target_sources(p9-draw-phases PRIVATE ${ROOT}/components/debug/debuglog.cpp
  ${ROOT}/components/files/conversion.cpp)
add_test(NAME p9-draw-phases COMMAND p9-draw-phases)
set_tests_properties(p9-draw-phases PROPERTIES TIMEOUT 30
  ENVIRONMENT "OPENMW_P9_LEAF_TRACE_FILE=${CMAKE_CURRENT_BINARY_DIR}/draw-phase-test.csv")

add_test(NAME p9-draw-phases-threaded COMMAND p9-draw-phases --threaded)
set_tests_properties(p9-draw-phases-threaded PROPERTIES TIMEOUT 30
  ENVIRONMENT "OPENMW_P9_LEAF_TRACE_FILE=${CMAKE_CURRENT_BINARY_DIR}/draw-phase-threaded-test.csv")
add_executable(p9-resource-catalog resource-catalog-tests.cpp)
target_link_libraries(p9-resource-catalog PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL Threads::Threads)
add_test(NAME p9-resource-catalog COMMAND p9-resource-catalog --expect-legacy)
set_tests_properties(p9-resource-catalog PROPERTIES TIMEOUT 30
  ENVIRONMENT "OPENMW_P9_LEAF_TRACE_FILE=${CMAKE_CURRENT_BINARY_DIR}/resource-catalog-test.csv;OPENMW_P9_CATALOG_OPTIMIZED=0")
add_test(NAME p9-resource-catalog-optimized COMMAND p9-resource-catalog --expect-optimized)
set_tests_properties(p9-resource-catalog-optimized PROPERTIES TIMEOUT 30
  ENVIRONMENT "OPENMW_P9_LEAF_TRACE_FILE=${CMAKE_CURRENT_BINARY_DIR}/resource-catalog-optimized-test.csv;OPENMW_P9_CATALOG_OPTIMIZED=1")
add_test(NAME p9-resource-catalog-family-off COMMAND p9-resource-catalog --capture-off)
set_tests_properties(p9-resource-catalog-family-off PROPERTIES TIMEOUT 30
  ENVIRONMENT "OPENMW_P9_LEAF_TRACE_FILE=;OPENMW_P9_CATALOG_OPTIMIZED=1")
add_test(NAME p9-resource-catalog-invalid-switch COMMAND p9-resource-catalog --expect-legacy)
set_tests_properties(p9-resource-catalog-invalid-switch PROPERTIES TIMEOUT 30
  ENVIRONMENT "OPENMW_P9_LEAF_TRACE_FILE=${CMAKE_CURRENT_BINARY_DIR}/resource-catalog-invalid-switch-test.csv;OPENMW_P9_CATALOG_OPTIMIZED=true")

option(P9_REQUIRE_GAME_SDK "Compile the real camera/PostProcessor/rig/morph integration translation units" OFF)
if(P9_REQUIRE_GAME_SDK)
  find_path(P9_SDL3_INCLUDE SDL3/SDL.h PATHS ENV CPLUS_INCLUDE_PATH REQUIRED)
  find_path(P9_MYGUI_INCLUDE MyGUI_Colour.h PATH_SUFFIXES MYGUI REQUIRED)
  find_path(P9_BULLET_INCLUDE btBulletCollisionCommon.h PATH_SUFFIXES bullet REQUIRED)
  find_path(P9_YAML_INCLUDE yaml-cpp/yaml.h REQUIRED)
  find_path(P9_DETOUR_INCLUDE DetourAlloc.h PATH_SUFFIXES recastnavigation REQUIRED)
  find_path(P9_RECAST_INCLUDE Recast.h PATH_SUFFIXES recastnavigation REQUIRED)
  add_library(p9-game-integration OBJECT
    ${ROOT}/apps/openmw/mwrender/postprocessor.cpp
    ${ROOT}/apps/openmw/mwrender/camera.cpp
    ${ROOT}/apps/openmw/mwrender/pingpongcanvas.cpp
    ${ROOT}/apps/openmw/mwrender/luminancecalculator.cpp
    ${ROOT}/apps/openmw/mwrender/pingpongcull.cpp
    ${ROOT}/apps/openmw/mwrender/transparentpass.cpp
    ${ROOT}/apps/openmw/mwrender/distortion.cpp
    ${ROOT}/apps/openmw/mwrender/npcanimation.cpp
    ${ROOT}/components/terrain/compositemaprenderer.cpp
    ${ROOT}/components/terrain/terraindrawable.cpp
    ${ROOT}/components/terrain/world.cpp
    ${ROOT}/components/terrain/chunkmanager.cpp
    ${ROOT}/components/resource/openmwcompileoperation.cpp
    ${ROOT}/components/resource/scenemanager.cpp
    ${ROOT}/components/fx/stateupdater.cpp
    ${ROOT}/apps/openmw/mwrender/renderingmanager.cpp
    ${ROOT}/components/sceneutil/riggeometry.cpp
    ${ROOT}/components/sceneutil/morphgeometry.cpp)
  target_include_directories(p9-game-integration SYSTEM PRIVATE
    ${P9_SDL3_INCLUDE} ${P9_MYGUI_INCLUDE} ${P9_BULLET_INCLUDE} ${P9_YAML_INCLUDE} ${P9_DETOUR_INCLUDE} ${P9_RECAST_INCLUDE})
  target_include_directories(p9-game-integration PRIVATE ${ROOT}/extern/sol_config ${ROOT}/extern/sol3)
  target_link_libraries(p9-game-integration PRIVATE glm::glm ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL)
  # Retained production headers have existing warning classes; the authored
  # standalone Phase 9 implementation/tests retain -Werror above.
  if(NOT MSVC)
    target_compile_options(p9-game-integration PRIVATE -Wno-error)
  endif()
endif()

add_executable(p9-temporal-dynamic temporal-dynamic-tests.cpp
  ${ROOT}/components/sceneutil/riggeometry.cpp ${ROOT}/components/sceneutil/morphgeometry.cpp
  ${ROOT}/components/sceneutil/skeleton.cpp ${ROOT}/components/debug/debuglog.cpp
  ${ROOT}/components/files/conversion.cpp)
add_executable(p9-frozen-rig-deformation frozen-rig-deformation-tests.cpp
  ${ROOT}/components/sceneutil/riggeometry.cpp ${ROOT}/components/sceneutil/skeleton.cpp
  ${ROOT}/components/debug/debuglog.cpp ${ROOT}/components/files/conversion.cpp)
target_link_libraries(p9-frozen-rig-deformation PRIVATE glm::glm ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL Threads::Threads)
add_test(NAME p9-frozen-rig-deformation COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/verify-frozen-rig-deformation.py $<TARGET_FILE:p9-frozen-rig-deformation>)
set_tests_properties(p9-frozen-rig-deformation PROPERTIES TIMEOUT 90)
target_include_directories(p9-temporal-dynamic SYSTEM PRIVATE
  ${P9_SDL3_INCLUDE} ${P9_MYGUI_INCLUDE} ${P9_BULLET_INCLUDE} ${P9_YAML_INCLUDE} ${P9_DETOUR_INCLUDE} ${P9_RECAST_INCLUDE})
target_link_libraries(p9-temporal-dynamic PRIVATE p9-temporal-production glm::glm ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL Threads::Threads)
target_compile_definitions(p9-temporal-dynamic PRIVATE
  P9_VERTEX="${ROOT}/files/shaders/compatibility/temporal_camera_motion.vert"
  P9_FRAGMENT="${ROOT}/files/shaders/compatibility/temporal_camera_motion.frag"
  P9_DYNAMIC_VERTEX="${ROOT}/files/shaders/compatibility/temporal_dynamic_motion.vert"
  P9_DYNAMIC_FRAGMENT="${ROOT}/files/shaders/compatibility/temporal_dynamic_motion.frag")
if(NOT MSVC)
  target_compile_options(p9-temporal-dynamic PRIVATE -Wno-error=unused-parameter)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # Retained recordEvent copies all three text fields using non-short-circuit |.
  # Preserve its behavior and visible warning in the actual rig fixture.
  target_compile_options(p9-temporal-dynamic PRIVATE -Wno-error=bitwise-instead-of-logical)
endif()
add_test(NAME p9-temporal-dynamic COMMAND p9-temporal-dynamic)
set_tests_properties(p9-temporal-dynamic PROPERTIES TIMEOUT 45 ENVIRONMENT "OPENMW_P9_DYNAMIC_MOTION=1")
add_test(NAME p9-temporal-dynamic-indexed COMMAND p9-temporal-dynamic)
set_tests_properties(p9-temporal-dynamic-indexed PROPERTIES TIMEOUT 45
  ENVIRONMENT "OPENMW_P9_DYNAMIC_MOTION=1;OPENMW_TEMPORAL_HISTORY_LOOKUP_FIXTURE=1")
add_executable(p9-nis-owner nis-owner-tests.cpp
  ${ROOT}/apps/openmw/mwrender/nisscaler.cpp ${ROOT}/components/debug/debuglog.cpp
  ${ROOT}/components/files/conversion.cpp)
target_link_libraries(p9-nis-owner PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL Threads::Threads)
add_test(NAME p9-nis-owner COMMAND p9-nis-owner)
set_tests_properties(p9-nis-owner PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77)

add_test(NAME p9-production-startup COMMAND p9-draw-phases --startup)
set_tests_properties(p9-production-startup PROPERTIES TIMEOUT 30
  ENVIRONMENT "OPENMW_P9_LEAF_TRACE_FILE=${CMAKE_CURRENT_BINARY_DIR}/draw-production-startup.csv")
add_library(p9-rootcause-header OBJECT rootcause-header-check.cpp)
target_link_libraries(p9-rootcause-header PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL)
add_executable(p9-gl-hook-contract gl-hook-contract-tests.cpp)
target_link_libraries(p9-gl-hook-contract PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL)
add_test(NAME p9-gl-hook-contract COMMAND p9-gl-hook-contract ${CMAKE_CURRENT_BINARY_DIR}/gl-hook-contract)
set_tests_properties(p9-gl-hook-contract PROPERTIES TIMEOUT 30)

if(NOT MSVC)
  # These production diagnostic/deformation headers retain u8path for their
  # cross-platform UTF-8 contract. GCC 13 deprecates that C++17 helper in C++20;
  # preserve the warning without changing legacy Windows path behavior here.
  foreach(target p9-capture-transport p9-gpu-owner p9-composite-preparation p9-draw-phases p9-temporal-dynamic
    p9-benchmark-context p9-animation-preparation p9-cloud-image-preparation)
    target_compile_options(${target} PRIVATE -Wno-error=deprecated-declarations)
  endforeach()
endif()

add_test(NAME p9-production-cull-threaded COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/verify-cull-timeline.py $<TARGET_FILE:p9-draw-phases>)
set_tests_properties(p9-production-cull-threaded PROPERTIES TIMEOUT 45
  ENVIRONMENT "OPENMW_P9_LEAF_TRACE_FILE=${CMAKE_CURRENT_BINARY_DIR}/draw-production-cull.csv;OPENMW_P9_HITCH_SPANS_FILE=${CMAKE_CURRENT_BINARY_DIR}/draw-production-cull-spans.csv")

cmake_minimum_required(VERSION 3.20)
project(OptimizedMWPhase9 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
find_package(OpenSceneGraph 3.6 REQUIRED COMPONENTS osg osgUtil osgViewer)
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
enable_testing()
find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
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
option(OPENMW_PHASE9_INTEROP_FIXTURE "Build the separate GL/Vulkan shared-image fixture" OFF)
if(OPENMW_PHASE9_INTEROP_FIXTURE)
  add_subdirectory(interop)
endif()
add_executable(p9-dynamic-stream dynamic-stream-tests.cpp)
target_link_libraries(p9-dynamic-stream PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL)
add_test(NAME p9-dynamic-stream COMMAND p9-dynamic-stream)
set_tests_properties(p9-dynamic-stream PROPERTIES TIMEOUT 30)
add_library(p9-temporal-production STATIC "${ROOT}/apps/openmw/mwrender/temporalmotion.cpp")
target_link_libraries(p9-temporal-production PRIVATE glm::glm ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL)
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
add_test(NAME p9-composite-preparation COMMAND p9-composite-preparation)
set_tests_properties(p9-composite-preparation PROPERTIES TIMEOUT 45)
add_executable(p9-draw-phases draw-phase-tests.cpp ${ROOT}/components/terrain/terraindrawable.cpp
  ${ROOT}/components/terrain/compositemaprenderer.cpp ${ROOT}/components/resource/openmwcompileoperation.cpp)
if(NOT MSVC)
  target_compile_options(p9-draw-phases PRIVATE -Wno-error=unused-parameter)
endif()
target_link_libraries(p9-draw-phases PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL Threads::Threads)
add_test(NAME p9-draw-phases COMMAND p9-draw-phases)
set_tests_properties(p9-draw-phases PROPERTIES TIMEOUT 30
  ENVIRONMENT "OPENMW_P9_LEAF_TRACE_FILE=${CMAKE_CURRENT_BINARY_DIR}/draw-phase-test.csv")

add_test(NAME p9-draw-phases-threaded COMMAND p9-draw-phases --threaded)
set_tests_properties(p9-draw-phases-threaded PROPERTIES TIMEOUT 30
  ENVIRONMENT "OPENMW_P9_LEAF_TRACE_FILE=${CMAKE_CURRENT_BINARY_DIR}/draw-phase-threaded-test.csv")
add_executable(p9-resource-catalog resource-catalog-tests.cpp)
target_link_libraries(p9-resource-catalog PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL Threads::Threads)
add_test(NAME p9-resource-catalog COMMAND p9-resource-catalog)
set_tests_properties(p9-resource-catalog PROPERTIES TIMEOUT 30
  ENVIRONMENT "OPENMW_P9_LEAF_TRACE_FILE=${CMAKE_CURRENT_BINARY_DIR}/resource-catalog-test.csv")

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
  foreach(target p9-capture-transport p9-gpu-owner p9-composite-preparation p9-draw-phases p9-temporal-dynamic)
    target_compile_options(${target} PRIVATE -Wno-error=deprecated-declarations)
  endforeach()
endif()

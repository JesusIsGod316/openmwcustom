cmake_minimum_required(VERSION 3.20)
project(OptimizedMWPhase9 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
find_package(OpenSceneGraph 3.6 REQUIRED COMPONENTS osg osgUtil osgViewer)
find_package(OpenGL REQUIRED)
find_package(glm CONFIG REQUIRED)
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
add_executable(p9-draw-phases draw-phase-tests.cpp)
target_link_libraries(p9-draw-phases PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL)
add_test(NAME p9-draw-phases COMMAND p9-draw-phases)
set_tests_properties(p9-draw-phases PROPERTIES TIMEOUT 30
  ENVIRONMENT "OPENMW_P9_LEAF_TRACE_FILE=${CMAKE_CURRENT_BINARY_DIR}/draw-phase-test.csv")

add_test(NAME p9-draw-phases-threaded COMMAND p9-draw-phases --threaded)
set_tests_properties(p9-draw-phases-threaded PROPERTIES TIMEOUT 30
  ENVIRONMENT "OPENMW_P9_LEAF_TRACE_FILE=${CMAKE_CURRENT_BINARY_DIR}/draw-phase-threaded-test.csv")

option(P9_REQUIRE_GAME_SDK "Compile the real camera/PostProcessor/rig/morph integration translation units" OFF)
if(P9_REQUIRE_GAME_SDK)
  find_path(P9_SDL3_INCLUDE SDL3/SDL.h PATHS ENV CPLUS_INCLUDE_PATH REQUIRED)
  find_path(P9_MYGUI_INCLUDE MyGUI_Colour.h PATH_SUFFIXES MYGUI REQUIRED)
  find_path(P9_BULLET_INCLUDE btBulletCollisionCommon.h PATH_SUFFIXES bullet REQUIRED)
  add_library(p9-game-integration OBJECT
    ${ROOT}/apps/openmw/mwrender/postprocessor.cpp
    ${ROOT}/apps/openmw/mwrender/camera.cpp
    ${ROOT}/apps/openmw/mwrender/pingpongcanvas.cpp
    ${ROOT}/apps/openmw/mwrender/pingpongcull.cpp
    ${ROOT}/components/sceneutil/riggeometry.cpp
    ${ROOT}/components/sceneutil/morphgeometry.cpp)
  target_include_directories(p9-game-integration SYSTEM PRIVATE
    ${P9_SDL3_INCLUDE} ${P9_MYGUI_INCLUDE} ${P9_BULLET_INCLUDE})
  target_include_directories(p9-game-integration PRIVATE ${ROOT}/extern/sol_config ${ROOT}/extern/sol3)
  target_link_libraries(p9-game-integration PRIVATE glm::glm ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL)
  # Retained production headers have existing warning classes; the authored
  # standalone Phase 9 implementation/tests retain -Werror above.
  if(NOT MSVC)
    target_compile_options(p9-game-integration PRIVATE -Wno-error)
  endif()
endif()

cmake_minimum_required(VERSION 3.20)
project(OptimizedMWP8U1 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(P8U1_SANITIZERS "Address/undefined sanitizers" OFF)
option(P8U1_REQUIRE_GAME_SDK "Require production LuaJIT/sol and FFmpeg integration tests" OFF)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
find_package(OpenSceneGraph 3.6 REQUIRED COMPONENTS osg osgDB osgUtil osgViewer)
find_package(Threads REQUIRED)
find_package(OpenGL REQUIRED)
enable_testing()
if(MSVC)
  add_compile_options(/W4 /EHsc)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()
if(P8U1_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
include_directories(${ROOT} ${OPENSCENEGRAPH_INCLUDE_DIRS})
add_executable(p8u1-cache objectcache-tests.cpp)
target_link_libraries(p8u1-cache PRIVATE ${OPENSCENEGRAPH_LIBRARIES} Threads::Threads)
add_test(NAME p8u1-cache COMMAND p8u1-cache)
add_executable(p8u1-composite composite-tests.cpp ${ROOT}/components/terrain/compositemaprenderer.cpp
  ${ROOT}/components/resource/openmwcompileoperation.cpp)
target_link_libraries(p8u1-composite PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL Threads::Threads)
add_test(NAME p8u1-composite COMMAND p8u1-composite)
add_executable(p8u1-preparedtexture preparedtexture-tests.cpp)
target_link_libraries(p8u1-preparedtexture PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL Threads::Threads)
add_test(NAME p8u1-preparedtexture COMMAND p8u1-preparedtexture)
find_path(P8U1_LUAJIT_INCLUDE lua.h PATH_SUFFIXES luajit-2.1 luajit-2.0)
find_library(P8U1_LUAJIT_LIBRARY NAMES luajit-5.1 lua51)
if(P8U1_LUAJIT_INCLUDE AND P8U1_LUAJIT_LIBRARY)
  add_executable(p8u1-lua lua-tests.cpp)
  target_include_directories(p8u1-lua PRIVATE ${P8U1_LUAJIT_INCLUDE})
  target_link_libraries(p8u1-lua PRIVATE ${P8U1_LUAJIT_LIBRARY})
  find_path(P8U1_MYGUI_INCLUDE MyGUI_Colour.h PATH_SUFFIXES MYGUI REQUIRED)
  add_executable(p8u1-sol-object sol-object-tests.cpp ${ROOT}/apps/openmw/mwlua/objectcache.cpp)
  target_include_directories(p8u1-sol-object PRIVATE ${P8U1_LUAJIT_INCLUDE} ${P8U1_MYGUI_INCLUDE}
      ${ROOT}/extern/sol_config ${ROOT}/extern/sol3)
  target_link_libraries(p8u1-sol-object PRIVATE ${P8U1_LUAJIT_LIBRARY} ${OPENSCENEGRAPH_LIBRARIES})
  add_test(NAME p8u1-sol-object COMMAND p8u1-sol-object)
elseif(P8U1_REQUIRE_GAME_SDK)
  message(FATAL_ERROR "LuaJIT development SDK is required; do not substitute the minimal ABI fixture")
else()
  # Explicit local fallback only; never sufficient for publication gate.
  find_library(P8U1_LUA54_RUNTIME NAMES lua5.4 liblua5.4.so.0 REQUIRED)
  add_executable(p8u1-lua lua-tests.cpp)
  target_compile_definitions(p8u1-lua PRIVATE P8U1_MINIMAL_LUA54=1)
  target_include_directories(p8u1-lua PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lua54-runtime-abi)
  target_link_libraries(p8u1-lua PRIVATE ${P8U1_LUA54_RUNTIME})
  message(STATUS "Local fallback only: real LuaJIT/sol integration remains required in CI")
endif()
add_test(NAME p8u1-lua COMMAND p8u1-lua)
set_tests_properties(p8u1-lua PROPERTIES TIMEOUT 20)
add_executable(p8u1-warmqueue warmqueue-tests.cpp ${ROOT}/apps/openmw/mwsound/warmqueue.cpp)
target_link_libraries(p8u1-warmqueue PRIVATE Threads::Threads)
add_test(NAME p8u1-warmqueue COMMAND p8u1-warmqueue)
add_executable(p8u1-headcache headcache-tests.cpp ${ROOT}/apps/openmw/mwsound/headcache.cpp ${ROOT}/components/vfs/manager.cpp)
target_link_libraries(p8u1-headcache PRIVATE Threads::Threads)
add_test(NAME p8u1-headcache COMMAND p8u1-headcache)
set_tests_properties(p8u1-cache p8u1-composite p8u1-preparedtexture p8u1-warmqueue p8u1-headcache PROPERTIES TIMEOUT 20)

add_executable(p8u1-shadow-settings shadow-settings-tests.cpp)
target_link_libraries(p8u1-shadow-settings PRIVATE ${OPENSCENEGRAPH_LIBRARIES})
add_test(NAME p8u1-shadow-settings COMMAND p8u1-shadow-settings)
if(P8U1_REQUIRE_GAME_SDK)
  find_package(PkgConfig REQUIRED)
  find_package(OpenAL REQUIRED)
  pkg_check_modules(P8U1_AV REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libswresample)
  add_executable(p8u1-ffmpeg ffmpeg-tests.cpp
      ${ROOT}/apps/openmw/mwsound/ffmpegdecoder.cpp
      ${ROOT}/apps/openmw/mwsound/sounddecoder.cpp
      ${ROOT}/apps/openmw/mwsound/headcache.cpp
      ${ROOT}/apps/openmw/mwsound/warmqueue.cpp
      ${ROOT}/components/vfs/manager.cpp
      ${ROOT}/components/debug/debuglog.cpp
      ${ROOT}/components/files/conversion.cpp)
  target_include_directories(p8u1-ffmpeg PRIVATE ${ROOT}/extern/osg-ffmpeg-videoplayer/include)
  target_link_libraries(p8u1-ffmpeg PRIVATE PkgConfig::P8U1_AV Threads::Threads)
  add_test(NAME p8u1-ffmpeg COMMAND p8u1-ffmpeg)
  set_tests_properties(p8u1-ffmpeg p8u1-sol-object PROPERTIES TIMEOUT 30)
  # Compile the real integration translation units; the complete Windows link
  # below remains mandatory, this is not a mocked engine.
  add_library(p8u1-production OBJECT
      ${ROOT}/components/resource/openmwcompileoperation.cpp
      ${ROOT}/components/terrain/chunkmanager.cpp
      ${ROOT}/components/terrain/texturemanager.cpp
      ${ROOT}/components/terrain/world.cpp
      ${ROOT}/components/sceneutil/shadow.cpp
      ${ROOT}/components/sceneutil/mwshadowtechnique.cpp
      ${ROOT}/apps/openmw/mwsound/sfxpredecodecache.cpp
      ${ROOT}/apps/openmw/mwsound/openaloutput.cpp
      ${ROOT}/apps/openmw/mwsound/soundmanagerimp.cpp)
  target_include_directories(p8u1-production PRIVATE ${P8U1_MYGUI_INCLUDE} ${P8U1_LUAJIT_INCLUDE}
      ${ROOT}/extern/sol_config ${ROOT}/extern/sol3
      ${ROOT}/extern/osg-ffmpeg-videoplayer/include /usr/include/bullet)
  target_link_libraries(p8u1-production PRIVATE PkgConfig::P8U1_AV OpenAL::OpenAL)
endif()

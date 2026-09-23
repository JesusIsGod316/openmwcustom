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
find_package(OpenSceneGraph 3.6 REQUIRED COMPONENTS osg osgDB osgUtil)
find_path(CP4F_GLM_INCLUDE_DIR glm/glm.hpp HINTS "${CP4F_RENDER_SDK}/sysroot/usr/include" "${CP4F_OSG_ROOT}/include" REQUIRED)
if(NOT EXISTS "${CP4F_MYGUI_INCLUDE_DIR}/MyGUI.h")
    message(FATAL_ERROR "Set CP4F_MYGUI_INCLUDE_DIR to the pinned MyGUIEngine/include directory")
endif()
add_library(rendering-source STATIC
    "${OPENMW_ROOT}/components/debug/debuglog.cpp"
    "${OPENMW_ROOT}/components/render/backend/vsg/legacymaterialshader.cpp"
    "${OPENMW_ROOT}/components/render/backend/vsg/openmwviewdependentstate.cpp"
    "${OPENMW_ROOT}/components/render/backend/vsg/offscreenrendertarget.cpp"
    "${OPENMW_ROOT}/components/render/backend/vsg/staticassetconformance.cpp"
    "${OPENMW_ROOT}/components/render/backend/vsg/staticassetrealizer.cpp"
    "${OPENMW_ROOT}/components/render/backend/vsg/statictexturedecode.cpp"
    "${OPENMW_ROOT}/components/render/backend/vsg/uipipeline.cpp"
    "${OPENMW_ROOT}/components/render/backend/vsg/watersurface.cpp"
    "${OPENMW_ROOT}/components/render/backend/vsg/skybackdrop.cpp"
    "${OPENMW_ROOT}/components/render/backend/vsg/nativesky.cpp")
target_include_directories(rendering-source PUBLIC "${OPENMW_ROOT}" "${CP4F_GLM_INCLUDE_DIR}")
target_link_libraries(rendering-source PUBLIC rendering-deps Threads::Threads ${CMAKE_DL_LIBS})
add_executable(rendering-pixels "${OPENMW_ROOT}/tools/v4/cp4/rendering-pixel-tests.cpp")
target_link_libraries(rendering-pixels PRIVATE rendering-source)
add_executable(rendering-capture
    "${OPENMW_ROOT}/tools/v4/cp4/rendering-capture-tests.cpp"
    "${OPENMW_ROOT}/apps/openmw/mwrender/v4skycapture.cpp"
    "${OPENMW_ROOT}/components/debug/debuglog.cpp"
    "${OPENMW_ROOT}/components/vfs/manager.cpp"
    "${OPENMW_ROOT}/components/misc/resourcehelpers.cpp"
    "${OPENMW_ROOT}/components/files/hash.cpp"
    "${OPENMW_ROOT}/extern/smhasher/MurmurHash3.cpp"
    "${OPENMW_ROOT}/components/sceneutil/util.cpp"
    "${OPENMW_ROOT}/components/sceneutil/material.cpp")
target_include_directories(rendering-capture PRIVATE "${OPENMW_ROOT}" "${OPENMW_ROOT}/extern"
    "${OPENMW_ROOT}/extern/smhasher/include" "${OPENMW_ROOT}/extern/smhasher/include/smhasher")
target_include_directories(rendering-capture SYSTEM PRIVATE "${CP4F_GLM_INCLUDE_DIR}" "${CP4F_MYGUI_INCLUDE_DIR}" ${OPENSCENEGRAPH_INCLUDE_DIRS})
target_link_libraries(rendering-capture PRIVATE ${OPENSCENEGRAPH_LIBRARIES} Threads::Threads)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(target rendering-source rendering-capture)
        target_compile_options(${target} PRIVATE -ffunction-sections -fdata-sections)
    endforeach()
    foreach(target rendering-pixels rendering-capture)
        target_link_options(${target} PRIVATE -Wl,--gc-sections)
    endforeach()
    if(CP4F_CAPTURE_SANITIZERS)
        target_compile_options(rendering-capture PRIVATE -O0 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(rendering-capture PRIVATE -fsanitize=address,undefined)
        # util.cpp contains unrelated unused renderer methods whose UBSan vptr
        # metadata refers to a full-engine SceneManager vtable. The actual new
        # capture translation units keep all UBSan checks, including vptr.
        set_source_files_properties("${OPENMW_ROOT}/components/sceneutil/util.cpp"
            PROPERTIES COMPILE_OPTIONS "-fno-sanitize=vptr")
    endif()
endif()
add_executable(rendering-uniforms "${OPENMW_ROOT}/tools/v4/cp4/rendering-uniform-tests.cpp")
target_link_libraries(rendering-uniforms PRIVATE rendering-source)
enable_testing()
add_test(NAME rendering-uniforms COMMAND rendering-uniforms)
add_test(NAME rendering-capture COMMAND rendering-capture)
# Pixel execution is explicit: missing Vulkan hardware is an error, never a PASS.

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

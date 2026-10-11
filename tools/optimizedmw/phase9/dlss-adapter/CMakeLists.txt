cmake_minimum_required(VERSION 3.20)
project(OptimizedMWDlssAdapterFixture LANGUAGES CXX)

# Deliberately standalone. Neither the GPL game nor its package links NGX.
# The locally supplied official SDK retains its own license and binaries.
if(NOT WIN32)
    message(FATAL_ERROR "This OpenGL/D3D11 adapter fixture requires Windows")
endif()
set(P9_NGX_SDK_ROOT "" CACHE PATH "Locally supplied, pinned official NVIDIA DLSS SDK")
if(NOT EXISTS "${P9_NGX_SDK_ROOT}/include/nvsdk_ngx_helpers.h"
    OR NOT EXISTS "${P9_NGX_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
    message(FATAL_ERROR "Supply P9_NGX_SDK_ROOT; no automatic download or game integration")
endif()
add_executable(p9-dlss-d3d11-adapter dlss-d3d11-adapter.cpp)
add_executable(p9-dlss-synchronous-lease-tests synchronous-lease-tests.cpp)
target_compile_features(p9-dlss-synchronous-lease-tests PRIVATE cxx_std_20)
target_compile_features(p9-dlss-d3d11-adapter PRIVATE cxx_std_20)
target_compile_definitions(p9-dlss-d3d11-adapter PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(p9-dlss-d3d11-adapter SYSTEM PRIVATE "${P9_NGX_SDK_ROOT}/include")
target_link_libraries(p9-dlss-d3d11-adapter PRIVATE
    "${P9_NGX_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib"
    d3d11 dxgi opengl32 user32 gdi32)
set_property(TARGET p9-dlss-d3d11-adapter PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Optional import-only proof against the already installed, pinned OSG runtime.
# No dependency download and no game renderer integration or NGX evaluation.
set(P9_OSG_PREFIX "" CACHE PATH "Existing pinned OSG 3.6.5 prefix for native format proof")
if(P9_OSG_PREFIX)
    list(PREPEND CMAKE_PREFIX_PATH "${P9_OSG_PREFIX}")
    find_package(OpenSceneGraph 3.6.5 EXACT REQUIRED COMPONENTS osg)
    add_executable(p9-dlss-production-format production-format-tests.cpp)
    target_compile_features(p9-dlss-production-format PRIVATE cxx_std_20)
    target_compile_definitions(p9-dlss-production-format PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_include_directories(p9-dlss-production-format SYSTEM PRIVATE ${OPENSCENEGRAPH_INCLUDE_DIRS})
    target_link_libraries(p9-dlss-production-format PRIVATE ${OPENSCENEGRAPH_LIBRARIES}
        d3d11 dxgi opengl32 user32 gdi32)
    set_property(TARGET p9-dlss-production-format PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# The signed runtime is supplied beside this separate executable by its audited
# local runner, never installed into OptimizedMW or bundled with this source.

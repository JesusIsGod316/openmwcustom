# CP3B3-only CMake injection for building the real-NIF Vulkan conformance tool
# against OpenMW's authoritative parser/VFS/components target without changing
# the production OpenMW target graph. Configure the repository root with:
#   -DCMAKE_PROJECT_INCLUDE=<repo>/tools/v4/cp3b3/OpenMWRealNifTool.cmake
# and make the VSG/vsgXchange package prefix visible through CMAKE_PREFIX_PATH.

if(NOT PROJECT_NAME STREQUAL "OpenMW")
    return()
endif()

get_property(_openmw_cp3b3_real_nif_tool_scheduled GLOBAL PROPERTY OPENMW_CP3B3_REAL_NIF_TOOL_SCHEDULED)
if(_openmw_cp3b3_real_nif_tool_scheduled)
    return()
endif()
set_property(GLOBAL PROPERTY OPENMW_CP3B3_REAL_NIF_TOOL_SCHEDULED TRUE)

function(openmw_cp3b3_define_real_nif_tool)
    if(TARGET openmw-vulkan-nif-conformance)
        return()
    endif()
    if(NOT TARGET components)
        message(FATAL_ERROR "CP3B3 real-NIF injection requires the OpenMW components target")
    endif()
    if(NOT TARGET SDL3::SDL3)
        message(FATAL_ERROR "CP3B3 real-NIF injection requires OpenMW's pinned SDL3 target")
    endif()

    find_package(glm CONFIG REQUIRED)
    find_package(Vulkan REQUIRED)
    find_package(vsg 1.1.15 CONFIG REQUIRED)
    find_package(vsgXchange CONFIG REQUIRED)

    include("${CMAKE_SOURCE_DIR}/components/render/backend/vsg/runtime-sources.cmake")
    include("${CMAKE_SOURCE_DIR}/apps/openmw/mwrender/v4engine-sources.cmake")

    # Keep GLM/VSG semantics out of the legacy OpenGL target graph until the
    # production Vulkan engine path is explicitly enabled. Compile the real
    # game-state adapter here so its OpenMW-facing surface remains checked by
    # the Vulkan integration job without adding GLM to OpenGL-only builds.
    add_library(openmw-v4-semantic-source-compile OBJECT ${OPENMW_V4_ENGINE_RUNTIME_SOURCES})
    target_include_directories(openmw-v4-semantic-source-compile PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(
        openmw-v4-semantic-source-compile PRIVATE components SDL3::SDL3 glm::glm Vulkan::Vulkan vsg::vsg)
    target_compile_features(openmw-v4-semantic-source-compile PRIVATE cxx_std_20)

    add_executable(openmw-vulkan-nif-conformance
        "${CMAKE_SOURCE_DIR}/tools/v4/cp3b3/nif-conformance.cpp"
        "${CMAKE_SOURCE_DIR}/components/render/backend/vsg/staticnifconformance.cpp"
        ${OPENMW_V4_VSG_RUNTIME_SOURCES})

    target_include_directories(openmw-vulkan-nif-conformance PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(openmw-vulkan-nif-conformance PRIVATE
        components
        SDL3::SDL3
        glm::glm
        Vulkan::Vulkan
        vsg::vsg
        vsgXchange::vsgXchange)
    target_compile_features(openmw-vulkan-nif-conformance PRIVATE cxx_std_20)
    add_dependencies(openmw-vulkan-nif-conformance openmw-v4-semantic-source-compile)

    # This integration target intentionally recompiles existing OpenMW translator
    # sources in addition to CP3B3-owned code. Keep the normal high warning level
    # and conformance flags, but do not make inherited production warnings fatal.
    # The isolated CP3B3 backend targets remain warning-as-error gated.
    if(MSVC)
        target_compile_options(openmw-v4-semantic-source-compile PRIVATE /W4 /permissive-)
        target_compile_options(openmw-vulkan-nif-conformance PRIVATE /W4 /permissive-)
    else()
        target_compile_options(openmw-v4-semantic-source-compile PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(openmw-vulkan-nif-conformance PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# CMAKE_PROJECT_INCLUDE runs during project(). Defer target creation until the
# end of OpenMW's top-level directory so components and SDL3::SDL3 already exist.
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL openmw_cp3b3_define_real_nif_tool)

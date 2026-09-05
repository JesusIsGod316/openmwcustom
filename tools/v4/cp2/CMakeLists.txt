cmake_minimum_required(VERSION 3.21)

project(OpenMWV4CP2VulkanFoundation LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "V4 CP2 foundation is currently a Windows validation target")
endif()

get_filename_component(OPENMW_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

# Keep CP1B's exact SDL3 identity rather than allowing the VSG dependency
# graph to choose the platform layer. SDL remains OpenMW's window/input owner.
set(OPENMW_SDL3_VERSION "3.4.10")
set(OPENMW_SDL3_VC_SHA256 "e2b336b10b037934af98308027410732ef7b22f2c6697d58092aa1c209fae7d7")
set(_openmw_sdl3_deps_dir "${CMAKE_BINARY_DIR}/_deps")
set(_openmw_sdl3_archive "${_openmw_sdl3_deps_dir}/SDL3-devel-${OPENMW_SDL3_VERSION}-VC.zip")
set(_openmw_sdl3_root "${_openmw_sdl3_deps_dir}/SDL3-${OPENMW_SDL3_VERSION}")
set(_openmw_sdl3_config "${_openmw_sdl3_root}/cmake/SDL3Config.cmake")

if(NOT EXISTS "${_openmw_sdl3_config}")
    file(MAKE_DIRECTORY "${_openmw_sdl3_deps_dir}")
    file(DOWNLOAD
        "https://github.com/libsdl-org/SDL/releases/download/release-${OPENMW_SDL3_VERSION}/SDL3-devel-${OPENMW_SDL3_VERSION}-VC.zip"
        "${_openmw_sdl3_archive}"
        EXPECTED_HASH "SHA256=${OPENMW_SDL3_VC_SHA256}"
        SHOW_PROGRESS
        STATUS _openmw_sdl3_download_status)
    list(GET _openmw_sdl3_download_status 0 _openmw_sdl3_download_code)
    list(GET _openmw_sdl3_download_status 1 _openmw_sdl3_download_message)
    if(NOT _openmw_sdl3_download_code EQUAL 0)
        message(FATAL_ERROR "SDL3 download failed: ${_openmw_sdl3_download_message}")
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xvf "${_openmw_sdl3_archive}"
        WORKING_DIRECTORY "${_openmw_sdl3_deps_dir}"
        RESULT_VARIABLE _openmw_sdl3_extract_result)
    if(NOT _openmw_sdl3_extract_result EQUAL 0 OR NOT EXISTS "${_openmw_sdl3_config}")
        message(FATAL_ERROR "Failed to extract pinned SDL3 ${OPENMW_SDL3_VERSION} development package")
    endif()
endif()

list(PREPEND CMAKE_PREFIX_PATH "${_openmw_sdl3_root}")

find_package(SDL3 3.4.10 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(Vulkan REQUIRED)
find_package(vsg 1.1.15 CONFIG REQUIRED)

add_library(openmw_vsg_foundation STATIC
    "${OPENMW_ROOT}/components/render/backend/vsg/residencyledger.cpp"
    "${OPENMW_ROOT}/components/render/backend/vsg/sdlvulkanwindow.cpp"
)
target_include_directories(openmw_vsg_foundation PUBLIC "${OPENMW_ROOT}")
target_link_libraries(openmw_vsg_foundation PUBLIC SDL3::SDL3 Vulkan::Vulkan vsg::vsg)
target_compile_definitions(openmw_vsg_foundation PUBLIC SDL_DISABLE_OLD_NAMES)

add_executable(openmw-vulkan-foundation main.cpp)
target_include_directories(openmw-vulkan-foundation PRIVATE "${OPENMW_ROOT}")
target_link_libraries(openmw-vulkan-foundation PRIVATE openmw_vsg_foundation glm::glm)

add_custom_command(TARGET openmw-vulkan-foundation POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${_openmw_sdl3_root}/lib/x64/SDL3.dll"
        "$<TARGET_FILE_DIR:openmw-vulkan-foundation>/SDL3.dll"
    VERBATIM)

add_executable(openmw-vulkan-foundation-tests
    foundation_tests.cpp
    "${OPENMW_ROOT}/components/render/backend/vsg/residencyledger.cpp"
)
target_include_directories(openmw-vulkan-foundation-tests PRIVATE "${OPENMW_ROOT}")
target_link_libraries(openmw-vulkan-foundation-tests PRIVATE glm::glm)

if(MSVC)
    target_compile_options(openmw_vsg_foundation PRIVATE /W4 /permissive-)
    target_compile_options(openmw-vulkan-foundation PRIVATE /W4 /permissive-)
    target_compile_options(openmw-vulkan-foundation-tests PRIVATE /W4 /permissive-)
endif()

enable_testing()
add_test(NAME v4_cp2_foundation_contract COMMAND openmw-vulkan-foundation-tests)

install(TARGETS openmw-vulkan-foundation RUNTIME DESTINATION .)
install(FILES "${_openmw_sdl3_root}/lib/x64/SDL3.dll" DESTINATION .)

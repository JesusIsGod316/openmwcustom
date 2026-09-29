cmake_minimum_required(VERSION 3.25)
project(VulkanMWP3RealGpuRegression LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(vsg 1.1.15 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(Threads REQUIRED)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
add_library(p3-real-source STATIC
    "${ROOT}/components/render/backend/vsg/gpupopulationcull.cpp"
    "${ROOT}/components/render/backend/vsg/staticassetconformance.cpp"
    "${ROOT}/components/render/backend/vsg/staticassetrealizer.cpp"
    "${ROOT}/components/render/backend/vsg/legacymaterialshader.cpp"
    "${ROOT}/components/render/backend/vsg/openmwviewdependentstate.cpp"
    "${ROOT}/components/render/backend/vsg/offscreenrendertarget.cpp"
    "${ROOT}/components/debug/debuglog.cpp"
    "${ROOT}/components/files/conversion.cpp")
target_include_directories(p3-real-source PUBLIC "${ROOT}")
target_link_libraries(p3-real-source PUBLIC vsg::vsg glm::glm Threads::Threads ${CMAKE_DL_LIBS})
add_executable(p3-real-gpu "${ROOT}/tools/v4/cp3b3/gpu-population-routing-smoke.cpp")
target_compile_definitions(p3-real-gpu PRIVATE P3_GPU_PIXEL_TESTS=1)
target_link_libraries(p3-real-gpu PRIVATE p3-real-source)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(target IN ITEMS p3-real-source p3-real-gpu)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror -ffunction-sections -fdata-sections)
    endforeach()
    target_link_options(p3-real-gpu PRIVATE -Wl,--gc-sections)
endif()
enable_testing()
foreach(inventory IN ITEMS none pipeline resource both)
    add_test(NAME p3-real-gpu-${inventory} COMMAND p3-real-gpu ${inventory} --gpu)
endforeach()

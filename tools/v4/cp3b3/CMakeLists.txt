cmake_minimum_required(VERSION 3.25)
project(OpenMWV4CP3B3StaticConformance LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "V4 CP3B3 static Vulkan conformance is currently validated on Windows only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(glm CONFIG REQUIRED)
find_package(Vulkan REQUIRED)
find_package(vsg 1.1.15 CONFIG REQUIRED)
find_package(vsgXchange CONFIG REQUIRED)

get_filename_component(OPENMW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

add_library(openmw_cp3b3_vsg STATIC
    "${OPENMW_ROOT}/components/render/backend/vsg/legacymaterialshader.cpp"
    "${OPENMW_ROOT}/components/render/backend/vsg/openmwviewdependentstate.cpp"
    "${OPENMW_ROOT}/components/render/backend/vsg/staticassetrealizer.cpp"
    "${OPENMW_ROOT}/components/render/backend/vsg/staticassetconformance.cpp"
    "${OPENMW_ROOT}/components/render/backend/vsg/statictexturedecode.cpp")
target_include_directories(openmw_cp3b3_vsg PUBLIC "${OPENMW_ROOT}")
target_link_libraries(openmw_cp3b3_vsg PUBLIC glm::glm Vulkan::Vulkan vsg::vsg vsgXchange::vsgXchange)
target_compile_features(openmw_cp3b3_vsg PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(openmw_cp3b3_vsg PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(openmw_cp3b3_vsg PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(openmw-vulkan-static-realizer-smoke realizer-smoke.cpp)
target_include_directories(openmw-vulkan-static-realizer-smoke PRIVATE "${OPENMW_ROOT}")
target_link_libraries(openmw-vulkan-static-realizer-smoke PRIVATE openmw_cp3b3_vsg)
if(MSVC)
    target_compile_options(openmw-vulkan-static-realizer-smoke PRIVATE /W4 /WX /permissive-)
endif()

add_executable(openmw-vulkan-legacy-material-shader-smoke legacy-material-shader-smoke.cpp)
target_include_directories(openmw-vulkan-legacy-material-shader-smoke PRIVATE "${OPENMW_ROOT}")
target_link_libraries(openmw-vulkan-legacy-material-shader-smoke PRIVATE openmw_cp3b3_vsg)
if(MSVC)
    target_compile_options(openmw-vulkan-legacy-material-shader-smoke PRIVATE /W4 /WX /permissive-)
endif()

add_executable(openmw-vulkan-static-conformance-routing-smoke conformance-routing-smoke.cpp)
target_include_directories(openmw-vulkan-static-conformance-routing-smoke PRIVATE "${OPENMW_ROOT}")
target_link_libraries(openmw-vulkan-static-conformance-routing-smoke PRIVATE openmw_cp3b3_vsg)
if(MSVC)
    target_compile_options(openmw-vulkan-static-conformance-routing-smoke PRIVATE /W4 /WX /permissive-)
endif()

enable_testing()
add_test(NAME openmw-vulkan-static-realizer-smoke COMMAND openmw-vulkan-static-realizer-smoke)
add_test(NAME openmw-vulkan-legacy-material-shader-smoke COMMAND openmw-vulkan-legacy-material-shader-smoke)
add_test(NAME openmw-vulkan-static-conformance-routing-smoke COMMAND openmw-vulkan-static-conformance-routing-smoke)

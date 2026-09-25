cmake_minimum_required(VERSION 3.16)
project(OptimizedMW_GL_P4_QC LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

find_package(OpenSceneGraph 3.6 REQUIRED COMPONENTS osg osgUtil)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

enable_testing()

add_executable(optimizedmw-p4-policy-tests policy-tests.cpp)
target_include_directories(optimizedmw-p4-policy-tests PRIVATE "${ROOT}")
target_link_libraries(optimizedmw-p4-policy-tests PRIVATE Threads::Threads)

add_library(optimizedmw-p4-compile-object OBJECT
    "${ROOT}/components/resource/openmwcompileoperation.cpp")
target_include_directories(optimizedmw-p4-compile-object PRIVATE "${ROOT}")
target_include_directories(optimizedmw-p4-compile-object SYSTEM PRIVATE ${OPENSCENEGRAPH_INCLUDE_DIRS})

if(MSVC)
    target_compile_options(optimizedmw-p4-policy-tests PRIVATE /W4 /WX /permissive- /EHsc)
    target_compile_options(optimizedmw-p4-compile-object PRIVATE /W4 /WX /permissive- /EHsc)
else()
    target_compile_options(optimizedmw-p4-policy-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(optimizedmw-p4-compile-object PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_test(NAME optimizedmw-p4-policy-tests COMMAND optimizedmw-p4-policy-tests)
set_tests_properties(optimizedmw-p4-policy-tests PROPERTIES TIMEOUT 30)

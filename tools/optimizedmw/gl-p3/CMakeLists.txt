cmake_minimum_required(VERSION 3.16)
project(OptimizedMW_GL_P3_QC LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
option(OPTIMIZEDMW_P3_SANITIZERS "Address and undefined-behavior sanitizers" OFF)
find_package(OpenSceneGraph 3.6 REQUIRED COMPONENTS osg osgDB osgUtil)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
enable_testing()
add_executable(optimizedmw-p3-submission-tests
    submission-tests.cpp
    "${ROOT}/components/sceneutil/optimizer.cpp"
    "${ROOT}/components/sceneutil/material.cpp"
    "${ROOT}/components/sceneutil/pagingwork.cpp")
target_include_directories(optimizedmw-p3-submission-tests PRIVATE "${ROOT}")
target_include_directories(optimizedmw-p3-submission-tests SYSTEM PRIVATE ${OPENSCENEGRAPH_INCLUDE_DIRS})
target_link_libraries(optimizedmw-p3-submission-tests PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL Threads::Threads)

add_executable(optimizedmw-p3-threading-tests threading-tests.cpp)
target_include_directories(optimizedmw-p3-threading-tests PRIVATE "${ROOT}")
target_link_libraries(optimizedmw-p3-threading-tests PRIVATE Threads::Threads)

add_executable(optimizedmw-p3-shadow-proxy-tests
    shadow-proxy-tests.cpp
    "${ROOT}/components/sceneutil/shadowproxygroup.cpp")
target_include_directories(optimizedmw-p3-shadow-proxy-tests PRIVATE "${ROOT}")
target_link_libraries(optimizedmw-p3-shadow-proxy-tests PRIVATE ${OPENSCENEGRAPH_LIBRARIES})

set(p3_targets optimizedmw-p3-submission-tests optimizedmw-p3-threading-tests optimizedmw-p3-shadow-proxy-tests)
if(MSVC)
    foreach(t IN LISTS p3_targets)
        target_compile_options(${t} PRIVATE /W4 /WX /permissive- /EHsc)
    endforeach()
else()
    foreach(t IN LISTS p3_targets)
        target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter)
        if(OPTIMIZEDMW_P3_SANITIZERS)
            target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${t} PRIVATE -fsanitize=address,undefined)
        endif()
    endforeach()
endif()
foreach(t IN LISTS p3_targets)
    add_test(NAME ${t} COMMAND ${t})
    set_tests_properties(${t} PROPERTIES TIMEOUT 60)
endforeach()

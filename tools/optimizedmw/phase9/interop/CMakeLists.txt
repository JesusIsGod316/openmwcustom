# Standalone developer fixture. No engine, NGX, jitter, or world renderer linkage.
add_executable(p9-interop-contract interop-contract-tests.cpp)
add_test(NAME p9-interop-contract COMMAND p9-interop-contract)
set_tests_properties(p9-interop-contract PROPERTIES TIMEOUT 10)

add_executable(p9-gl-vulkan-interop gl-vulkan-interop.cpp)
target_link_libraries(p9-gl-vulkan-interop PRIVATE ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL)
if(WIN32)
  # Headers are sufficient. The standalone process loads the installed driver
  # loader from System32, avoiding a globally installed SDK or copied loader.
  find_path(P9_VULKAN_INCLUDE_DIR vulkan/vulkan.h
    HINTS ${Vulkan_INCLUDE_DIR} "$ENV{VULKAN_SDK}" PATH_SUFFIXES Include include)
  if(NOT P9_VULKAN_INCLUDE_DIR)
    include(FetchContent)
    FetchContent_Declare(p9_vulkan_headers
      URL https://codeload.github.com/KhronosGroup/Vulkan-Headers/zip/refs/tags/v1.3.290
      URL_HASH SHA256=918a8bb31b8312fd649fd1d1826443da870f33f3187c16c4528aa94927409bab
      SOURCE_SUBDIR openmw-headers-only)
    FetchContent_MakeAvailable(p9_vulkan_headers)
    set(P9_VULKAN_INCLUDE_DIR "${p9_vulkan_headers_SOURCE_DIR}/include")
  endif()
  target_include_directories(p9-gl-vulkan-interop SYSTEM PRIVATE ${P9_VULKAN_INCLUDE_DIR})
  target_compile_definitions(p9-gl-vulkan-interop PRIVATE VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR NOMINMAX)
endif()
# Run explicitly on the user's intended GPU. CI's unsupported software adapter
# is a skip, while actual pixel/protocol failures fail the hardware gate.
add_test(NAME p9-gl-vulkan-interop COMMAND p9-gl-vulkan-interop
  --output ${CMAKE_CURRENT_BINARY_DIR}/interop-evidence.jsonl)
set_tests_properties(p9-gl-vulkan-interop PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)

# OpimizedMW GL-P0: initialize a NEW OpenGL build directory explicitly.
# Existing OpenMW executable/config/save/API names are intentionally retained.
set(OPENMW_ENABLE_V4_VULKAN_RUNTIME OFF CACHE BOOL "OpimizedMW uses OpenGL" FORCE)
set(OPENMW_V4_BUILD_RECOVERY_TESTS OFF CACHE BOOL "Vulkan-only recovery tests are not part of this build" FORCE)
set(OPENMW_LTO_BUILD ON CACHE BOOL "Match the established Windows control recipe")
set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Optimized build with symbols")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Record exact compilation commands")

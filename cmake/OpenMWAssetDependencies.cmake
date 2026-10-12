# Exact CPU-only dependency closure for the experimental asset pipeline.
# Shared by the production graph and the independently configured Phase9 graph.
function(openmw_asset_dependencies)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    include(FetchContent)
    if(NOT TARGET meshoptimizer)
        set(MESHOPT_BUILD_DEMO OFF)
        set(MESHOPT_BUILD_GLTFPACK OFF)
        set(MESHOPT_BUILD_SHARED_LIBS OFF)
        set(MESHOPT_INSTALL OFF)
        FetchContent_Declare(openmw_meshoptimizer
            URL https://codeload.github.com/zeux/meshoptimizer/zip/9e1f07b159d3cb777f1c67ed31fc11fd117986f4
            URL_HASH SHA512=597c06a7e0175c9b8570c6c006d079682c819649496c60556b66f70639928b5d16df6d585c76a8b98fa04e6b062129da4c33315a79a620cd41b953c89608b3eb)
        FetchContent_MakeAvailable(openmw_meshoptimizer)
        install(FILES "${openmw_meshoptimizer_SOURCE_DIR}/LICENSE.md"
            DESTINATION licenses RENAME meshoptimizer-LICENSE.txt COMPONENT Runtime)
    endif()
    if(WIN32 AND NOT TARGET DirectXTex)
        # Function-local normal variables keep upstream generic options from
        # changing the engine's shared-library, tool or compiler policies.
        set(BUILD_SHARED_LIBS OFF)
        set(BUILD_TOOLS OFF)
        set(BUILD_SAMPLE OFF)
        set(BUILD_DX11 OFF)
        set(BUILD_DX12 OFF)
        set(BC_USE_OPENMP OFF)
        set(BUILD_FUZZING OFF)
        set(ENABLE_OPENEXR_SUPPORT OFF)
        set(ENABLE_LIBJPEG_SUPPORT OFF)
        set(ENABLE_LIBPNG_SUPPORT OFF)
        FetchContent_Declare(openmw_directxtex
            URL https://codeload.github.com/microsoft/DirectXTex/zip/4feb3e11a020f35b796fc769a74216a555d4f5ef
            URL_HASH SHA512=58a19eca4f7b26766c714cec87c59f05c18785822a33c8c973ec19f65691ef63e3c133f579dc955f29638822fbd0aae738c6cc46b39d481132448a1926fe75a8)
        FetchContent_MakeAvailable(openmw_directxtex)
        install(FILES "${openmw_directxtex_SOURCE_DIR}/LICENSE"
            DESTINATION licenses RENAME DirectXTex-LICENSE.txt COMPONENT Runtime)
    endif()
    if(NOT TARGET openmw-asset-backends)
        add_library(openmw-asset-backends INTERFACE)
        target_link_libraries(openmw-asset-backends INTERFACE meshoptimizer)
        if(WIN32)
            target_link_libraries(openmw-asset-backends INTERFACE DirectXTex windowscodecs ole32)
            target_compile_definitions(openmw-asset-backends INTERFACE OPENMW_HAVE_DIRECTXTEX=1)
        endif()
    endif()
endfunction()

set(OPENMW_SOURCES
    engine.cpp
    options.cpp
)

set(OPENMW_RESOURCES
    ${CMAKE_SOURCE_DIR}/files/windows/openmw.rc
    ${CMAKE_SOURCE_DIR}/files/windows/openmw.exe.manifest
)

set(OPENMW_HEADERS
    doc.hpp
    engine.hpp
    options.hpp
    profile.hpp
)

source_group(apps/openmw FILES main.cpp androidmain.cpp ${OPENMW_SOURCES} ${OPENMW_HEADERS} ${OPENMW_RESOURCES})

add_openmw_dir (mwrender
    actors objects renderingmanager animation rotatecontroller sky skyutil npcanimation esm4npcanimation vismask
    creatureanimation effectmanager util renderinginterface pathgrid rendermode weaponanimation screenshotmanager
    bulletdebugdraw globalmap characterpreview camera localmap localmapnative water terrainstorage ripplesimulation
    renderbin actoranimation landmanager navmesh actorspaths recastmesh fogmanager fogstate celllighting objectpaging groundcover groundcoverquery
    postprocessor pingpongcull luminancecalculator pingpongcanvas nisscaler transparentpass precipitationocclusion ripples
    actorutil distortion animationpriority bonegroup blendmask animblendcontroller occlusionculling
    )

add_openmw_dir (mwinput
    actions actionmanager bindingsmanager controllermanager controlswitch
    inputmanagerimp mousemanager keyboardmanager sensormanager gyromanager
    )

add_openmw_dir (mwgui
    layout textinput widgets race class birth review windowmanagerimp console dialogue
    windowbase statswindow messagebox journalwindow charactercreation
    mapwindow windowpinnablebase tooltips scrollwindow bookwindow resourceskin
    formatting inventorywindow container hud countdialog tradewindow settingswindow
    confirmationdialog alchemywindow referenceinterface spellwindow mainmenu quickkeysmenu
    itemselection spellbuyingwindow loadingscreen levelupdialog waitdialog spellcreationdialog
    enchantingdialog trainingwindow travelwindow exposedwindow cursor spellicons
    merchantrepair repair soulgemdialog companionwindow bookpage journalviewmodel journalbooks
    itemmodel containeritemmodel inventoryitemmodel sortfilteritemmodel itemview
    tradeitemmodel companionitemmodel pickpocketitemmodel controllers savegamedialog
    recharge mode videowidget backgroundimage itemwidget screenfader debugwindow spellmodel spellview
    draganddrop timeadvancer jailscreen itemchargeview keyboardnavigation textcolours statswatcher
    postprocessorhud settings worlditemmodel itemtransfer controllerbuttonsoverlay inventorytabsoverlay
    )

add_openmw_dir (mwdialogue
    dialoguemanagerimp journalimp journalentry quest topic filter selectwrapper keywordsearch scripttest
    )

add_openmw_dir (mwscript
    locals scriptmanagerimp compilercontext interpretercontext cellextensions miscextensions
    guiextensions soundextensions skyextensions statsextensions containerextensions
    aiextensions controlextensions extensions globalscripts ref dialogueextensions
    animationextensions transformationextensions consoleextensions userextensions
    )

add_openmw_dir (mwlua
    luamanagerimp object objectlists userdataserializer luaevents engineevents objectvariant loadscripts contentbindings
    context menuscripts globalscripts localscripts playerscripts luabindings objectbindings cellbindings coremwscriptbindings
    mwscriptbindings camerabindings vfsbindings uibindings soundbindings inputbindings nearbybindings dialoguebindings
    postprocessingbindings stats recordstore debugbindings corebindings worldbindings worker landbindings magicbindings factionbindings
    classbindings itemdata inputprocessor animationbindings birthsignbindings racebindings markupbindings weatherbindings regionbindings
    types/types types/door types/item types/actor types/container types/lockable types/weapon types/npc types/bodypart
    types/creature types/player types/activator types/book types/lockpick types/probe types/apparatus
    types/potion types/ingredient types/misc types/repair types/armor types/light types/static
    types/clothing types/levelledlist types/terminal magictypebindings
    )

add_openmw_dir (mwsound
    soundmanagerimp openaloutput ffmpegdecoder headcache sfxpredecodecache sound soundbuffer sounddecoder soundoutput
    loudness movieaudiofactory alext efx efxpresets regionsoundselector watersoundupdater
    )

add_openmw_dir (mwworld
    refdata worldimp scene globals class action nullaction actionteleport
    containerstore actiontalk actiontake manualref player cellvisitors failedaction
    worldmodel localscripts customdata inventorystore ptr actionopen actionread actionharvest
    actionequip timestamp actionalchemy cellstore actionapply actioneat
    store esmstore fallback actionrepair actionsoulgem livecellref actiondoor
    contentloader esmloader actiontrap cellreflist cellref weather projectilemanager
    cellpreloader datetimemanager groundcoverstore magiceffects cell ptrregistry
    positioncellgrid
    )

add_openmw_dir (mwphysics
    physicssystem trace collisiontype actor convert object heightfield closestnotmerayresultcallback
    contacttestresultcallback stepper movementsolver projectile
    actorconvexcallback raycasting mtphysics contacttestwrapper projectileconvexcallback
    )

add_openmw_dir (mwclass
    classes activator creature npc weapon armor potion apparatus book clothing container door
    ingredient creaturelevlist itemlevlist light lockpick misc probe repair static actor bodypart
    esm4base esm4npc light4
    )

add_openmw_dir (mwmechanics
    mechanicsmanagerimp stat creaturestats magiceffects movement actorutil spelllist
    drawstate spells activespells npcstats aipackage aisequence aipursue alchemy aiwander aitravel aifollow aiavoiddoor aibreathe
    aicast aiescort aiface aiactivate aicombat recharge repair enchanting pathfinding pathgrid security spellcasting spellresistance
    disease pickpocket levelledlist combat steering obstacle autocalcspell difficultyscaling aicombataction summoning
    character actors objects aistate weaponpriority spellpriority weapontype spellutil
    spelleffects
    )

add_openmw_dir (mwstate
    statemanagerimp charactermanager character quicksavemanager
    )

add_openmw_dir (mwbase
    environment world scriptmanager dialoguemanager journal soundmanager mechanicsmanager
    inputmanager windowmanager statemanager luamanager
    )

# Main executable

add_library(openmw-lib STATIC
    ${OPENMW_FILES}
    ${OPENMW_SOURCES}
)

if(OPENMW_ENABLE_V4_VULKAN_RUNTIME)
    if(NOT BUILD_OPENMW)
        message(FATAL_ERROR "OPENMW_ENABLE_V4_VULKAN_RUNTIME requires BUILD_OPENMW=ON")
    endif()

    find_package(glm CONFIG REQUIRED)
    find_package(Vulkan REQUIRED)
    find_package(vsg 1.1.15 CONFIG REQUIRED)
    find_package(vsgXchange CONFIG REQUIRED)

    include("${CMAKE_SOURCE_DIR}/components/render/backend/vsg/runtime-sources.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/mwrender/v4engine-sources.cmake")

    add_library(openmw-v4-vulkan-runtime STATIC ${OPENMW_V4_VSG_RUNTIME_SOURCES})
    target_include_directories(openmw-v4-vulkan-runtime PUBLIC ${CMAKE_SOURCE_DIR})
    target_link_libraries(openmw-v4-vulkan-runtime PUBLIC
        components
        SDL3::SDL3
        glm::glm
        Vulkan::Vulkan
        vsg::vsg
        vsgXchange::vsgXchange)
    target_compile_features(openmw-v4-vulkan-runtime PUBLIC cxx_std_20)

    target_sources(openmw-lib PRIVATE ${OPENMW_V4_ENGINE_RUNTIME_SOURCES})
    # openmw-lib uses CMake's plain target_link_libraries signature below.
    # Mixing the keyword and plain signatures is rejected at configure time.
    target_link_libraries(openmw-lib openmw-v4-vulkan-runtime)
    # engine.hpp conditionally contains Vulkan runtime members, so this definition
    # is part of openmw-lib's public C++ ABI. Every consumer that sees Engine must
    # compile with the same layout; keeping it PRIVATE under-allocates Engine in
    # main.cpp and corrupts the Windows process heap during construction.
    target_compile_definitions(openmw-lib PUBLIC OPENMW_ENABLE_V4_VULKAN_RUNTIME=1)
endif()

if (MSVC)
    # these TUs take a lot of memory for codegen
    # in CI, they often go over the 8GB memory limit if another cl.exe is running at the same time
    # this option (make sure you're not looking at the docs for the identically-named linker option) stops four codegen threads being spun off
    # it hurts build time ~33% to enable this unnecessarily, though, so target it to the hungriest boys
    set_source_files_properties(
        mwlua/dialoguebindings.cpp
        mwlua/magicbindings.cpp
        mwlua/objectbindings
        mwlua/stats.cpp
        mwworld/esmstore.cpp
        PROPERTIES
        COMPILE_OPTIONS /cgthreads1
    )
endif()

if(BUILD_OPENMW)
    if (ANDROID)
        add_library(openmw SHARED
            main.cpp
            androidmain.cpp
        )
    else()
        openmw_add_executable(openmw
            ${APPLE_BUNDLE_RESOURCES}
            ${OPENMW_RESOURCES}
            main.cpp
        )
    endif()

    target_link_libraries(openmw openmw-lib)
    if (TARGET openmw-shader-resources)
        add_dependencies(openmw openmw-shader-resources)
    endif()

    # Workaround necessary to ensure osgAnimation::MatrixLinearSampler dynamic casts work under Clang
    # NOTE: it's unclear whether the broken behavior is spec-compliant
    if (CMAKE_CXX_COMPILER_ID STREQUAL Clang)
        set_target_properties(openmw PROPERTIES ENABLE_EXPORTS ON)
    endif()
endif()

# Sound stuff - here so CMake doesn't stupidly recompile EVERYTHING
# when we change the backend.
include_directories(
    ${FFmpeg_INCLUDE_DIRS}
)

target_link_libraries(openmw-lib
    # CMake's built-in OSG finder does not use pkgconfig, so we have to
    # manually ensure the order is correct for inter-library dependencies.
    # This only makes a difference with `-DOPENMW_USE_SYSTEM_OSG=ON -DOSG_STATIC=ON`.
    # https://gitlab.kitware.com/cmake/cmake/-/issues/21701
    ${OSGPARTICLE_LIBRARIES}
    ${OSGVIEWER_LIBRARIES}
    ${OSGGA_LIBRARIES}
    ${OSGSHADOW_LIBRARIES}
    ${OSGDB_LIBRARIES}
    ${OSGUTIL_LIBRARIES}
    ${OSG_LIBRARIES}
    Boost::program_options
    ${OPENAL_LIBRARY}
    ${FFmpeg_LIBRARIES}
    ${MyGUI_LIBRARIES}
    SDL3::SDL3
    ${RecastNavigation_LIBRARIES}
    osg-ffmpeg-videoplayer
    oics
    components
)

if (MSVC AND PRECOMPILE_HEADERS_WITH_MSVC)
    target_precompile_headers(openmw-lib PRIVATE
        <boost/program_options/options_description.hpp>

        <sol/sol.hpp>

        <osg/StateSet>
        <osg/Node>
        <osg/Drawable>
        <osg/Camera>

        <osgViewer/Viewer>

        <MyGUI_Widget.h>

        <algorithm>
        <filesystem>
        <fstream>
        <functional>
        <memory>
        <string>
        <string_view>
        <vector>
    )
endif()

if (ANDROID)
    target_link_libraries(openmw-lib EGL android log z)
endif (ANDROID)

if (USE_SYSTEM_TINYXML)
    target_link_libraries(openmw-lib ${TinyXML_LIBRARIES})
endif()


# Fix for not visible pthreads functions for linker with glibc 2.15
if (UNIX AND NOT APPLE)
    target_link_libraries(openmw-lib ${CMAKE_THREAD_LIBS_INIT})
endif()

if(APPLE AND BUILD_OPENMW)
    set(BUNDLE_RESOURCES_DIR "${APP_BUNDLE_DIR}/Contents/Resources")

    set(OPENMW_RESOURCES_ROOT ${BUNDLE_RESOURCES_DIR})

    add_subdirectory(../../files/ ${CMAKE_CURRENT_BINARY_DIR}/files)

    configure_file("${OpenMW_BINARY_DIR}/defaults.bin" ${BUNDLE_RESOURCES_DIR} COPYONLY)
    configure_file("${OpenMW_BINARY_DIR}/openmw.cfg" ${BUNDLE_RESOURCES_DIR} COPYONLY)
    configure_file("${OpenMW_BINARY_DIR}/gamecontrollerdb.txt" ${BUNDLE_RESOURCES_DIR} COPYONLY)

    add_custom_command(TARGET openmw
        POST_BUILD
        COMMAND cp "${OpenMW_BINARY_DIR}/resources/version" "${BUNDLE_RESOURCES_DIR}/resources")

    find_library(COCOA_FRAMEWORK Cocoa)
    find_library(IOKIT_FRAMEWORK IOKit)
    target_link_libraries(openmw ${COCOA_FRAMEWORK} ${IOKIT_FRAMEWORK})

    if (FFmpeg_FOUND)
        target_link_options(openmw PRIVATE "LINKER:SHELL:-framework CoreVideo"
                                           "LINKER:SHELL:-framework CoreMedia"
                                           "LINKER:SHELL:-framework VideoToolbox"
                                           "LINKER:SHELL:-framework AudioToolbox"
                                           "LINKER:SHELL:-framework VideoDecodeAcceleration")
    endif()
endif()

if (BUILD_WITH_CODE_COVERAGE)
    target_compile_options(openmw-lib PRIVATE --coverage)
    target_link_libraries(openmw-lib gcov)
    if (NOT ANDROID AND BUILD_OPENMW)
        target_compile_options(openmw PRIVATE --coverage)
        target_link_libraries(openmw gcov)
    endif()
endif()

if (WIN32 AND BUILD_OPENMW)
    INSTALL(TARGETS openmw RUNTIME DESTINATION ".")
    # Optional manual diagnostics travel with both optimized backend builds.
    # They never run during install, never change cache policy, and never load a save.
    INSTALL(FILES
        ../../tools/v4/cp4/gameplay-diagnostics.py
        ../../tools/v4/cp4/diagnosticconfig.py
        ../../tools/v4/cp4/Start-CP4F-Test.cmd
        ../../tools/v4/cp4/runtime-diagnostics.py
        ../../tools/v4/cp4/shader_resources.py
        ../../tools/v4/cp4/run-runtime-qc.ps1
        ../../tools/v4/cp4/run-memory-qc.ps1
        ../../tools/v4/cp4/HOST-MEMORY-TERRAIN-REPAIR.md
        ../../tools/v4/cp4/RENDERING-REPAIR.md
        ../../tools/v4/cp4/RUNTIME-DIAGNOSTICS.md
        DESTINATION ".")

    # VulkanMW packaging contract: every installed Windows Vulkan build carries
    # the same automated benchmark launcher and driver used for matched hardware
    # comparisons. OpenGL-only builds do not install these VulkanMW entrypoints.
    if(OPENMW_ENABLE_V4_VULKAN_RUNTIME)
        INSTALL(FILES
            ../../tools/vulkanmw/START-VulkanMW-Test.bat
            ../../tools/v4/cp4/architecture-benchmark.py
            ../../tools/v4/cp4/architecture-benchmark.lua
            DESTINATION ".")
    endif()
endif()

if (OPENMW_V4_BUILD_RECOVERY_TESTS)
    if (NOT OPENMW_ENABLE_V4_VULKAN_RUNTIME)
        message(FATAL_ERROR "CP4F recovery tests require OPENMW_ENABLE_V4_VULKAN_RUNTIME")
    endif()
    add_executable(openmw-v4-water-camera-tests ../../tools/v4/cp4/water-auxiliary-smoke.cpp)
    target_link_libraries(openmw-v4-water-camera-tests PRIVATE openmw-lib)
    add_test(NAME openmw-v4-water-camera-tests COMMAND openmw-v4-water-camera-tests)
    add_executable(openmw-v4-effect-capture-tests ../../tools/v4/cp4/effect-capture-tests.cpp)
    target_link_libraries(openmw-v4-effect-capture-tests PRIVATE openmw-lib)
    add_test(NAME openmw-v4-effect-capture-tests COMMAND openmw-v4-effect-capture-tests)
    add_executable(openmw-v4-effect-uv-tests ../../tools/v4/cp4/effect-uv-tests.cpp)
    target_link_libraries(openmw-v4-effect-uv-tests PRIVATE openmw-lib)
    add_test(NAME openmw-v4-effect-uv-tests COMMAND openmw-v4-effect-uv-tests)
    add_executable(openmw-v4-rendering-uniform-tests ../../tools/v4/cp4/rendering-uniform-tests.cpp)
    target_link_libraries(openmw-v4-rendering-uniform-tests PRIVATE openmw-lib)
    add_test(NAME openmw-v4-rendering-uniform-tests COMMAND openmw-v4-rendering-uniform-tests)
    add_executable(openmw-v4-rendering-capture-tests ../../tools/v4/cp4/rendering-capture-tests.cpp)
    target_link_libraries(openmw-v4-rendering-capture-tests PRIVATE openmw-lib)
    add_test(NAME openmw-v4-rendering-capture-tests COMMAND openmw-v4-rendering-capture-tests)
    add_executable(openmw-v4-rendering-view-features-tests ../../tools/v4/cp4/rendering-view-features-tests.cpp)
    target_link_libraries(openmw-v4-rendering-view-features-tests PRIVATE openmw-lib)
    add_test(NAME openmw-v4-rendering-view-features-tests COMMAND openmw-v4-rendering-view-features-tests)
    # Headless Vulkan pixel suite: compile on Windows, execute only on an explicitly
    # provisioned Vulkan host. It creates no gameplay window and reads no assets/saves.
    add_executable(openmw-v4-rendering-pixel-tests ../../tools/v4/cp4/rendering-pixel-tests.cpp)
    target_link_libraries(openmw-v4-rendering-pixel-tests PRIVATE openmw-lib)
    foreach(pressure_case host-memory cache-pressure)
        add_executable(openmw-v4-${pressure_case}-tests ../../tools/v4/cp4/${pressure_case}-tests.cpp)
        target_link_libraries(openmw-v4-${pressure_case}-tests PRIVATE openmw-lib)
        add_test(NAME openmw-v4-${pressure_case}-tests COMMAND openmw-v4-${pressure_case}-tests)
    endforeach()
    add_executable(openmw-v4-runtime-diagnostics-tests ../../tools/v4/cp4/runtime-diagnostics-tests.cpp)
    target_link_libraries(openmw-v4-runtime-diagnostics-tests PRIVATE openmw-lib)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/runtime-diagnostic-evidence")
    foreach(diagnostic_mode off standard focused)
        add_test(NAME openmw-v4-runtime-diagnostics-${diagnostic_mode}
            COMMAND openmw-v4-runtime-diagnostics-tests ${diagnostic_mode})
        set_tests_properties(openmw-v4-runtime-diagnostics-${diagnostic_mode} PROPERTIES ENVIRONMENT
            "OPENMW_RUNTIME_DIAGNOSTICS=${diagnostic_mode};OPENMW_RUNTIME_DIAGNOSTICS_FILE=${CMAKE_BINARY_DIR}/runtime-diagnostic-evidence/${diagnostic_mode}.jsonl;OPENMW_GAMEPLAY_DIAGNOSTICS_FILE=${CMAKE_BINARY_DIR}/runtime-diagnostic-evidence/${diagnostic_mode}-gameplay.jsonl")
    endforeach()
    # Explicit GPU gate: creates a short-lived Vulkan window. Run manually on a
    # graphics-capable host, rather than making headless CTest require a display.
    add_executable(openmw-v4-gui-isolation-tests ../../tools/v4/cp4/gui-isolation-tests.cpp)
    target_link_libraries(openmw-v4-gui-isolation-tests PRIVATE openmw-lib)
    add_executable(openmw-v4-texture-residency-tests ../../tools/v4/cp4/texture-residency-tests.cpp)
    target_link_libraries(openmw-v4-texture-residency-tests PRIVATE openmw-lib)
    add_executable(openmw-v4-water-transition-tests ../../tools/v4/cp4/water-transition-tests.cpp)
    target_link_libraries(openmw-v4-water-transition-tests PRIVATE openmw-lib)
    add_executable(openmw-v4-update-only-tests ../../tools/v4/cp4/update-only-tests.cpp)
    target_link_libraries(openmw-v4-update-only-tests PRIVATE openmw-lib)
    add_test(NAME openmw-v4-update-only-tests COMMAND openmw-v4-update-only-tests)
    add_executable(openmw-v4-static-sync-tests ../../tools/v4/cp4/static-sync-state-tests.cpp)
    target_link_libraries(openmw-v4-static-sync-tests PRIVATE openmw-lib)
    add_test(NAME openmw-v4-static-sync-tests COMMAND openmw-v4-static-sync-tests)
    add_executable(openmw-v4-actor-skin-space-tests ../../tools/v4/cp4/actor-skin-space-tests.cpp)
    target_link_libraries(openmw-v4-actor-skin-space-tests PRIVATE openmw-lib)
    add_test(NAME openmw-v4-actor-skin-space-tests COMMAND openmw-v4-actor-skin-space-tests)
    add_executable(openmw-v4-publication-runtime-tests ../../tools/v4/cp4/publication-runtime-tests.cpp)
    target_link_libraries(openmw-v4-publication-runtime-tests PRIVATE openmw-lib)
    add_test(NAME openmw-v4-publication-runtime-tests COMMAND openmw-v4-publication-runtime-tests)
    add_test(NAME openmw-v4-publication-control-tests COMMAND openmw-v4-publication-runtime-tests)
    set_tests_properties(openmw-v4-publication-control-tests PROPERTIES ENVIRONMENT "OPENMW_V4_FULL_WORLD_PUBLICATION=1")
    add_executable(openmw-v4-semantic-tests ../../tools/v4/cp3b/rendercore-semantic-smoke.cpp)
    target_link_libraries(openmw-v4-semantic-tests PRIVATE openmw-lib)
    if(MSVC)
        target_compile_options(openmw-v4-semantic-tests PRIVATE /UNDEBUG)
    else()
        target_compile_options(openmw-v4-semantic-tests PRIVATE -UNDEBUG)
    endif()
    add_test(NAME openmw-v4-semantic-tests COMMAND openmw-v4-semantic-tests)
    add_test(NAME openmw-v4-semantic-control-tests COMMAND openmw-v4-semantic-tests)
    set_tests_properties(openmw-v4-semantic-control-tests PROPERTIES ENVIRONMENT "OPENMW_V4_FULL_WORLD_PUBLICATION=1")
    add_executable(openmw-v4-diagnostic-emission-tests ../../tools/v4/cp4/diagnostic-emission-tests.cpp)
    target_include_directories(openmw-v4-diagnostic-emission-tests PRIVATE ${PROJECT_SOURCE_DIR})
    add_test(NAME openmw-v4-diagnostics-enabled COMMAND openmw-v4-diagnostic-emission-tests on)
    add_test(NAME openmw-v4-diagnostics-disabled COMMAND openmw-v4-diagnostic-emission-tests off)
    foreach(recovery_case dynamic-actor-plan deformation)
        add_executable(openmw-v4-${recovery_case}-tests ../../tools/v4/cp3d/${recovery_case}-smoke.cpp)
        target_link_libraries(openmw-v4-${recovery_case}-tests PRIVATE openmw-lib)
        # Older fixtures use assert for publication and checks. Keep them live in Release.
        if(MSVC)
            target_compile_options(openmw-v4-${recovery_case}-tests PRIVATE /UNDEBUG)
        else()
            target_compile_options(openmw-v4-${recovery_case}-tests PRIVATE -UNDEBUG)
        endif()
        add_test(NAME openmw-v4-${recovery_case}-tests COMMAND openmw-v4-${recovery_case}-tests)
    endforeach()
endif()

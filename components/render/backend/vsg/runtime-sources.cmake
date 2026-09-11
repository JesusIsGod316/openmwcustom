# One authoritative implementation list for the production VSG runtime and the
# real-NIF integration executable. Keeping this closure shared prevents either
# target from compiling successfully while omitting a required implementation
# object from the other.
set(OPENMW_V4_VSG_RUNTIME_SOURCES
    "${CMAKE_SOURCE_DIR}/components/nifrender/niftranslator.cpp"
    "${CMAKE_SOURCE_DIR}/components/nifrender/staticniftranslator.cpp"
    "${CMAKE_SOURCE_DIR}/components/render/backend/vsg/auxiliaryreadback.cpp"
    "${CMAKE_SOURCE_DIR}/components/render/backend/vsg/legacymaterialshader.cpp"
    "${CMAKE_SOURCE_DIR}/components/render/backend/vsg/openmwviewdependentstate.cpp"
    "${CMAKE_SOURCE_DIR}/components/render/backend/vsg/offscreenrendertarget.cpp"
    "${CMAKE_SOURCE_DIR}/components/render/backend/vsg/sdlvulkanwindow.cpp"
    "${CMAKE_SOURCE_DIR}/components/render/backend/vsg/skybackdrop.cpp"
    "${CMAKE_SOURCE_DIR}/components/render/backend/vsg/staticassetconformance.cpp"
    "${CMAKE_SOURCE_DIR}/components/render/backend/vsg/staticassetrealizer.cpp"
    "${CMAKE_SOURCE_DIR}/components/render/backend/vsg/statictexturedecode.cpp"
    "${CMAKE_SOURCE_DIR}/components/render/backend/vsg/uipipeline.cpp"
    "${CMAKE_SOURCE_DIR}/components/render/backend/vsg/watersurface.cpp"
    "${CMAKE_SOURCE_DIR}/components/render/backend/vsg/vsgruntimebootstrap.cpp"
    "${CMAKE_SOURCE_DIR}/components/render/backend/vsg/vsgruntimehost.cpp"
    "${CMAKE_SOURCE_DIR}/components/render/backend/vsg/vsgsemanticsession.cpp"
    "${CMAKE_SOURCE_DIR}/components/vsgmygui/externaltextures.cpp"
    "${CMAKE_SOURCE_DIR}/components/vsgmygui/platform.cpp"
    "${CMAKE_SOURCE_DIR}/components/vsgmygui/rendermanager.cpp"
    "${CMAKE_SOURCE_DIR}/components/vsgmygui/texture.cpp"
    "${CMAKE_SOURCE_DIR}/components/vsgmygui/vfsimagedecoder.cpp")
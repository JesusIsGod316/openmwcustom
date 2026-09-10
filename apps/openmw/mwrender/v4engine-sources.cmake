# One authoritative source-side adapter list for the production OpenMW target
# and the real-NIF integration compile target.
set(OPENMW_V4_ENGINE_RUNTIME_SOURCES
    "${CMAKE_SOURCE_DIR}/apps/openmw/mwrender/v4engineframecoordinator.cpp"
    "${CMAKE_SOURCE_DIR}/apps/openmw/mwrender/v4enginerenderbridge.cpp"
    "${CMAKE_SOURCE_DIR}/apps/openmw/mwrender/v4runtimeoptions.cpp"
    "${CMAKE_SOURCE_DIR}/apps/openmw/mwrender/v4scenerenderlifecycle.cpp"
    "${CMAKE_SOURCE_DIR}/apps/openmw/mwrender/v4semanticsource.cpp"
    "${CMAKE_SOURCE_DIR}/apps/openmw/mwrender/v4terrainsource.cpp")

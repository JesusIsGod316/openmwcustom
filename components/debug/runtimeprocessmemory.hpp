#ifndef OPENMW_DEBUG_RUNTIMEPROCESSMEMORY_H
#define OPENMW_DEBUG_RUNTIMEPROCESSMEMORY_H

namespace Debug::RuntimeDiagnostics
{
    // Implemented in components so consumers do not import Windows SDK macros.
    void sampleProcessMemory() noexcept;
}

#endif

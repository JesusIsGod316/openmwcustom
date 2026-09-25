#include <components/render/native/nifsemanticcompiler.hpp>

#include <type_traits>

int main()
{
    using namespace RenderNative;
    static_assert(std::is_nothrow_move_constructible_v<NifSemanticCompileResult>);
    NifSemanticCompileResult result;
    if (result.compiled())
        return 1;
    if (result.semanticReady())
        return 2;
    return result.status == NifSemanticCompileStatus::InvalidSource ? 0 : 3;
}

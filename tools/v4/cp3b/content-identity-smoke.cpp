#include <components/nifrender/translationidentity.hpp>

#include <cassert>
#include <unordered_map>

int main()
{
    using namespace NifRender;

    TranslationSourceIdentity firstSource;
    firstSource.sourceIdentity = "meshes/a/example.nif";
    firstSource.contentIdentity = "sha256:0123456789abcdef";
    assert(firstSource.valid());

    TranslationSourceIdentity aliasSource;
    aliasSource.sourceIdentity = "meshes/b/alias.nif";
    aliasSource.contentIdentity = firstSource.contentIdentity;
    assert(aliasSource.valid());
    assert(firstSource.sourceIdentity != aliasSource.sourceIdentity);

    const TranslationContentKey firstKey = makeTranslationContentKey(firstSource.contentIdentity, false);
    const TranslationContentKey aliasKey = makeTranslationContentKey(aliasSource.contentIdentity, false);
    assert(firstKey.valid());
    assert(firstKey == aliasKey);
    assert(stableTranslationKeyFingerprint(firstKey) == stableTranslationKeyFingerprint(aliasKey));

    const TranslationContentKey markerKey = makeTranslationContentKey(firstSource.contentIdentity, true);
    assert(markerKey.valid());
    assert(markerKey != firstKey);
    assert(stableTranslationKeyFingerprint(markerKey) != stableTranslationKeyFingerprint(firstKey));

    TranslationContentKey nextSchema = firstKey;
    ++nextSchema.semanticSchemaRevision;
    assert(nextSchema != firstKey);
    assert(stableTranslationKeyFingerprint(nextSchema) != stableTranslationKeyFingerprint(firstKey));

    std::unordered_map<TranslationContentKey, unsigned int, TranslationContentKeyHash> dedup;
    dedup.emplace(firstKey, 1u);
    dedup.emplace(aliasKey, 2u);
    dedup.emplace(markerKey, 3u);
    assert(dedup.size() == 2u);
    assert(dedup.at(firstKey) == 1u);
    assert(dedup.at(markerKey) == 3u);

    TranslationContentKey invalid;
    assert(!invalid.valid());

    return 0;
}

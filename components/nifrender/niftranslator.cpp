#include "niftranslator.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <components/misc/strings/algorithm.hpp>
#include <components/nif/data.hpp>
#include <components/nif/extra.hpp>
#include <components/nif/niffile.hpp>
#include <components/nif/node.hpp>
#include <components/nif/property.hpp>
#include <components/nif/texture.hpp>

namespace
{
    [[nodiscard]] glm::vec2 toGlm(const osg::Vec2f& value) noexcept
    {
        return { value.x(), value.y() };
    }

    [[nodiscard]] glm::vec3 toGlm(const osg::Vec3f& value) noexcept
    {
        return { value.x(), value.y(), value.z() };
    }

    [[nodiscard]] glm::vec4 toGlm(const osg::Vec4f& value) noexcept
    {
        return { value.x(), value.y(), value.z(), value.w() };
    }

    [[nodiscard]] glm::mat4 toGlm(const Nif::NiTransform& value) noexcept
    {
        glm::mat4 result(1.0f);
        for (std::size_t row = 0; row < 3; ++row)
        {
            for (std::size_t column = 0; column < 3; ++column)
                result[column][row] = value.mRotation.mValues[row][column] * value.mScale;
        }
        result[3][0] = value.mTranslation.x();
        result[3][1] = value.mTranslation.y();
        result[3][2] = value.mTranslation.z();
        return result;
    }

    [[nodiscard]] float halfToFloat(std::uint16_t value) noexcept
    {
        std::uint32_t bits = static_cast<std::uint32_t>(value & 0x8000u) << 16u;
        const std::uint32_t exp16 = (value & 0x7c00u) >> 10u;
        std::uint32_t frac16 = value & 0x03ffu;
        if (exp16 != 0)
            bits |= (exp16 + 0x70u) << 23u;
        else if (frac16 != 0)
        {
            std::uint8_t offset = 0;
            do
            {
                ++offset;
                frac16 <<= 1u;
            } while ((frac16 & 0x0400u) != 0x0400u);
            frac16 &= 0x03ffu;
            bits |= (0x71u - offset) << 23u;
        }
        bits |= frac16 << 13u;

        float result = 0.0f;
        std::memcpy(&result, &bits, sizeof(result));
        return result;
    }

    [[nodiscard]] float normalByteToFloat(char value) noexcept
    {
        return static_cast<unsigned char>(value) / 255.0f * 2.0f - 1.0f;
    }

    [[nodiscard]] glm::vec4 colorByteToFloat(const std::array<char, 4>& value) noexcept
    {
        constexpr float scale = 1.0f / 255.0f;
        return { static_cast<unsigned char>(value[0]) * scale, static_cast<unsigned char>(value[1]) * scale,
            static_cast<unsigned char>(value[2]) * scale, static_cast<unsigned char>(value[3]) * scale };
    }

    void updateBounds(RenderCore::AxisAlignedBounds& bounds, const std::vector<glm::vec3>& positions) noexcept
    {
        if (positions.empty())
            return;

        bounds.minimum = positions.front();
        bounds.maximum = positions.front();
        for (const glm::vec3& position : positions)
        {
            bounds.minimum = glm::min(bounds.minimum, position);
            bounds.maximum = glm::max(bounds.maximum, position);
        }
    }

    [[nodiscard]] bool isLegacyGeometry(Nif::RecordType type) noexcept
    {
        switch (type)
        {
            case Nif::RC_NiTriShape:
            case Nif::RC_NiTriStrips:
            case Nif::RC_NiLines:
            case Nif::RC_BSLODTriShape:
            case Nif::RC_BSSegmentedTriShape:
                return true;
            default:
                return false;
        }
    }

    [[nodiscard]] bool isBethesdaGeometry(Nif::RecordType type) noexcept
    {
        switch (type)
        {
            case Nif::RC_BSTriShape:
            case Nif::RC_BSDynamicTriShape:
            case Nif::RC_BSMeshLODTriShape:
            case Nif::RC_BSSubIndexTriShape:
                return true;
            default:
                return false;
        }
    }

    [[nodiscard]] std::optional<std::uint32_t> sourceId(const Nif::Record& record) noexcept
    {
        if (record.mRecordIndex == std::numeric_limits<unsigned int>::max())
            return std::nullopt;
        return static_cast<std::uint32_t>(record.mRecordIndex);
    }

    [[nodiscard]] std::string sourceType(const Nif::Record& record)
    {
        if (!record.mRecordName.empty())
            return record.mRecordName;
        return "record:" + std::to_string(static_cast<unsigned int>(record.mRecordType));
    }

    struct RootPolicy
    {
        bool marker = false;
        bool recursiveCollision = false;
        const Nif::NiNode* collisionRoot = nullptr;
    };

    struct WalkContext
    {
        bool marker = false;
        bool collisionOnly = false;
        bool hiddenByAncestor = false;
    };

    class Translator final
    {
    public:
        Translator(Nif::FileView file, NifRender::TranslatorOptions options)
            : mFile(file)
            , mOptions(options)
        {
            mResult.sourceIdentity = std::string(file.getFilename().value());
            mResult.contentIdentity = file.getHash();
            mResult.model.sourceIdentity = mResult.sourceIdentity;
            mResult.model.contentIdentity = mResult.contentIdentity;
        }

        [[nodiscard]] NifRender::TranslationBundle run()
        {
            const std::size_t rootCount = mFile.numRoots();
            bool foundRoot = false;
            for (std::size_t rootIndex = 0; rootIndex < rootCount; ++rootIndex)
            {
                const Nif::Record* record = mFile.getRoot(rootIndex);
                if (record == nullptr)
                    continue;

                const auto* node = dynamic_cast<const Nif::NiAVObject*>(record);
                if (node == nullptr)
                {
                    classify(*record, NifRender::TranslationDisposition::Unsupported);
                    diagnose(*record, NifRender::DiagnosticSeverity::Error, "root.not_avobject",
                        "NIF root is not an NiAVObject and cannot participate in the neutral model graph");
                    continue;
                }

                foundRoot = true;
                const RootPolicy policy = inspectRoot(*node);
                WalkContext context;
                context.marker = policy.marker && !mOptions.showMarkers;
                const RenderCore::ModelNodeIndex root = translateNode(*node, {}, context, policy.collisionRoot);
                if (root.valid())
                    mResult.model.roots.push_back(root);
            }

            if (!foundRoot)
            {
                NifRender::TranslationDiagnostic diagnostic;
                diagnostic.severity = NifRender::DiagnosticSeverity::Error;
                diagnostic.sourceRecordType = "NIFFile";
                diagnostic.code = "file.no_avobject_roots";
                diagnostic.message = "NIF contains no NiAVObject root records";
                mResult.diagnostics.push_back(std::move(diagnostic));
            }

            return std::move(mResult);
        }

    private:
        [[nodiscard]] RootPolicy inspectRoot(const Nif::NiAVObject& root)
        {
            RootPolicy result;
            for (const Nif::ExtraPtr& extra : root.getExtraList())
            {
                if (extra.empty())
                    continue;

                if (extra->mRecordType == Nif::RC_NiStringExtraData)
                {
                    const auto* value = static_cast<const Nif::NiStringExtraData*>(extra.getPtr());
                    if (value->mData == "MRK")
                        result.marker = true;
                    else if (value->mData == "RCN")
                        result.recursiveCollision = true;
                }
                else if (extra->mRecordType == Nif::RC_BSXFlags)
                {
                    const auto* value = static_cast<const Nif::NiIntegerExtraData*>(extra.getPtr());
                    if ((value->mData & 32u) != 0)
                        result.marker = true;
                }
            }

            if (const auto* node = dynamic_cast<const Nif::NiNode*>(&root))
                result.collisionRoot = node->findRootCollisionNode(result.recursiveCollision);
            return result;
        }

        void classify(const Nif::Record& record, NifRender::TranslationDisposition disposition)
        {
            const auto id = sourceId(record);
            if (id && !mClassifiedRecords.insert(*id).second)
                return;

            NifRender::TranslationOutcome outcome;
            outcome.disposition = disposition;
            outcome.sourceRecordId = id;
            outcome.sourceRecordType = sourceType(record);
            mResult.outcomes.push_back(std::move(outcome));
        }

        void diagnose(const Nif::Record& record, NifRender::DiagnosticSeverity severity, std::string code,
            std::string message)
        {
            NifRender::TranslationDiagnostic diagnostic;
            diagnostic.severity = severity;
            diagnostic.sourceRecordId = sourceId(record);
            diagnostic.sourceRecordType = sourceType(record);
            diagnostic.code = std::move(code);
            diagnostic.message = std::move(message);
            mResult.diagnostics.push_back(std::move(diagnostic));
        }

        [[nodiscard]] bool hasVisibilityController(const Nif::NiAVObject& node) const noexcept
        {
            for (Nif::NiTimeControllerPtr controller = node.mController; !controller.empty();
                 controller = controller->mNext)
            {
                if (controller->mRecordType == Nif::RC_NiVisController)
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool classifyControllers(const Nif::NiAVObject& node)
        {
            bool hasActiveController = false;
            for (Nif::NiTimeControllerPtr controller = node.mController; !controller.empty();
                 controller = controller->mNext)
            {
                if (!controller->isActive())
                    continue;
                hasActiveController = true;
                classify(*controller.getPtr(), NifRender::TranslationDisposition::Deferred);
                diagnose(*controller.getPtr(), NifRender::DiagnosticSeverity::Info, "controller.playback_deferred",
                    "Controller metadata target is preserved, but dynamic playback is deferred to CP3D");
            }
            return hasActiveController;
        }

        [[nodiscard]] bool shouldSkipMarkerGeometry(const Nif::NiAVObject& node, bool marker) const
        {
            if (!marker)
                return false;
            if (mFile.getVersion() <= Nif::NIFFile::VER_MW)
                return Misc::StringUtils::ciStartsWith(node.mName, "tri editormarker");
            return Misc::StringUtils::ciStartsWith(node.mName, "EditorMarker")
                || Misc::StringUtils::ciStartsWith(node.mName, "VisibilityEditorMarker");
        }

        [[nodiscard]] bool shouldSkipLegacyShadowGeometry(const Nif::NiAVObject& node) const
        {
            return mFile.getVersion() <= Nif::NIFFile::VER_MW
                && (Misc::StringUtils::ciStartsWith(node.mName, "shadow")
                    || Misc::StringUtils::ciStartsWith(node.mName, "tri shadow"));
        }

        [[nodiscard]] RenderCore::ModelBillboardMode billboardMode(const Nif::NiBillboardNode& node)
        {
            using SourceMode = Nif::NiBillboardNode::BillboardMode;
            switch (node.mMode)
            {
                case SourceMode::AlwaysFaceCamera:
                    return RenderCore::ModelBillboardMode::AlwaysFaceCamera;
                case SourceMode::RotateAboutUp:
                    return RenderCore::ModelBillboardMode::RotateAboutUp;
                case SourceMode::RigidFaceCamera:
                    return RenderCore::ModelBillboardMode::RigidFaceCamera;
                case SourceMode::BSRotateAboutUp:
                    return RenderCore::ModelBillboardMode::RotateAboutUpBethesda;
                case SourceMode::AlwaysFaceCenter:
                case SourceMode::RigidFaceCenter:
                    diagnose(node, NifRender::DiagnosticSeverity::Warning, "billboard.v325_fallback",
                        "V3.25 falls back to rigid face-camera behavior for this authored billboard mode");
                    return RenderCore::ModelBillboardMode::RigidFaceCamera;
            }
            diagnose(node, NifRender::DiagnosticSeverity::Warning, "billboard.unknown_fallback",
                "Unknown billboard mode uses the V3.25 rigid face-camera fallback");
            return RenderCore::ModelBillboardMode::RigidFaceCamera;
        }

        [[nodiscard]] RenderCore::ModelSortSemantic sortSemantic(const Nif::NiSortAdjustNode& node)
        {
            RenderCore::ModelSortSemantic result;
            using SourceMode = Nif::NiSortAdjustNode::SortingMode;
            switch (node.mMode)
            {
                case SourceMode::Inherit:
                    result.mode = RenderCore::ModelSortMode::Inherit;
                    break;
                case SourceMode::Off:
                    result.mode = RenderCore::ModelSortMode::Off;
                    break;
                case SourceMode::Subsort:
                    result.mode = RenderCore::ModelSortMode::Subsort;
                    break;
            }

            if (node.mSubSorter.empty())
            {
                result.accumulator = RenderCore::ModelSortAccumulator::Missing;
                diagnose(node, NifRender::DiagnosticSeverity::Warning, "sort.missing_accumulator",
                    "NiSortAdjustNode has no accumulator; V3.25 leaves the previous sort scope unchanged");
            }
            else if (node.mSubSorter->mRecordType == Nif::RC_NiAlphaAccumulator)
                result.accumulator = RenderCore::ModelSortAccumulator::Alpha;
            else if (node.mSubSorter->mRecordType == Nif::RC_NiClusterAccumulator)
                result.accumulator = RenderCore::ModelSortAccumulator::Cluster;
            else
            {
                result.accumulator = RenderCore::ModelSortAccumulator::Unsupported;
                diagnose(*node.mSubSorter.getPtr(), NifRender::DiagnosticSeverity::Error,
                    "sort.unsupported_accumulator", "Unsupported NiSortAdjustNode accumulator cannot be silently dropped");
                classify(*node.mSubSorter.getPtr(), NifRender::TranslationDisposition::Unsupported);
            }
            return result;
        }

        [[nodiscard]] RenderCore::ModelNodeIndex translateNode(const Nif::NiAVObject& source,
            RenderCore::ModelNodeIndex parent, WalkContext context, const Nif::NiNode* collisionRoot)
        {
            if (parent.valid() && Misc::StringUtils::ciEqual(source.mName, "Bounding Box"))
            {
                classify(source, NifRender::TranslationDisposition::Ignored);
                diagnose(source, NifRender::DiagnosticSeverity::Info, "node.bounding_box_ignored",
                    "V3.25 ignores child nodes named Bounding Box in the render model");
                return {};
            }

            if (&source == collisionRoot)
                context.collisionOnly = true;

            const bool hidden = source.isHidden() || context.hiddenByAncestor;
            const bool hiddenWithoutController = hidden && !hasVisibilityController(source);
            const bool hasController = classifyControllers(source);

            NifRender::TranslatedModelNode node;
            node.name = source.mName;
            node.sourceRecordId = sourceId(source);
            node.parent = parent;
            node.localTransform = toGlm(source.mTransform);

            if (hidden)
                node.flags |= RenderCore::modelNodeFlag(RenderCore::ModelNodeFlag::Hidden);
            if (context.marker)
                node.flags |= RenderCore::modelNodeFlag(RenderCore::ModelNodeFlag::Marker);
            if (context.collisionOnly)
            {
                node.flags |= RenderCore::modelNodeFlag(RenderCore::ModelNodeFlag::Collision);
                node.flags |= RenderCore::modelNodeFlag(RenderCore::ModelNodeFlag::CollisionOnly);
            }
            else if (source.hasMeshCollision() || source.hasBBoxCollision() || source.collisionActive())
                node.flags |= RenderCore::modelNodeFlag(RenderCore::ModelNodeFlag::Collision);
            if (hasController)
                node.flags |= RenderCore::modelNodeFlag(RenderCore::ModelNodeFlag::ControllerTarget);

            const bool legacyGeometry = isLegacyGeometry(source.mRecordType);
            const bool bethesdaGeometry = isBethesdaGeometry(source.mRecordType);
            const bool sourceGeometry = legacyGeometry || bethesdaGeometry;
            const bool markerGeometry = shouldSkipMarkerGeometry(source, context.marker);
            const bool shadowGeometry = shouldSkipLegacyShadowGeometry(source);
            const bool omitVisibleGeometry
                = (!context.collisionOnly && hiddenWithoutController) || markerGeometry || shadowGeometry;

            if (source.mRecordType == Nif::RC_NiParticles)
            {
                node.kind = RenderCore::ModelNodeKind::Transform;
                classify(source, NifRender::TranslationDisposition::Deferred);
                diagnose(source, NifRender::DiagnosticSeverity::Info, "particles.realization_deferred",
                    "Particle-system playback and realization are deferred to the dynamic renderer checkpoint");
            }
            else if (sourceGeometry && !omitVisibleGeometry)
            {
                const std::optional<NifRender::MeshIndex> mesh
                    = legacyGeometry ? translateLegacyGeometry(source) : translateBethesdaGeometry(source);
                if (mesh)
                {
                    node.kind = RenderCore::ModelNodeKind::Geometry;
                    node.mesh = *mesh;
                    classify(source, context.collisionOnly ? NifRender::TranslationDisposition::CollisionOnly
                                                           : NifRender::TranslationDisposition::Rendered);
                }
                else
                {
                    node.kind = RenderCore::ModelNodeKind::Transform;
                    classify(source, NifRender::TranslationDisposition::Unsupported);
                }
            }
            else if (sourceGeometry)
            {
                node.kind = RenderCore::ModelNodeKind::Transform;
                if (markerGeometry || hiddenWithoutController)
                    classify(source, NifRender::TranslationDisposition::Hidden);
                else
                    classify(source, NifRender::TranslationDisposition::Ignored);
                if (shadowGeometry)
                    diagnose(source, NifRender::DiagnosticSeverity::Info, "geometry.legacy_shadow_proxy_ignored",
                        "V3.25 intentionally does not render legacy geometry named shadow/tri shadow");
            }
            else if (source.mRecordType == Nif::RC_NiLODNode)
            {
                node.kind = RenderCore::ModelNodeKind::Lod;
                const auto& lod = static_cast<const Nif::NiLODNode&>(source);
                RenderCore::ModelLodSemantic semantic;
                semantic.center = toGlm(lod.mLODCenter);
                node.lod = std::move(semantic);
                classify(source, hasController ? NifRender::TranslationDisposition::Deferred
                                               : NifRender::TranslationDisposition::Rendered);
            }
            else if (source.mRecordType == Nif::RC_NiFltAnimationNode)
            {
                node.kind = RenderCore::ModelNodeKind::Switch;
                classify(source, NifRender::TranslationDisposition::Deferred);
                diagnose(source, NifRender::DiagnosticSeverity::Info, "sequence.playback_deferred",
                    "NiFltAnimationNode sequence playback is preserved as hierarchy metadata and deferred to CP3D");
            }
            else if (source.mRecordType == Nif::RC_NiSwitchNode)
            {
                node.kind = RenderCore::ModelNodeKind::Switch;
                classify(source, hasController ? NifRender::TranslationDisposition::Deferred
                                               : NifRender::TranslationDisposition::Rendered);
            }
            else if (source.mRecordType == Nif::RC_NiBillboardNode)
            {
                node.kind = RenderCore::ModelNodeKind::Billboard;
                node.billboard = billboardMode(static_cast<const Nif::NiBillboardNode&>(source));
                classify(source, NifRender::TranslationDisposition::Rendered);
            }
            else if (source.mRecordType == Nif::RC_NiSortAdjustNode)
            {
                node.kind = RenderCore::ModelNodeKind::Sort;
                node.sort = sortSemantic(static_cast<const Nif::NiSortAdjustNode&>(source));
                classify(source, NifRender::TranslationDisposition::Rendered);
            }
            else
            {
                node.kind = RenderCore::ModelNodeKind::Transform;
                classify(source, hasController ? NifRender::TranslationDisposition::Deferred
                                               : (hidden ? NifRender::TranslationDisposition::Hidden
                                                         : NifRender::TranslationDisposition::Rendered));
            }

            const auto nodeIndex
                = RenderCore::ModelNodeIndex{ static_cast<std::uint32_t>(mResult.model.nodes.size()) };
            mResult.model.nodes.push_back(std::move(node));

            const auto* group = dynamic_cast<const Nif::NiNode*>(&source);
            if (group == nullptr)
                return nodeIndex;

            std::vector<std::optional<RenderCore::ModelNodeIndex>> childMap(group->mChildren.size());
            WalkContext childContext = context;
            childContext.hiddenByAncestor = hiddenWithoutController && !context.collisionOnly;
            for (std::size_t sourceChildIndex = 0; sourceChildIndex < group->mChildren.size(); ++sourceChildIndex)
            {
                const Nif::NiAVObjectPtr& child = group->mChildren[sourceChildIndex];
                if (child.empty())
                    continue;
                const RenderCore::ModelNodeIndex translated
                    = translateNode(*child.getPtr(), nodeIndex, childContext, collisionRoot);
                if (translated.valid())
                    childMap[sourceChildIndex] = translated;
            }

            NifRender::TranslatedModelNode& publishedNode = mResult.model.nodes[nodeIndex.value()];
            if (source.mRecordType == Nif::RC_NiSwitchNode || source.mRecordType == Nif::RC_NiFltAnimationNode)
            {
                const auto& switchNode = static_cast<const Nif::NiSwitchNode&>(source);
                if (switchNode.mInitialIndex < childMap.size() && childMap[switchNode.mInitialIndex])
                    publishedNode.activeSwitchChild = *childMap[switchNode.mInitialIndex];
                else if (!childMap.empty())
                    diagnose(source, NifRender::DiagnosticSeverity::Warning, "switch.initial_child_missing",
                        "Initial switch index does not identify a translated direct child");
            }
            else if (source.mRecordType == Nif::RC_NiLODNode)
            {
                const auto& lodNode = static_cast<const Nif::NiLODNode&>(source);
                auto& semantic = *publishedNode.lod;
                for (std::size_t sourceChildIndex = 0; sourceChildIndex < childMap.size(); ++sourceChildIndex)
                {
                    if (!childMap[sourceChildIndex])
                        continue;
                    RenderCore::ModelLodRange range;
                    range.child = *childMap[sourceChildIndex];
                    if (sourceChildIndex < lodNode.mLODLevels.size())
                    {
                        range.minimumDistance = lodNode.mLODLevels[sourceChildIndex].mMinRange;
                        range.maximumDistance = lodNode.mLODLevels[sourceChildIndex].mMaxRange;
                    }
                    else
                    {
                        diagnose(source, NifRender::DiagnosticSeverity::Error, "lod.range_missing",
                            "LOD child has no corresponding source range; publication must fail closed");
                    }
                    semantic.ranges.push_back(range);
                }
                if (lodNode.mLODLevels.size() != childMap.size())
                    diagnose(source, NifRender::DiagnosticSeverity::Warning, "lod.range_count_mismatch",
                        "LOD source range count differs from the authored child slot count");
            }

            for (const Nif::NiAVObjectPtr& effect : group->mEffects)
            {
                if (effect.empty())
                    continue;
                classify(*effect.getPtr(), NifRender::TranslationDisposition::Deferred);
                diagnose(*effect.getPtr(), NifRender::DiagnosticSeverity::Info, "effect.realization_deferred",
                    "Node effect semantics will be translated with the material/effect CP3B1 slice");
            }

            return nodeIndex;
        }

        void appendSurface(RenderCore::MeshPayload& payload, RenderCore::PrimitiveTopology topology,
            const std::vector<unsigned short>& sourceIndices)
        {
            if (sourceIndices.empty())
                return;
            RenderCore::MeshSurface surface;
            surface.topology = topology;
            surface.firstIndex = static_cast<std::uint32_t>(payload.indices.size());
            surface.indexCount = static_cast<std::uint32_t>(sourceIndices.size());
            payload.indices.reserve(payload.indices.size() + sourceIndices.size());
            for (const unsigned short index : sourceIndices)
                payload.indices.push_back(index);
            payload.surfaces.push_back(surface);
        }

        [[nodiscard]] std::optional<NifRender::MeshIndex> translateLegacyGeometry(const Nif::NiAVObject& source)
        {
            const auto& geometry = static_cast<const Nif::NiGeometry&>(source);
            if (geometry.mData.empty())
            {
                diagnose(source, NifRender::DiagnosticSeverity::Warning, "geometry.missing_data",
                    "Geometry has no NiGeometryData and produces no drawable payload");
                return std::nullopt;
            }

            const Nif::NiGeometryData& data = *geometry.mData.getPtr();
            if (data.mVertices.empty())
            {
                diagnose(source, NifRender::DiagnosticSeverity::Warning, "geometry.empty_vertices",
                    "Geometry has no vertices and produces no drawable payload");
                return std::nullopt;
            }

            auto payload = std::make_shared<RenderCore::MeshPayload>();
            payload->positions.reserve(data.mVertices.size());
            for (const osg::Vec3f& value : data.mVertices)
                payload->positions.push_back(toGlm(value));
            payload->normals.reserve(data.mNormals.size());
            for (const osg::Vec3f& value : data.mNormals)
                payload->normals.push_back(toGlm(value));
            payload->tangents.reserve(data.mTangents.size());
            for (const osg::Vec3f& value : data.mTangents)
                payload->tangents.push_back(toGlm(value));
            payload->bitangents.reserve(data.mBitangents.size());
            for (const osg::Vec3f& value : data.mBitangents)
                payload->bitangents.push_back(toGlm(value));
            payload->colors.reserve(data.mColors.size());
            for (const osg::Vec4f& value : data.mColors)
                payload->colors.push_back(toGlm(value));
            payload->texCoordSets.reserve(data.mUVList.size());
            for (const auto& sourceSet : data.mUVList)
            {
                auto& destinationSet = payload->texCoordSets.emplace_back();
                destinationSet.reserve(sourceSet.size());
                for (const osg::Vec2f& value : sourceSet)
                    destinationSet.push_back(toGlm(value));
            }

            bool usedSkinPartitions = false;
            if (!geometry.mSkin.empty())
            {
                const Nif::NiSkinPartition* partitions = geometry.mSkin->getPartitions();
                if (partitions != nullptr)
                {
                    usedSkinPartitions = true;
                    for (const Nif::NiSkinPartition::Partition& partition : partitions->mPartitions)
                    {
                        appendSurface(*payload, RenderCore::PrimitiveTopology::Triangles, partition.mTrueTriangles);
                        for (const auto& strip : partition.mTrueStrips)
                        {
                            if (strip.size() >= 3)
                                appendSurface(*payload, RenderCore::PrimitiveTopology::TriangleStrip, strip);
                        }
                    }
                }
            }

            if (!usedSkinPartitions)
            {
                if (source.mRecordType == Nif::RC_NiTriShape || source.mRecordType == Nif::RC_BSLODTriShape)
                {
                    const auto& triangles = static_cast<const Nif::NiTriShapeData&>(data).mTriangles;
                    appendSurface(*payload, RenderCore::PrimitiveTopology::Triangles, triangles);
                }
                else if (source.mRecordType == Nif::RC_NiTriStrips)
                {
                    const auto& strips = static_cast<const Nif::NiTriStripsData&>(data).mStrips;
                    for (const auto& strip : strips)
                    {
                        if (strip.size() >= 3)
                            appendSurface(*payload, RenderCore::PrimitiveTopology::TriangleStrip, strip);
                    }
                }
                else if (source.mRecordType == Nif::RC_NiLines)
                {
                    appendSurface(*payload, RenderCore::PrimitiveTopology::Lines,
                        static_cast<const Nif::NiLinesData&>(data).mLines);
                }
                else if (source.mRecordType == Nif::RC_BSSegmentedTriShape)
                {
                    diagnose(source, NifRender::DiagnosticSeverity::Error, "geometry.segmented_unpartitioned",
                        "V3.25 does not establish an unpartitioned BSSegmentedTriShape primitive path; do not invent one");
                    return std::nullopt;
                }
            }

            if (payload->surfaces.empty())
            {
                diagnose(source, NifRender::DiagnosticSeverity::Warning, "geometry.empty_primitives",
                    "Geometry contains no supported non-empty primitive sets");
                return std::nullopt;
            }

            RenderCore::MeshRecord record;
            record.sourceIdentity
                = mResult.sourceIdentity + "#mesh:" + std::to_string(static_cast<unsigned int>(source.mRecordIndex));
            record.surfaceCount = static_cast<std::uint32_t>(payload->surfaces.size());
            record.skinned = !geometry.mSkin.empty();
            record.payload = std::move(payload);
            updateBounds(record.bounds, record.payload->positions);

            for (Nif::NiTimeControllerPtr controller = source.mController; !controller.empty();
                 controller = controller->mNext)
            {
                if (controller->isActive() && controller->mRecordType == Nif::RC_NiGeomMorpherController)
                {
                    record.morphed = true;
                    break;
                }
            }

            if (record.skinned)
                diagnose(source, NifRender::DiagnosticSeverity::Info, "geometry.skinning_deferred",
                    "Skin metadata source is recognized; dynamic skin realization is deferred to CP3D");
            if (record.morphed)
                diagnose(source, NifRender::DiagnosticSeverity::Info, "geometry.morph_deferred",
                    "Morph metadata source is recognized; morph playback is deferred to CP3D");

            const auto index = NifRender::MeshIndex{ static_cast<std::uint32_t>(mResult.meshes.size()) };
            NifRender::TranslatedMesh translated;
            translated.record = std::move(record);
            translated.sourceRecordId = sourceId(source);
            mResult.meshes.push_back(std::move(translated));
            return index;
        }

        [[nodiscard]] std::optional<NifRender::MeshIndex> translateBethesdaGeometry(const Nif::NiAVObject& source)
        {
            const auto& geometry = static_cast<const Nif::BSTriShape&>(source);
            if (geometry.mTriangles.empty())
            {
                diagnose(source, NifRender::DiagnosticSeverity::Warning, "bsgeometry.empty_triangles",
                    "BSTriShape contains no triangle indices");
                return std::nullopt;
            }

            const bool fullPrecision
                = (geometry.mVertDesc.mFlags & Nif::BSVertexDesc::VertexAttribute::Full_Precision) != 0;
            const bool hasVertices = (geometry.mVertDesc.mFlags & Nif::BSVertexDesc::VertexAttribute::Vertex) != 0;
            const bool hasNormals = (geometry.mVertDesc.mFlags & Nif::BSVertexDesc::VertexAttribute::Normals) != 0;
            const bool hasTangents = (geometry.mVertDesc.mFlags & Nif::BSVertexDesc::VertexAttribute::Tangents) != 0;
            const bool hasColors = (geometry.mVertDesc.mFlags & Nif::BSVertexDesc::VertexAttribute::Vertex_Colors) != 0;
            const bool hasUv = (geometry.mVertDesc.mFlags & Nif::BSVertexDesc::VertexAttribute::UVs) != 0;
            const bool hasSecondUv = (geometry.mVertDesc.mFlags & Nif::BSVertexDesc::VertexAttribute::UVs_2) != 0;

            if (!hasVertices || geometry.mVertData.empty())
            {
                diagnose(source, NifRender::DiagnosticSeverity::Error, "bsgeometry.missing_vertices",
                    "BSTriShape has triangle indices but no translatable vertex stream");
                return std::nullopt;
            }

            auto payload = std::make_shared<RenderCore::MeshPayload>();
            payload->positions.reserve(geometry.mVertData.size());
            if (hasNormals)
                payload->normals.reserve(geometry.mVertData.size());
            if (hasTangents)
            {
                payload->tangents.reserve(geometry.mVertData.size());
                payload->bitangents.reserve(geometry.mVertData.size());
            }
            if (hasColors)
                payload->colors.reserve(geometry.mVertData.size());
            if (hasUv)
                payload->texCoordSets.emplace_back().reserve(geometry.mVertData.size());

            for (const Nif::BSVertexData& vertex : geometry.mVertData)
            {
                if (fullPrecision)
                    payload->positions.emplace_back(vertex.mVertex.x(), vertex.mVertex.y(), vertex.mVertex.z());
                else
                    payload->positions.emplace_back(halfToFloat(vertex.mHalfVertex[0]),
                        halfToFloat(vertex.mHalfVertex[1]), halfToFloat(vertex.mHalfVertex[2]));

                if (hasNormals)
                    payload->normals.emplace_back(normalByteToFloat(vertex.mNormal[0]),
                        normalByteToFloat(vertex.mNormal[1]), normalByteToFloat(vertex.mNormal[2]));
                if (hasTangents)
                {
                    payload->tangents.emplace_back(normalByteToFloat(vertex.mTangent[0]),
                        normalByteToFloat(vertex.mTangent[1]), normalByteToFloat(vertex.mTangent[2]));
                    const float bitangentX
                        = fullPrecision ? vertex.mVertex.w() : halfToFloat(vertex.mHalfVertex[3]);
                    payload->bitangents.emplace_back(bitangentX, normalByteToFloat(vertex.mNormal[3]),
                        normalByteToFloat(vertex.mTangent[3]));
                }
                if (hasColors)
                    payload->colors.push_back(colorByteToFloat(vertex.mVertColor));
                if (hasUv)
                    payload->texCoordSets.front().emplace_back(
                        halfToFloat(vertex.mUV[0]), halfToFloat(vertex.mUV[1]));
            }

            appendSurface(*payload, RenderCore::PrimitiveTopology::Triangles, geometry.mTriangles);

            RenderCore::MeshRecord record;
            record.sourceIdentity
                = mResult.sourceIdentity + "#mesh:" + std::to_string(static_cast<unsigned int>(source.mRecordIndex));
            record.surfaceCount = static_cast<std::uint32_t>(payload->surfaces.size());
            record.skinned = !geometry.mSkin.empty();
            record.payload = std::move(payload);
            updateBounds(record.bounds, record.payload->positions);

            if (hasSecondUv)
                diagnose(source, NifRender::DiagnosticSeverity::Warning, "bsgeometry.second_uv_deferred",
                    "Packed second UV stream is declared but not exposed by the current parser vertex record; never silently alias it");
            if (record.skinned)
                diagnose(source, NifRender::DiagnosticSeverity::Info, "bsgeometry.skinning_deferred",
                    "Bethesda skin metadata is recognized; dynamic skin realization is deferred to CP3D");
            if (source.mRecordType == Nif::RC_BSDynamicTriShape)
                diagnose(source, NifRender::DiagnosticSeverity::Info, "bsgeometry.dynamic_positions_deferred",
                    "BSDynamicTriShape dynamic position playback is deferred; static source vertex data is retained");
            if (source.mRecordType == Nif::RC_BSMeshLODTriShape || source.mRecordType == Nif::RC_BSSubIndexTriShape)
                diagnose(source, NifRender::DiagnosticSeverity::Warning, "bsgeometry.substructure_deferred",
                    "Subtype-specific LOD/segmentation metadata still requires explicit CP3B translation before closeout");

            const auto index = NifRender::MeshIndex{ static_cast<std::uint32_t>(mResult.meshes.size()) };
            NifRender::TranslatedMesh translated;
            translated.record = std::move(record);
            translated.sourceRecordId = sourceId(source);
            mResult.meshes.push_back(std::move(translated));
            return index;
        }

        Nif::FileView mFile;
        NifRender::TranslatorOptions mOptions;
        NifRender::TranslationBundle mResult;
        std::unordered_set<std::uint32_t> mClassifiedRecords;
    };
}

namespace NifRender
{
    TranslationBundle translateNif(Nif::FileView file, TranslatorOptions options)
    {
        return Translator(file, options).run();
    }
}

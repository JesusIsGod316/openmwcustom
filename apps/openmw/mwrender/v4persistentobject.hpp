#ifndef OPENMW_MWRENDER_V4PERSISTENTOBJECT_H
#define OPENMW_MWRENDER_V4PERSISTENTOBJECT_H

#include "v4effectcapture.hpp"
#include <components/rendercore/persistentdraw.hpp>
#include <components/nifosg/matrixtransform.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/rendermutation.hpp>
#include <components/sceneutil/skeleton.hpp>
#include <osg/MatrixTransform>
#include <osg/PositionAttitudeTransform>
#include <atomic>

namespace MWRender
{
    // Owned by ObjectAnimation, built after the engine imports its NIF and
    // installs controllers. Live bindings are used ONLY on the update thread.
    // Published frames contain values and immutable meshes, never these nodes.
    class V4PersistentObject
    {
    public:
        explicit V4PersistentObject(osg::Node& root, std::size_t budget = 64u * 1024u * 1024u)
            : mBudget(budget)
        {
            osg::NodePath path;
            mSupported = append(root, NoParent, 0, path);
            std::size_t bytes = mNodes.capacity() * sizeof(Binding) + mDrawCount * 4096;
            for (const auto& b : mNodes)
                bytes += b.path.capacity() * sizeof(osg::Node*) + b.children.capacity() * sizeof(osg::Node*)
                    + b.controllers.capacity() * sizeof(Controller);
            if (mSupported && !charge(bytes))
                reject("producer budget");
            if (!mSupported) std::vector<Binding>().swap(mNodes);
        }
        ~V4PersistentObject() { sBytes.fetch_sub(mCharged, std::memory_order_relaxed); }
        V4PersistentObject(const V4PersistentObject&) = delete;
        V4PersistentObject& operator=(const V4PersistentObject&) = delete;
        bool supported() const noexcept { return mSupported; }
        const std::string& fallbackReason() const noexcept { return mFallback; }
        static std::size_t retainedBytes() { return sBytes.load(std::memory_order_relaxed); }
        std::uint64_t geometryBuilds = 0, materialUpdates = 0, uvUpdates = 0, reusedDraws = 0;

        std::optional<V4EffectCaptureResult> publish(const std::string& prefix,
            const VFS::Manager& vfs, NifRender::TextureIdentityCache& identities,
            RenderCore::PersistentDrawWorld* persistent = nullptr, bool environmentPreLight = false)
        {
            mPublishingWorld = persistent;
            if (!mSupported) return {};
            if (mPrefix.empty()) mPrefix = prefix;
            else if (mPrefix != prefix) return fallback("instance identity replaced");
            auto& counters = Debug::GameplayDiagnostics::context;
            Debug::GameplayDiagnostics::CapturePhase phase(counters.nativeBindingMs);
            // Flat pointer/edge guards protect engine attachment replacement.
            // This never scans vertex streams, material values or inherited state.
            for (const auto& b : mNodes)
            {
                if (!b.node.valid() || b.node->getUpdateCallback() != b.callbackHead
                    || (b.group && b.group->getNumChildren() != b.children.size()))
                    return fallback("binding replaced");
                for (unsigned i = 0; i < b.children.size(); ++i)
                    if (b.group->getChild(i) != b.children[i]) return fallback("attachment replaced");
                for (const auto& c : b.controllers)
                    if (!c.callback.valid() || c.callback->getNestedCallback() != c.next)
                        return fallback("controller chain replaced");
                    else if (c.source->renderMutationMask() & SceneUtil::RenderUntracked)
                        return fallback("untracked controller added");
                if (b.node->getStateSet() && b.node->getStateSet()->requiresUpdateTraversal())
                    return fallback("state update callback added");
            }
            V4EffectCaptureResult result;
            if (!persistent) result.draws.reserve(mDrawCount);
            std::size_t ordinal = 0;
            for (auto& b : mNodes)
            {
                b.dirty = 0;
                bool transformChanged = !b.initialized;
                b.active = b.node->getNodeMask() != 0;
                if (b.parent != NoParent)
                {
                    const auto& p = mNodes[b.parent];
                    transformChanged |= p.transformChanged;
                    b.active = b.active && p.active && (!p.selection || p.selection->getValue(b.childIndex));
                    b.dirty = p.dirty;
                }
                // Engine movement setters publish revisions. Other supported
                // OSG transform classes use a cheap matrix-value adapter so an
                // inherited setMatrix call cannot bypass notification coverage.
                if (b.transform)
                {
                    const auto revision = b.transformSource ? b.transformSource->renderMutationRevision() : 0;
                    const auto referenceFrame = b.transform->getReferenceFrame();
                    if (!b.transformSource || !b.initialized || revision != b.transformRevision
                        || referenceFrame != b.referenceFrame)
                    {
                        osg::Matrix local;
                        b.transform->computeLocalToWorldMatrix(local, nullptr);
                        transformChanged |= local != b.local || referenceFrame != b.referenceFrame;
                        b.local = local; b.referenceFrame = referenceFrame; b.transformRevision = revision;
                    }
                }
                if (transformChanged)
                    b.matrix = b.parent != NoParent && b.referenceFrame == osg::Transform::RELATIVE_RF
                        ? b.local * mNodes[b.parent].matrix : b.local;
                b.transformChanged = transformChanged;
                b.initialized = true;
                for (auto& c : b.controllers)
                {
                    const auto revision = c.source->renderMutationRevision();
                    if (revision != c.revision) b.dirty |= c.source->renderMutationMask();
                    c.revision = revision;
                }
                const auto* state = b.node->getStateSet();
                if (state != b.state) b.dirty |= SceneUtil::RenderMaterial | SceneUtil::RenderTexCoords;
                b.state = state;
                if (!b.geometry) continue;
                const auto drawOrdinal = ordinal++;
                // Dirty state must survive hidden frames until this draw is used.
                b.pending |= b.dirty;
                if (!b.active)
                {
                    if (persistent) persistent->hide(b.handle);
                    continue;
                }
                bool resourceChanged = false;
                if (!b.draw.meshSnapshot)
                {
                    resourceChanged = true;
                    const auto identity = prefix + ":geometry:" + std::to_string(drawOrdinal);
                    if (!v4_effect_detail::captureGeometry(*b.geometry, b.path, vfs, identity, b.draw,
                            result.diagnostic, &identities, false, &b.matrix)) return result;
                    if (!freeze(b.draw, b.meshBytes)) return fallback("mesh budget");
                    bindTextures(b, identities);
                    ++geometryBuilds;
                    b.pending = 0;
                }
                else if (b.pending & (SceneUtil::RenderMaterial | SceneUtil::RenderTexCoords))
                {
                    resourceChanged = true;
                    // Controllers have known bindings: only affected material/UV
                    // fields are evaluated. Positions/indices are NEVER recaptured.
                    const auto effective = v4_effect_detail::effectiveState(b.path, b.geometry->getStateSet());
                    v4_effect_detail::CapturedMaterial material;
                    if (!v4_effect_detail::captureMaterial(b.path, b.geometry->getStateSet(), vfs,
                            material, result.diagnostic, &identities, false, effective.get())) return result;
                    auto updated = b.draw;
                    updated.material = std::move(material.material);
                    updated.material.sourceIdentity = b.draw.material.sourceIdentity;
                    updated.textures = std::move(material.textures);
                    // captureMaterial returns authored texture units; remap only
                    // the UV streams, preserving arbitrary TexMat and flip units.
                    if (!v4_effect_detail::captureEffectTextureCoordinates(*b.geometry, *effective,
                            updated, result.diagnostic)) return result;
                    const auto& oldMesh = b.draw.meshSnapshot->mesh();
                    if (updated.mesh.texCoordSets != oldMesh.texCoordSets)
                    {
                        auto uv = std::move(updated.mesh.texCoordSets);
                        updated.mesh = oldMesh;
                        updated.mesh.texCoordSets = std::move(uv);
                        updated.meshSnapshot.reset();
                        // Changed UV layouts can grow or shrink. Account for
                        // the replacement before publishing; old frames still
                        // own their snapshots through GPU-safe retirement.
                        if (!freeze(updated, b.meshBytes)) return fallback("updated mesh budget");
                        ++uvUpdates;
                    }
                    updated.mesh = {};
                    b.draw = std::move(updated);
                    bindTextures(b, identities);
                    ++materialUpdates;
                    b.pending = 0;
                }
                else ++reusedDraws;
                b.draw.worldTransform = v4_effect_detail::toGlm(b.matrix);
                if (!v4_effect_detail::finite(b.draw.worldTransform))
                { result.diagnostic = "persistent object has a non-finite transform"; return result; }
                // Identity slots either follow the engine asset's loaded
                // lifetime or the independently selectable per-frame control.
                // Neither path rediscovers materials or decodes images here.
                phase.next(counters.nativeIdentityMs);
                for (std::size_t i = 0; i < b.draw.textures.size(); ++i)
                {
                    auto& t = b.draw.textures[i];
                    if (t.texture.pixels) return fallback("procedural texture");
                    const auto& resolved = Misc::environmentFlag<"OPENMW_V4_LOAD_BOUND_TEXTURES">()
                        ? identities.resolveLoaded(*b.textureBindings[i])
                        : identities.resolveBound(*b.textureBindings[i]);
                    if (!resolved.valid()) return fallback("texture identity unavailable");
                    if (t.texture.contentIdentity != resolved.contentIdentity)
                    {
                        resourceChanged = true;
                        t.texture.contentIdentity = resolved.contentIdentity;
                    }
                    if (t.texture.sourceIdentity != resolved.canonicalPath.value())
                    {
                        resourceChanged = true;
                        t.texture.sourceIdentity = resolved.canonicalPath.value();
                    }
                }
                phase.next(counters.nativeCopyMs);
                if (persistent)
                {
                    b.draw.semanticFlags = RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::OrdinaryWorld)
                        | RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::ShadowCaster)
                        | RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::ReflectionEligible)
                        | RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::RefractionEligible);
                    resourceChanged |= b.draw.material.environmentMapPreLight != environmentPreLight;
                    b.draw.material.environmentMapPreLight = environmentPreLight;
                    if (!persistent->update(b.handle, b.draw, resourceChanged)) return fallback("persistent slot budget");
                }
                else result.draws.push_back(b.draw);
                phase.next(counters.nativeBindingMs);
            }
            return result;
        }

    private:
        static constexpr std::size_t NoParent = std::numeric_limits<std::size_t>::max();
        struct Controller
        {
            osg::observer_ptr<osg::Callback> callback;
            const osg::Callback* next;
            SceneUtil::RenderMutationSource* source;
            std::uint64_t revision;
        };
        struct Binding
        {
            osg::observer_ptr<osg::Node> node;
            osg::Group* group = nullptr;
            osg::Transform* transform = nullptr;
            osg::Switch* selection = nullptr;
            osg::Geometry* geometry = nullptr;
            SceneUtil::RenderMutationSource* transformSource = nullptr;
            std::uint64_t transformRevision = 0;
            osg::Transform::ReferenceFrame referenceFrame = osg::Transform::RELATIVE_RF;
            const osg::Callback* callbackHead = nullptr;
            const osg::StateSet* state = nullptr;
            std::vector<const osg::Node*> children;
            std::vector<Controller> controllers;
            std::vector<std::shared_ptr<NifRender::TextureIdentityCache::Binding>> textureBindings;
            osg::NodePath path;
            std::size_t parent;
            std::size_t meshBytes = 0;
            unsigned childIndex;
            unsigned dirty = 0, pending = 0;
            bool active = false, initialized = false, transformChanged = false;
            osg::Matrix matrix, local;
            RenderCore::ImmediateEffectDraw draw;
            RenderCore::PersistentDrawHandle handle;
        };
        bool reject(std::string reason) { mSupported = false; mFallback = std::move(reason); return false; }
        static void bindTextures(Binding& b, NifRender::TextureIdentityCache& identities)
        {
            b.textureBindings.clear();
            for (const auto& texture : b.draw.textures)
            {
                const VFS::Path::Normalized path(texture.texture.sourceIdentity);
                b.textureBindings.push_back(Misc::environmentFlag<"OPENMW_V4_LOAD_BOUND_TEXTURES">()
                    ? identities.bindLoaded(path) : identities.bind(path));
            }
        }
        std::optional<V4EffectCaptureResult> fallback(std::string reason)
        {
            reject(std::move(reason));
            if (mPublishingWorld) for (const auto& b : mNodes) mPublishingWorld->remove(b.handle);
            std::vector<Binding>().swap(mNodes); // submitted immutable frames survive
            sBytes.fetch_sub(mCharged, std::memory_order_relaxed); mCharged = 0;
            return {};
        }
        bool charge(std::size_t bytes)
        {
            auto previous = sBytes.load(std::memory_order_relaxed);
            do { if (bytes > mBudget || previous > mBudget - bytes) return false; }
            while (!sBytes.compare_exchange_weak(previous, previous + bytes, std::memory_order_relaxed));
            mCharged += bytes; return true;
        }
        bool freeze(RenderCore::ImmediateEffectDraw& draw, std::size_t& lease)
        {
            const auto& mesh = draw.meshData();
            // Charge the privately retained streams, including both tangent
            // arrays. Do not reserve four full UV/tangent streams for every
            // untextured mesh: a changed layout acquires its additional lease
            // here before it becomes visible. Shared mesh owners are counted
            // conservatively per producer, not retained indefinitely in a cache.
            auto bytes = sizeof(RenderCore::FrozenEffectMesh)
                + (mesh.positions.size() + mesh.normals.size() + mesh.tangents.size()
                    + mesh.bitangents.size()) * sizeof(glm::vec3)
                + mesh.colors.size() * sizeof(glm::vec4)
                + mesh.indices.size() * sizeof(std::uint32_t)
                + mesh.surfaces.size() * sizeof(RenderCore::MeshSurface)
                + mesh.texCoordSets.size() * sizeof(std::vector<glm::vec2>);
            for (const auto& uv : mesh.texCoordSets) bytes += uv.size() * sizeof(glm::vec2);
            if (bytes > lease && !charge(bytes - lease)) return false;
            if (bytes < lease)
            {
                sBytes.fetch_sub(lease - bytes, std::memory_order_relaxed);
                mCharged -= lease - bytes;
            }
            lease = bytes;
            if (!draw.meshSnapshot) draw.meshSnapshot = std::make_shared<const RenderCore::FrozenEffectMesh>(mesh);
            draw.mesh = {};
            draw.bounds = draw.meshSnapshot->bounds();
            return draw.meshSnapshot->valid();
        }
        bool append(osg::Node& node, std::size_t parent, unsigned child, osg::NodePath& path)
        {
            if (parent != NoParent && v4_effect_detail::isEffectRoot(node)) return true;
            if (mNodes.size() >= 512 || path.size() >= 64
                || std::find(path.begin(), path.end(), &node) != path.end()) return reject("hierarchy limit");
            const auto& type = typeid(node);
            if (type != typeid(osg::Node) && type != typeid(osg::Group) && type != typeid(osg::Geode)
                && type != typeid(osg::Geometry) && type != typeid(osg::Switch)
                && type != typeid(osg::MatrixTransform) && type != typeid(NifOsg::MatrixTransform)
                && type != typeid(SceneUtil::Skeleton)
                && type != typeid(osg::PositionAttitudeTransform)
                && type != typeid(SceneUtil::PositionAttitudeTransform))
                return reject(std::string("node:") + node.className());
            Binding b;
            b.node = &node; b.parent = parent; b.childIndex = child;
            b.group = node.asGroup(); b.transform = dynamic_cast<osg::Transform*>(&node);
            b.transformSource = dynamic_cast<SceneUtil::RenderMutationSource*>(&node);
            if (b.transformSource) b.transformSource->watchRenderMutations();
            b.selection = dynamic_cast<osg::Switch*>(&node);
            b.geometry = dynamic_cast<osg::Geometry*>(&node);
            b.callbackHead = node.getUpdateCallback(); b.state = node.getStateSet();
            if (b.state && b.state->requiresUpdateTraversal()) return reject("state update callback");
            for (auto* c = node.getUpdateCallback(); c; c = c->getNestedCallback())
            {
                auto* source = dynamic_cast<SceneUtil::RenderMutationSource*>(c);
                if (!source || (source->renderMutationMask() & SceneUtil::RenderUntracked))
                    return reject(std::string("controller:") + c->className());
                source->watchRenderMutations();
                b.controllers.push_back({c, c->getNestedCallback(), source, source->renderMutationRevision()});
            }
            path.push_back(&node); b.path = path;
            if (b.geometry) ++mDrawCount;
            if (b.group) for (unsigned i = 0; i < b.group->getNumChildren(); ++i) b.children.push_back(b.group->getChild(i));
            const auto index = mNodes.size(); mNodes.push_back(std::move(b));
            if (auto* group = node.asGroup())
                for (unsigned i = 0; i < group->getNumChildren(); ++i)
                    if (!append(*group->getChild(i), index, i, path)) return false;
            path.pop_back(); return true;
        }
        inline static std::atomic<std::size_t> sBytes{0};
        const std::size_t mBudget;
        std::size_t mCharged = 0, mDrawCount = 0;
        bool mSupported = false;
        std::string mFallback;
        std::string mPrefix;
        std::vector<Binding> mNodes;
        // Borrowed only during publish/fallback on the update thread.
        RenderCore::PersistentDrawWorld* mPublishingWorld = nullptr;
    };
}
#endif

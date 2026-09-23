#include "v4skycapture.hpp"
#include "v4effectcapture.hpp"
#include <osg/Array>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/PrimitiveSet>
#include <osg/Texture2D>
#include <map>
#include <stdexcept>

namespace MWRender
{
    V4SkyCapture::GeometrySnapshot V4SkyCapture::geometry(const osg::Geometry& source, std::string& diagnostic)
    {
        using namespace RenderCore;
        using namespace v4_effect_detail;
        std::vector<std::uint64_t> signature;
        const auto observe = [&](const osg::BufferData* data) {
            signature.push_back(reinterpret_cast<std::uintptr_t>(data));
            signature.push_back(data ? data->getModifiedCount() : 0);
            signature.push_back(data ? data->getTotalDataSize() : 0);
        };
        observe(source.getVertexArray()); observe(source.getColorArray()); observe(source.getTexCoordArray(0));
        signature.push_back(source.getNumPrimitiveSets());
        for (const auto& primitive : source.getPrimitiveSetList()) observe(primitive);
        auto& cached = mGeometry[&source];
        if (cached.source.valid() && cached.signature == signature && cached.snapshot.mesh)
            return cached.snapshot;
        const auto* vertices = dynamic_cast<const osg::Vec3Array*>(source.getVertexArray());
        if (!vertices || vertices->empty())
        { diagnostic = "native sky geometry requires a nonempty authored Vec3 vertex array"; return {}; }
        auto mesh = std::make_shared<MeshPayload>();
        mesh->positions.reserve(vertices->size());
        for (const auto& v : *vertices) mesh->positions.push_back(toGlm(v));
        mesh->colors.assign(vertices->size(), glm::vec4(1.f));
        if (const auto* colors = dynamic_cast<const osg::Vec4Array*>(source.getColorArray()))
        {
            if (colors->size() == 1) std::fill(mesh->colors.begin(), mesh->colors.end(), toGlm(colors->front()));
            else if (colors->size() == vertices->size())
                for (std::size_t i = 0; i < colors->size(); ++i) mesh->colors[i] = toGlm((*colors)[i]);
            else if (!colors->empty())
            { diagnostic = "native sky has unsupported non-vertex colour binding"; return {}; }
        }
        else if (source.getColorArray())
        { diagnostic = "native sky has an unsupported colour array type"; return {}; }
        mesh->texCoordSets.emplace_back(vertices->size(), glm::vec2(0.f));
        if (const auto* uv = dynamic_cast<const osg::Vec2Array*>(source.getTexCoordArray(0)))
        {
            if (uv->size() != vertices->size())
            { diagnostic = "native sky texture coordinates do not match its authored vertices"; return {}; }
            for (std::size_t i = 0; i < uv->size(); ++i) mesh->texCoordSets[0][i] = {(*uv)[i].x(), (*uv)[i].y()};
        }
        else if (source.getTexCoordArray(0))
        { diagnostic = "native sky requires Vec2 authored texture coordinates"; return {}; }
        for (const auto& primitive : source.getPrimitiveSetList())
            if (!primitive || !appendPrimitive(*primitive, *mesh, diagnostic)) return {};
        if (!validMeshPayload(*mesh))
        { diagnostic = "native sky mesh failed semantic validation"; return {}; }
        if (mNextIdentity == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("native sky mesh identity exhausted");
        const auto identity = cached.source.valid() ? cached.snapshot.identity : mNextIdentity++;
        cached = {osg::observer_ptr<const osg::Geometry>(&source), std::move(signature), {identity, std::move(mesh)}};
        return cached.snapshot;
    }

    namespace
    {
        class SkyVisitor final : public osg::NodeVisitor
        {
        public:
            SkyVisitor(V4SkyCapture& capture, NifRender::TextureIdentityCache& textures)
                : osg::NodeVisitor(TRAVERSE_ACTIVE_CHILDREN), mCapture(capture), mTextures(textures) {}
            void apply(osg::Geode& geode) override
            {
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                    if (auto* drawable = geode.getDrawable(i)) drawable->accept(*this);
            }
            void apply(osg::Drawable& drawable) override
            {
                if (!diagnostic.empty()) return;
                auto* geometry = drawable.asGeometry();
                if (!geometry) { diagnostic = "native sky drawable is not geometry: " + drawable.getName(); return; }
                using namespace v4_effect_detail;
                using namespace RenderCore;
                auto state = effectiveState(getNodePath(), geometry->getStateSet());
                int pass = -1;
                if (const auto* uniform = state->getUniform("pass")) uniform->get(pass);
                // These require view-dependent occlusion results, not the stale
                // cull-callback output of the update-only legacy viewer.
                if (pass == 5 || pass == 6) { ++snapshot.deferredOcclusionDraws; return; }
                if (pass < 0 || pass > 4)
                { diagnostic = "native sky geometry has unsupported pass " + std::to_string(pass); return; }
                const auto mesh = mCapture.geometry(*geometry, diagnostic);
                if (!mesh.mesh) return;
                SkyDrawSnapshot draw;
                draw.identity = "native-sky:" + std::to_string(mesh.identity) + ":"
                    + std::to_string(mOccurrences[mesh.identity]++);
                draw.pass = static_cast<SkyPass>(pass);
                draw.mesh = mesh.mesh;
                draw.transform = toGlm(osg::computeLocalToWorld(getNodePath()));
                const auto vec4 = [&](const char* name, glm::vec4 fallback) {
                    osg::Vec4f value;
                    if (const auto* uniform = state->getUniform(name); uniform && uniform->get(value)) return toGlm(value);
                    return fallback;
                };
                draw.diffuseColor = vec4("diffuseColor", glm::vec4(1.f));
                draw.moonBlend = vec4("moonBlend", glm::vec4(0.f));
                draw.atmosphereFade = vec4("atmosphereFade", glm::vec4(0.f));
                if (const auto* uniform = state->getUniform("opacity")) uniform->get(draw.opacity);
                if (draw.pass == SkyPass::Clouds)
                    if (const auto* matrix = dynamic_cast<const osg::TexMat*>(
                        state->getTextureAttribute(0, osg::StateAttribute::TEXMAT))) draw.uvTransform = toGlm(matrix->getMatrix());
                draw.blendEnabled = stateEnabled(*state, GL_BLEND, true);
                if (const auto* blend = dynamic_cast<const osg::BlendFunc*>(state->getAttribute(osg::StateAttribute::BLENDFUNC)))
                {
                    const auto source = blendFactor(blend->getSource());
                    const auto destination = blendFactor(blend->getDestination());
                    if (!source || !destination)
                    { diagnostic = "native sky blend factors unsupported"; return; }
                    draw.sourceBlend = *source; draw.destinationBlend = *destination;
                }
                draw.cullMode = stateEnabled(*state, GL_CULL_FACE, true) ? CullMode::Back : CullMode::None;
                if (const auto* cull = dynamic_cast<const osg::CullFace*>(state->getAttribute(osg::StateAttribute::CULLFACE));
                    cull && draw.cullMode != CullMode::None)
                {
                    if (cull->getMode() == osg::CullFace::FRONT) draw.cullMode = CullMode::Front;
                    else if (cull->getMode() != osg::CullFace::BACK)
                    { diagnostic = "native sky culls both faces"; return; }
                }
                if (const auto* front = dynamic_cast<const osg::FrontFace*>(state->getAttribute(osg::StateAttribute::FRONTFACE)))
                    draw.frontFace = front->getMode() == osg::FrontFace::CLOCKWISE
                        ? FrontFaceWinding::Clockwise : FrontFaceWinding::CounterClockwise;
                const unsigned count = pass == 0 ? 0u : pass == 3 ? 2u : 1u;
                if (count && !geometry->getTexCoordArray(0))
                { diagnostic = "textured native sky geometry has no authored UVs"; return; }
                for (unsigned unit = 0; unit < count; ++unit)
                {
                    const auto* texture = dynamic_cast<const osg::Texture2D*>(
                        state->getTextureAttribute(unit, osg::StateAttribute::TEXTURE));
                    const auto* image = texture ? texture->getImage() : nullptr;
                    if (!image || image->getFileName().empty() || image->s() <= 0 || image->t() <= 0)
                    { diagnostic = "native sky texture lacks a winning VFS image identity"; return; }
                    const auto identity = mTextures.resolve(VFS::Path::Normalized(image->getFileName()));
                    if (!identity.valid()) { diagnostic = "native sky VFS texture resolution failed"; return; }
                    EffectTextureSnapshot captured;
                    captured.texture.sourceIdentity = std::string(identity.canonicalPath.value());
                    captured.texture.contentIdentity = identity.contentIdentity;
                    captured.texture.revision = InitialResourceRevision;
                    captured.texture.width = static_cast<std::uint32_t>(image->s());
                    captured.texture.height = static_cast<std::uint32_t>(image->t());
                    captured.texture.mipmapped = image->getNumMipmapLevels() > 1;
                    captured.binding.role = TextureRole::Diffuse;
                    captured.binding.colorSpace = TextureColorSpace::Srgb;
                    textureFilter(*texture, captured.binding.sampler);
                    draw.textures.push_back(std::move(captured));
                }
                if (!validSkyDraw(draw)) { diagnostic = "native sky draw failed validation: " + draw.identity; return; }
                snapshot.draws.push_back(std::move(draw));
            }
            RenderCore::NativeSkySnapshot snapshot;
            std::string diagnostic;
        private:
            V4SkyCapture& mCapture;
            NifRender::TextureIdentityCache& mTextures;
            std::map<std::uint64_t, unsigned int> mOccurrences;
        };
    }
    V4SkyCapture::Result V4SkyCapture::capture(osg::Group& root, NifRender::TextureIdentityCache& textures)
    {
        std::erase_if(mGeometry, [](const auto& entry) { return !entry.second.source.valid(); });
        SkyVisitor visitor(*this, textures);
        NifRender::TextureIdentityCache::CaptureScope captureScope(textures);
        root.accept(visitor);
        if (!visitor.diagnostic.empty()) return {{}, std::move(visitor.diagnostic)};
        return {std::make_shared<const RenderCore::NativeSkySnapshot>(std::move(visitor.snapshot)), {}};
    }

}

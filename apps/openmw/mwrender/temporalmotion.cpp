#include "temporalmotion.hpp"
#include "temporaldynamic.hpp"

#include <components/rendercore/temporalframe.hpp>
#include <osg/ColorMask>
#include <osg/FrameBufferObject>
#include <osg/FrameStamp>
#include <osg/Geometry>
#include <osg/GLExtensions>
#include <osg/Multisample>
#include <osg/PolygonMode>
#include <osg/RenderInfo>
#include <osg/State>
#include <osg/StateSet>
#include <osg/Uniform>
#include <osg/observer_ptr>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace MWRender
{
    namespace
    {
        bool selected(const char* name)
        {
            const char* value = std::getenv(name);
            return value && std::strcmp(value, "1") == 0;
        }
        glm::dmat4 toColumnMatrix(const osg::Matrixd& matrix)
        {
            glm::dmat4 result;
            // OSG uses row-vector logical matrices; GLM uses column vectors.
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r) result[c][r] = matrix(c, r);
            return result;
        }
        osg::Matrixf toOsgMatrix(const glm::dmat4& matrix)
        {
            osg::Matrixf result;
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r) result(c, r) = static_cast<float>(matrix[c][r]);
            return result;
        }
        // Do not treat per-frame near/far adjustment as a camera cut. Changes
        // in the focal scale/asymmetry/projection family do invalidate history.
        bool lensChanged(const osg::Matrixd& a, const osg::Matrixd& b)
        {
            for (int row = 0; row < 4; ++row)
                for (int col = 0; col < 4; ++col)
                    if (col != 2 && a(row, col) != b(row, col)) return true;
            return false;
        }
        // Use the EXT/ARB spellings supplied by OSG on Windows. They have
        // the same values as the core aliases; keep separate read/draw state.
        struct RestoreDrawState
        {
            osg::State& state;
            osg::GLExtensions& ext;
            GLint draw = 0, read = 0;
            std::array<GLint, 4> viewport{};
            bool pushed = false;
            RestoreDrawState(osg::State& s, osg::GLExtensions& e) : state(s), ext(e)
            {
                glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING_EXT, &draw);
                glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING_EXT, &read);
                glGetIntegerv(GL_VIEWPORT, viewport.data());
            }
            ~RestoreDrawState()
            {
                if (pushed) { state.popStateSet(); state.apply(); }
                ext.glBindFramebuffer(GL_DRAW_FRAMEBUFFER_EXT, static_cast<GLuint>(draw));
                ext.glBindFramebuffer(GL_READ_FRAMEBUFFER_EXT, static_cast<GLuint>(read));
                glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            }
        };
    }

    struct TemporalMotion::Impl
    {
        struct Context
        {
            struct PreviousSurface
            {
                TemporalDynamicFrame::Surface surface;
                osg::Matrixd projection;
            };
            RenderCore::Temporal::History history;
            osg::ref_ptr<osg::Texture2D> motion;
            osg::ref_ptr<osg::FrameBufferObject> fbo;
            osg::ref_ptr<osg::StateSet> state;
            osg::ref_ptr<osg::Uniform> transform, extent, clear, reset, depthRange;
            osg::Matrixd lastProjection;
            std::uint64_t lensEpoch = 1, targetRevision = 0, resourceEpoch = 0;
            bool haveProjection = false, programFailed = false;
            Status status;
            ConsumerFrame consumer;
            osg::ref_ptr<osg::StateSet> dynamicState;
            std::vector<PreviousSurface> previousSurfaces;
        };
        osg::ref_ptr<osg::Program> program;
        osg::ref_ptr<osg::Program> dynamicProgram;
        // The integration is mono/single-context initially. Additional contexts
        // can have independent state here, but are never silently aliased.
        std::array<std::unique_ptr<Context>, 16> contexts;
        Impl(osg::Program* value, osg::Program* dynamicValue) : program(value), dynamicProgram(dynamicValue)
        {
            if (dynamicProgram) dynamicProgram->addBindAttribLocation("previousPosition", 6);
        }

        Context& get(unsigned id)
        {
            auto& result = contexts[id];
            if (!result)
            {
                result = std::make_unique<Context>();
                auto& c = *result;
                c.state = new osg::StateSet;
                c.state->setAttributeAndModes(program, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
                c.state->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
                c.state->setMode(GL_BLEND, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
                c.state->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
                c.state->setMode(GL_SCISSOR_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
                c.state->setMode(GL_STENCIL_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
                c.state->setMode(GL_ALPHA_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
                c.state->setMode(GL_SAMPLE_ALPHA_TO_COVERAGE_ARB, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
                c.state->setAttribute(new osg::ColorMask(true, true, true, true), osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
                c.state->setAttribute(new osg::PolygonMode(osg::PolygonMode::FRONT_AND_BACK, osg::PolygonMode::FILL),
                    osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
                c.transform = new osg::Uniform("clipToPreviousClip", osg::Matrixf{});
                c.extent = new osg::Uniform("renderSize", osg::Vec2f{});
                c.clear = new osg::Uniform("clearDepth", 1.0f);
                c.reset = new osg::Uniform("resetHistory", true);
                c.depthRange = new osg::Uniform("depthZeroToOne", false);
                for (auto* u : {c.transform.get(), c.extent.get(), c.clear.get(), c.reset.get(), c.depthRange.get()})
                    c.state->addUniform(u);
                c.state->addUniform(new osg::Uniform("temporalDepth", 0));
                c.state->addUniform(new osg::Uniform("jitterPixels", osg::Vec2f(0, 0)));
            }
            return *result;
        }
    };

    TemporalMotion::TemporalMotion(osg::Program* program, osg::Program* dynamicProgram)
        : mImpl(std::make_unique<Impl>(program, dynamicProgram)) {}
    TemporalMotion::~TemporalMotion() = default;
    bool TemporalMotion::enabled()
    {
        static const bool value = selected("OPENMW_P9_TEMPORAL_INPUTS") || selected("OPENMW_P9_MOTION_VIEW")
            || selected("OPENMW_P9_DYNAMIC_MOTION");
        return value;
    }
    bool TemporalMotion::debugView()
    {
        static const bool value = selected("OPENMW_P9_MOTION_VIEW");
        return value;
    }
    bool TemporalMotion::ownershipEnabled()
    {
        static const bool value = selected("OPENMW_P9_TEMPORAL_OWNERSHIP");
        return value;
    }

    osg::Texture2D* TemporalMotion::render(osg::RenderInfo& info, const TemporalCamera& camera,
        osg::Texture2D* depth, const osg::Geometry& fullscreen)
    {
        osg::State* state = info.getState();
        if (!state || state->getContextID() >= mImpl->contexts.size() || !mImpl->program) return nullptr;
        const auto id = state->getContextID();
        auto& c = mImpl->get(id);
        c.status = {};
        c.status.frame = camera.frame;
        c.consumer = {};
        const auto* stamp = state->getFrameStamp();
        if (!camera.projection || !depth || !stamp || stamp->getFrameNumber() != camera.frame
            || depth == c.motion.get()
            || !camera.renderWidth || !camera.renderHeight || !camera.outputWidth || !camera.outputHeight
            || camera.renderWidth > camera.outputWidth || camera.renderHeight > camera.outputHeight
            || camera.renderWidth != static_cast<unsigned>(depth->getTextureWidth())
            || camera.renderHeight != static_cast<unsigned>(depth->getTextureHeight())
            || !std::isfinite(camera.clearDepth) || (camera.clearDepth != 0.0 && camera.clearDepth != 1.0))
        {
            c.history.invalidate();
            return nullptr;
        }
        auto* ext = state->get<osg::GLExtensions>();
        if (!ext || !ext->isFrameBufferObjectSupported || !ext->glCheckFramebufferStatus
            || !ext->glBindFramebuffer || c.programFailed)
        {
            c.history.invalidate();
            return nullptr;
        }

        // Copy only here, after the world cull finalized the retained matrix.
        const osg::Matrixd projection(*camera.projection);
        if ((c.haveProjection && lensChanged(projection, c.lastProjection))
            || (c.resourceEpoch && c.resourceEpoch != camera.resourceEpoch)) ++c.lensEpoch;
        c.resourceEpoch = camera.resourceEpoch;
        c.lastProjection = projection;
        c.haveProjection = true;
        RenderCore::Temporal::FrameInput input;
        input.identity = {1, camera.worldEpoch, camera.cameraEpoch, c.lensEpoch};
        input.frame = camera.frame;
        input.render = {camera.renderWidth, camera.renderHeight};
        input.output = {camera.outputWidth, camera.outputHeight};
        input.view = toColumnMatrix(camera.view);
        input.projection = toColumnMatrix(projection);
        input.depthRange = camera.zeroToOne ? RenderCore::Temporal::DepthRange::ZeroToOne
            : RenderCore::Temporal::DepthRange::NegativeOneToOne;
        // Until a reconstruction consumer is present, ordinary gameplay must
        // not be jittered merely to record motion. Halton/jitter remains tested
        // in the common core; enabling scene jitter is a separate later gate.
        input.jitterEnabled = false;
        auto frame = c.history.prepare(input);
        if (!frame) return nullptr;

        struct PendingTicket
        {
            RenderCore::Temporal::History& history;
            std::uint64_t ticket;
            ~PendingTicket() { history.abort(ticket); }
        } pending{c.history, frame->ticket};
        RestoreDrawState restore(*state, *ext);
        if (!c.motion || c.motion->getTextureWidth() != static_cast<int>(camera.renderWidth)
            || c.motion->getTextureHeight() != static_cast<int>(camera.renderHeight))
        {
            c.motion = new osg::Texture2D;
            c.motion->setTextureSize(camera.renderWidth, camera.renderHeight);
            c.motion->setInternalFormat(GL_RG16F);
            c.motion->setSourceFormat(GL_RG);
            c.motion->setSourceType(GL_FLOAT);
            c.motion->setResizeNonPowerOfTwoHint(false);
            c.motion->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
            c.motion->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
            c.motion->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            c.motion->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            c.fbo = new osg::FrameBufferObject;
            c.fbo->setAttachment(osg::Camera::COLOR_BUFFER0, osg::FrameBufferAttachment(c.motion));
            ++c.targetRevision;
        }
        c.fbo->apply(*state);
        if (ext->glCheckFramebufferStatus(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT)
        {
            c.history.abort(frame->ticket);
            c.history.invalidate();
            return nullptr;
        }
        glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
        c.transform->set(toOsgMatrix(frame->clipToPreviousClip));
        c.extent->set(osg::Vec2f(static_cast<float>(camera.renderWidth), static_cast<float>(camera.renderHeight)));
        c.clear->set(static_cast<float>(camera.clearDepth));
        c.reset->set(!frame->hasHistory());
        c.depthRange->set(camera.zeroToOne);
        c.state->setTextureAttributeAndModes(0, depth, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
        state->pushStateSet(c.state);
        restore.pushed = true;
        state->apply();
        auto* pcp = mImpl->program->getPCP(*state);
        if (!pcp || !pcp->isLinked())
        {
            c.programFailed = true;
            c.history.abort(frame->ticket);
            c.history.invalidate();
            return nullptr;
        }
        glViewport(0, 0, camera.renderWidth, camera.renderHeight);
        fullscreen.osg::Geometry::drawImplementation(info);
        unsigned dynamicSubmitted = 0;
        unsigned dynamicUnsupported = camera.dynamic ? camera.dynamic->unsupported : 0;
        std::vector<Impl::Context::PreviousSurface> nextSurfaces;
        if (camera.dynamic && mImpl->dynamicProgram)
        {
            if (!c.dynamicState)
            {
                c.dynamicState = new osg::StateSet(*c.state, osg::CopyOp::DEEP_COPY_UNIFORMS);
                c.dynamicState->setAttributeAndModes(mImpl->dynamicProgram,
                    osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
                c.dynamicState->addUniform(new osg::Uniform("currentModelViewProjection", osg::Matrixf{}));
                c.dynamicState->addUniform(new osg::Uniform("previousModelViewProjection", osg::Matrixf{}));
            }
            c.dynamicState->setTextureAttributeAndModes(0, depth,
                osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
            c.dynamicState->getUniform("renderSize")->set(osg::Vec2f(
                static_cast<float>(camera.renderWidth), static_cast<float>(camera.renderHeight)));
            c.dynamicState->getUniform("clearDepth")->set(static_cast<float>(camera.clearDepth));
            nextSurfaces.reserve(camera.dynamic->surfaces.size());
            for (const auto& surface : camera.dynamic->surfaces)
            {
                const auto previous = std::find_if(c.previousSurfaces.begin(), c.previousSurfaces.end(), [&](const auto& value) {
                    return value.surface.instance == surface.instance && value.surface.topology == surface.topology
                        && (surface.deformation ? value.surface.deformation == surface.deformation
                            : value.surface.owner == surface.owner);
                });
                const bool history = frame->hasHistory() && previous != c.previousSurfaces.end()
                    && previous->surface.geometry->getVertexArray()->getNumElements()
                        == surface.geometry->getVertexArray()->getNumElements();
                const auto currentProjection = osg::Matrixd(*surface.projection);
                c.dynamicState->getUniform("currentModelViewProjection")->set(osg::Matrixf(surface.modelView * currentProjection));
                c.dynamicState->getUniform("previousModelViewProjection")->set(osg::Matrixf(history
                    ? previous->surface.modelView * previous->projection : surface.modelView * currentProjection));
                c.dynamicState->getUniform("resetHistory")->set(!history);
                surface.geometry->setVertexAttribArray(6, history ? previous->surface.geometry->getVertexArray()
                    : surface.geometry->getVertexArray(), osg::Array::BIND_PER_VERTEX);
                state->pushStateSet(c.dynamicState);
                state->apply();
                auto* dynamicPcp = mImpl->dynamicProgram->getPCP(*state);
                if (dynamicPcp && dynamicPcp->isLinked())
                {
                    surface.geometry->osg::Geometry::drawImplementation(info);
                    ++dynamicSubmitted;
                    nextSurfaces.push_back({surface, currentProjection});
                }
                else ++dynamicUnsupported;
                state->popStateSet();
                state->apply();
                surface.geometry->setVertexAttribArray(6, nullptr);
            }
        }
        else if (camera.dynamic) dynamicUnsupported += static_cast<unsigned>(camera.dynamic->surfaces.size());
        if (!c.history.commit(frame->ticket)) return nullptr;
        c.previousSurfaces = std::move(nextSurfaces);

        c.status.frame = camera.frame;
        c.status.previousFrame = frame->previousFrame;
        c.status.targetRevision = c.targetRevision;
        c.status.resetReasons = frame->resetReasons;
        c.status.renderWidth = camera.renderWidth;
        c.status.renderHeight = camera.renderHeight;
        c.status.outputWidth = camera.outputWidth;
        c.status.outputHeight = camera.outputHeight;
        c.status.jitterPixels = osg::Vec2f(
            static_cast<float>(frame->jitterPixels.x), static_cast<float>(frame->jitterPixels.y));
        c.status.previousJitterPixels = osg::Vec2f(
            static_cast<float>(frame->previousJitterPixels.x), static_cast<float>(frame->previousJitterPixels.y));
        c.status.submitted = true;
        c.status.historyValid = frame->hasHistory();
        c.status.denseDynamicMotion = false;
        // Opaque rigid/CPU rig/morph surfaces are now overlaid with submitted
        // previous poses. Transparent/cutout/wind/effect/first-person coverage
        // remains explicit; no complete dense/DLSS claim is made.
        c.status.dynamicSurfaces = dynamicSubmitted;
        c.status.unsupportedSurfaces = dynamicUnsupported;

        c.consumer.status = c.status;
        c.consumer.motion = c.motion;
        c.consumer.currentViewProjection = RenderCore::Temporal::rowMajor(frame->currentViewProjection);
        c.consumer.previousViewProjection = RenderCore::Temporal::rowMajor(frame->previousViewProjection);
        c.consumer.inverseViewProjection = RenderCore::Temporal::rowMajor(frame->inverseViewProjection);
        c.consumer.motionInPixels = true;
        return c.motion;
    }

    TemporalMotion::Status TemporalMotion::status(unsigned context) const
    {
        if (context >= mImpl->contexts.size() || !mImpl->contexts[context]) return {};
        return mImpl->contexts[context]->status;
    }

    std::optional<TemporalMotion::ConsumerFrame> TemporalMotion::consumerFrame(unsigned context,
        std::uint64_t expectedFrame) const
    {
        if (context >= mImpl->contexts.size() || !mImpl->contexts[context]
            || !mImpl->contexts[context]->consumer.status.submitted
            || mImpl->contexts[context]->consumer.status.frame != expectedFrame
            || !mImpl->contexts[context]->consumer.motion)
            return std::nullopt;
        return mImpl->contexts[context]->consumer;
    }

    void TemporalMotion::resizeGLObjectBuffers(unsigned size)
    {
        if (mImpl->program) mImpl->program->resizeGLObjectBuffers(size);
        for (auto& c : mImpl->contexts) if (c)
        {
            c->state->resizeGLObjectBuffers(size);
            if (c->fbo) c->fbo->resizeGLObjectBuffers(size);
        }
    }
    void TemporalMotion::releaseGLObjects(osg::State* state)
    {
        if (mImpl->program) mImpl->program->releaseGLObjects(state);
        for (unsigned i = 0; i < mImpl->contexts.size(); ++i)
            if (mImpl->contexts[i] && (!state || state->getContextID() == i))
            {
                auto& c = *mImpl->contexts[i];
                c.history.invalidate();
                c.status = {};
                c.consumer = {};
                c.previousSurfaces.clear();
                if (c.dynamicState) c.dynamicState->releaseGLObjects(state);
                c.dynamicState = nullptr;
                c.haveProjection = false;
                c.resourceEpoch = 0;
                c.programFailed = false;
                if (c.fbo) c.fbo->releaseGLObjects(state);
                if (c.motion) c.motion->releaseGLObjects(state);
                c.fbo = nullptr;
                c.motion = nullptr;
                c.state->releaseGLObjects(state);
            }
    }
}

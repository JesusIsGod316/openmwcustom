#include "temporalmotion.hpp"

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
#include <array>
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
            RenderCore::Temporal::History history;
            osg::ref_ptr<osg::Texture2D> motion;
            osg::ref_ptr<osg::FrameBufferObject> fbo;
            osg::ref_ptr<osg::StateSet> state;
            osg::ref_ptr<osg::Uniform> transform, extent, clear, reset, depthRange;
            osg::Matrixd lastProjection;
            std::uint64_t lensEpoch = 1, targetRevision = 0;
            bool haveProjection = false, programFailed = false;
            Status status;
        };
        osg::ref_ptr<osg::Program> program;
        // The integration is mono/single-context initially. Additional contexts
        // can have independent state here, but are never silently aliased.
        std::array<std::unique_ptr<Context>, 16> contexts;
        explicit Impl(osg::Program* value) : program(value) {}

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

    TemporalMotion::TemporalMotion(osg::Program* program) : mImpl(std::make_unique<Impl>(program)) {}
    TemporalMotion::~TemporalMotion() = default;
    bool TemporalMotion::enabled()
    {
        static const bool value = selected("OPENMW_P9_TEMPORAL_INPUTS") || selected("OPENMW_P9_MOTION_VIEW");
        return value;
    }
    bool TemporalMotion::debugView()
    {
        static const bool value = selected("OPENMW_P9_MOTION_VIEW");
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
        if (c.haveProjection && lensChanged(projection, c.lastProjection)) ++c.lensEpoch;
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
        if (!c.history.commit(frame->ticket)) return nullptr;
        c.status = {camera.frame, frame->previousFrame, c.targetRevision, frame->resetReasons, true, false};
        return c.motion;
    }

    TemporalMotion::Status TemporalMotion::status(unsigned context) const
    {
        if (context >= mImpl->contexts.size() || !mImpl->contexts[context]) return {};
        return mImpl->contexts[context]->status;
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
                c.programFailed = false;
                if (c.fbo) c.fbo->releaseGLObjects(state);
                if (c.motion) c.motion->releaseGLObjects(state);
                c.state->releaseGLObjects(state);
            }
    }
}

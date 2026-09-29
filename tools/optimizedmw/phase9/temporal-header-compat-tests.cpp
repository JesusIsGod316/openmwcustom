// Compile-only regression for Windows' legacy OpenGL header surface.
// Preload the real OSG dependencies so a later transitive include cannot
// silently reintroduce Linux's optional core aliases. No fake GL definitions.
#include <apps/openmw/mwrender/temporalmotion.hpp>
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

#ifdef GL_DRAW_FRAMEBUFFER_BINDING
static_assert(GL_DRAW_FRAMEBUFFER_BINDING == GL_DRAW_FRAMEBUFFER_BINDING_EXT);
#undef GL_DRAW_FRAMEBUFFER_BINDING
#endif
#ifdef GL_READ_FRAMEBUFFER_BINDING
static_assert(GL_READ_FRAMEBUFFER_BINDING == GL_READ_FRAMEBUFFER_BINDING_EXT);
#undef GL_READ_FRAMEBUFFER_BINDING
#endif
#ifdef GL_DRAW_FRAMEBUFFER
static_assert(GL_DRAW_FRAMEBUFFER == GL_DRAW_FRAMEBUFFER_EXT);
#undef GL_DRAW_FRAMEBUFFER
#endif
#ifdef GL_READ_FRAMEBUFFER
static_assert(GL_READ_FRAMEBUFFER == GL_READ_FRAMEBUFFER_EXT);
#undef GL_READ_FRAMEBUFFER
#endif
#ifdef GL_FRAMEBUFFER
static_assert(GL_FRAMEBUFFER == GL_FRAMEBUFFER_EXT);
#undef GL_FRAMEBUFFER
#endif
#ifdef GL_FRAMEBUFFER_COMPLETE
static_assert(GL_FRAMEBUFFER_COMPLETE == GL_FRAMEBUFFER_COMPLETE_EXT);
#undef GL_FRAMEBUFFER_COMPLETE
#endif
#ifdef GL_COLOR_ATTACHMENT0
static_assert(GL_COLOR_ATTACHMENT0 == GL_COLOR_ATTACHMENT0_EXT);
#undef GL_COLOR_ATTACHMENT0
#endif
#ifdef GL_SAMPLE_ALPHA_TO_COVERAGE
static_assert(GL_SAMPLE_ALPHA_TO_COVERAGE == GL_SAMPLE_ALPHA_TO_COVERAGE_ARB);
#undef GL_SAMPLE_ALPHA_TO_COVERAGE
#endif

// This translation unit is compiled but never linked into the game or tests.
// The pixel tests still link the ordinary, separately compiled implementation.
#include <apps/openmw/mwrender/temporalmotion.cpp>

#if defined(GL_DRAW_FRAMEBUFFER_BINDING) \
    || defined(GL_READ_FRAMEBUFFER_BINDING) \
    || defined(GL_DRAW_FRAMEBUFFER) \
    || defined(GL_READ_FRAMEBUFFER) \
    || defined(GL_FRAMEBUFFER) \
    || defined(GL_FRAMEBUFFER_COMPLETE) \
    || defined(GL_COLOR_ATTACHMENT0) \
    || defined(GL_SAMPLE_ALPHA_TO_COVERAGE)
#error Optional core GL aliases were reintroduced; the regression is ineffective
#endif

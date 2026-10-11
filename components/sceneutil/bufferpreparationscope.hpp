#ifndef OPENMW_SCENEUTIL_BUFFERPREPARATIONSCOPE_H
#define OPENMW_SCENEUTIL_BUFFERPREPARATIONSCOPE_H

#include <osg/BufferObject>
#include <osg/GLExtensions>
#include <osg/State>
#include <osg/VertexArrayState>

namespace SceneUtil
{
    // compileBuffer binds GL directly. Cached unbind calls are insufficient:
    // they return without touching GL when OSG's cached pointer is null.
    // Restore the caller's known binding state, including its VAO, without GL
    // queries, CPU waits, array mutation, or a change to resource ownership.
    // This scope is for resource compilation, not arbitrary attribute drawing.
    class BufferPreparationScope
    {
    public:
        explicit BufferPreparationScope(osg::State* state) : mState(state)
        {
            if (!state) return;
            mExtensions = state->get<osg::GLExtensions>();
            mArrays = state->getCurrentVertexArrayState();
            if (!mExtensions || !mExtensions->glBindBuffer || !mArrays) return;
            mVertex = mArrays->getCurrentVertexBufferObject();
            mElement = mArrays->getCurrentElementBufferObject();
            mVao = state->getCurrentVertexArrayObject();
            mValid = true;
        }
        BufferPreparationScope(const BufferPreparationScope&) = delete;
        BufferPreparationScope& operator=(const BufferPreparationScope&) = delete;
        ~BufferPreparationScope()
        {
            if (!mValid) return;
            mState->setCurrentVertexArrayState(mArrays);
            mState->bindVertexArrayObject(mVao);
            mExtensions->glBindBuffer(GL_ARRAY_BUFFER_ARB, mVertex ? mVertex->getGLObjectID() : 0);
            mExtensions->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER_ARB, mElement ? mElement->getGLObjectID() : 0);
            mArrays->setCurrentVertexBufferObject(mVertex);
            mArrays->setCurrentElementBufferObject(mElement);
        }

    private:
        osg::State* mState = nullptr;
        osg::GLExtensions* mExtensions = nullptr;
        osg::ref_ptr<osg::VertexArrayState> mArrays;
        osg::ref_ptr<osg::GLBufferObject> mVertex, mElement;
        GLuint mVao = 0;
        bool mValid = false;
    };

    // A client pointer must never be interpreted as an offset into a leftover
    // VBO/EBO. Clear the raw Array dispatch cache as well: its key does not
    // include a change between client storage and VBO storage in pinned OSG.
    inline void prepareClientArrayDraw(osg::State& state)
    {
        auto* arrays = state.getCurrentVertexArrayState();
        auto* extensions = state.get<osg::GLExtensions>();
        if (!arrays || !extensions || !extensions->glBindBuffer) return;
        state.disableAllVertexArrays();
        arrays->setRequiresSetArrays(true);
        extensions->glBindBuffer(GL_ARRAY_BUFFER_ARB, 0);
        extensions->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
        arrays->resetBufferObjectPointers();
    }
}

#endif

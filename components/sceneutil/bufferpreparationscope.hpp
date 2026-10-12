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
    // Native Drawable restores the global VAS pointer while its geometry VAO
    // can remain bound. Its global cached EBO then describes another VAO.
    // Preserve actual GL bindings separately from those native cache pointers.
    // Binding queries are confined to preparation, never repeated draw reuse.
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
            GLint vertex=0,element=0,vao=0;
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING_ARB,&vertex);
            glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING_ARB,&element);
            // GL_VERTEX_ARRAY_BINDING is absent from some pinned SDK headers.
            if (mExtensions->glBindVertexArray) glGetIntegerv(0x85B5,&vao);
            mVertexBinding=static_cast<GLuint>(vertex);
            mElementBinding=static_cast<GLuint>(element);
            mVao=static_cast<GLuint>(vao);
            mValid = true;
        }
        BufferPreparationScope(const BufferPreparationScope&) = delete;
        BufferPreparationScope& operator=(const BufferPreparationScope&) = delete;
        ~BufferPreparationScope()
        {
            if (!mValid) return;
            mState->setCurrentVertexArrayState(mArrays);
            // Compilation may bind a VAO directly without changing State's
            // tracked name. Its cached-equality bind can then skip the actual
            // restoration. Force GL first and update the native tracked name.
            if (mExtensions->glBindVertexArray)
            {
                mExtensions->glBindVertexArray(mVao);
                mState->setCurrentVertexArrayObject(mVao);
            }
            // Contexts without VAO support keep their ordinary global VAS and
            // need only their actual VBO/EBO bindings and caches restored.
            mExtensions->glBindBuffer(GL_ARRAY_BUFFER_ARB, mVertexBinding);
            mExtensions->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER_ARB, mElementBinding);
            mArrays->setCurrentVertexBufferObject(mVertex);
            mArrays->setCurrentElementBufferObject(mElement);
        }

    private:
        osg::State* mState = nullptr;
        osg::GLExtensions* mExtensions = nullptr;
        osg::ref_ptr<osg::VertexArrayState> mArrays;
        osg::ref_ptr<osg::GLBufferObject> mVertex, mElement;
        GLuint mVao = 0,mVertexBinding = 0,mElementBinding = 0;
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

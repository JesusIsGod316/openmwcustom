#include <osg/Array>
#include <osg/BufferObject>
#include <osg/GLExtensions>
#include <osg/GraphicsContext>
#include <osg/PrimitiveSet>
#include <osg/State>
#include <osgViewer/Viewer>
#include <components/sceneutil/bufferpreparationscope.hpp>

#include <iostream>
#include <stdexcept>
#include <string_view>

namespace
{
    void require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }

    GLint binding(GLenum target)
    {
        GLint result = -1;
        glGetIntegerv(target, &result);
        return result;
    }
}

int main(int argc, char** argv) try
{
    osgViewer::Viewer anchor;
    osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
    traits->readDISPLAY(); traits->setUndefinedScreenDetailsToDefaultScreen();
    traits->width = 32; traits->height = 32; traits->doubleBuffer = false;
    osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
    require(context && context->realize() && context->makeCurrent(), "GL context unavailable");
    auto& state = *context->getState();
    auto* vas = state.getCurrentVertexArrayState();
    auto* ext = state.get<osg::GLExtensions>();
    require(vas && ext && ext->glBindBuffer, "buffer state unavailable");
    require(!vas->getCurrentVertexBufferObject() && !vas->getCurrentElementBufferObject(),
        "fixture must begin with empty OSG binding caches");

    osg::ref_ptr<osg::Vec3Array> positions = new osg::Vec3Array;
    positions->push_back(osg::Vec3(-.5f, -.5f, 0));
    positions->push_back(osg::Vec3(.5f, -.5f, 0));
    positions->push_back(osg::Vec3(0, .5f, 0));
    osg::ref_ptr<osg::VertexBufferObject> vertex = new osg::VertexBufferObject;
    positions->setVertexBufferObject(vertex);
    osg::ref_ptr<osg::DrawElementsUInt> indices = new osg::DrawElementsUInt(GL_TRIANGLES);
    indices->push_back(0); indices->push_back(1); indices->push_back(2);
    osg::ref_ptr<osg::ElementBufferObject> element = new osg::ElementBufferObject;
    indices->setElementBufferObject(element);
    auto* glVertex = vertex->getOrCreateGLBufferObject(state.getContextID());
    auto* glElement = element->getOrCreateGLBufferObject(state.getContextID());

    const bool legacy = argc > 1 && std::string_view(argv[1]) == "--legacy";
    if (legacy)
    {
        // Safe reproduction: inspect bindings without issuing an invalid draw.
        glVertex->compileBuffer();
        glElement->compileBuffer();
        state.unbindVertexBufferObject();
        state.unbindElementBufferObject();
    }
    else
    {
        SceneUtil::BufferPreparationScope preparation(&state);
        glVertex->compileBuffer();
        glElement->compileBuffer();
    }
    const GLint actualVertex = binding(GL_ARRAY_BUFFER_BINDING_ARB);
    const GLint actualElement = binding(GL_ELEMENT_ARRAY_BUFFER_BINDING_ARB);
    std::cout << "after_manual_prepare vertex=" << actualVertex << " element=" << actualElement
        << " cached_vertex=" << vas->getCurrentVertexBufferObject()
        << " cached_element=" << vas->getCurrentElementBufferObject() << '\n';
    // Repair the fixture before reporting failure. Never give a CPU index
    // address to GL while a foreign element buffer is bound.
    ext->glBindBuffer(GL_ARRAY_BUFFER_ARB, 0);
    ext->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
    require(actualVertex == 0 && actualElement == 0,
        "manual preparation left live GL bindings behind empty OSG caches");

    // Restore nonempty tracked caller bindings, including exception unwinding.
    state.bindVertexBufferObject(glVertex);
    state.bindElementBufferObject(glElement);
    try
    {
        SceneUtil::BufferPreparationScope preparation(&state);
        ext->glBindBuffer(GL_ARRAY_BUFFER_ARB, 0);
        ext->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
        throw std::runtime_error("fixture unwind");
    }
    catch (const std::runtime_error&) {}
    require(binding(GL_ARRAY_BUFFER_BINDING_ARB) == GLint(glVertex->getGLObjectID())
        && binding(GL_ELEMENT_ARRAY_BUFFER_BINDING_ARB) == GLint(glElement->getGLObjectID())
        && vas->getCurrentVertexBufferObject() == glVertex
        && vas->getCurrentElementBufferObject() == glElement,
        "caller bindings/cache were not restored on unwind");

    // Direct CPU draws must re-dispatch an unchanged Array when its backend
    // changes. No invalid draw is issued during the old backend transition.
    vas->setVertexBufferObjectSupported(true);
    state.setVertexPointer(positions);
    require(binding(GL_VERTEX_ARRAY_BUFFER_BINDING_ARB) == GLint(glVertex->getGLObjectID()),
        "VBO attribute was not established");
    SceneUtil::prepareClientArrayDraw(state);
    vas->setVertexBufferObjectSupported(false);
    state.setVertexPointer(positions);
    GLvoid* pointer = nullptr;
    glGetPointerv(GL_VERTEX_ARRAY_POINTER, &pointer);
    require(binding(GL_VERTEX_ARRAY_BUFFER_BINDING_ARB) == 0
        && binding(GL_ELEMENT_ARRAY_BUFFER_BINDING_ARB) == 0
        && pointer == positions->getDataPointer(), "client pointer retained a VBO interpretation");
    state.disableAllVertexArrays();

    // Compilation must not corrupt a retained caller VAO's element binding.
    if (ext->glGenVertexArrays && ext->glBindVertexArray && ext->glDeleteVertexArrays)
    {
        GLuint vao = 0;
        ext->glGenVertexArrays(1, &vao);
        state.bindVertexArrayObject(vao);
        state.bindElementBufferObject(glElement);
        {
            SceneUtil::BufferPreparationScope preparation(&state);
            state.unbindVertexArrayObject();
            ext->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
        }
        require(state.getCurrentVertexArrayObject() == vao
            && binding(GL_ELEMENT_ARRAY_BUFFER_BINDING_ARB) == GLint(glElement->getGLObjectID()),
            "caller VAO/element binding changed during preparation");
        state.unbindElementBufferObject();
        state.unbindVertexArrayObject();
        ext->glDeleteVertexArrays(1, &vao);
    }
    require(glGetError() == GL_NO_ERROR, "GL error in binding fixture");
    context->releaseContext(); context->close(true);
    std::cout << "PASS: empty/tracked bindings, unwind, client backend transition and caller VAO\n";
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}

#ifndef OPENMW_SCENEUTIL_STATICVAOGEOMETRY_H
#define OPENMW_SCENEUTIL_STATICVAOGEOMETRY_H

#include "bufferpreparationscope.hpp"
#include <osg/Geometry>
#include <osg/Geode>
#include <osg/GraphicsContext>
#include <osg/MatrixTransform>
#include <osg/NodeVisitor>
#include <osg/State>
#include <osg/VertexArrayState>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <new>
#include <typeinfo>
#include <utility>

namespace SceneUtil::StaticVao
{
    inline constexpr unsigned MaximumNodes = 4096, MaximumArrays = 32, MaximumPrimitives = 8;
    inline constexpr std::uint64_t MaximumBytes = 16ull * 1024ull * 1024ull;
    struct Counters
    {
        inline static std::atomic_uint64_t installed{0}, declined{0}, draws{0}, layoutReuses{0}, layoutSetups{0};
        inline static std::atomic_uint64_t producerFallbacks{0}, contextFallbacks{0}, liveBytes{0};
    };

    // Stores exact storage/layout identities, not vertex/index copies or a GL
    // command stream. The original references prevent address-reuse acceptance.
    struct ArrayRevision
    {
        osg::ref_ptr<const osg::Array> owner;
        osg::ref_ptr<const osg::BufferObject> buffer;
        unsigned revision=0, elements=0, bufferIndex=0;
        osg::Array::Binding binding=osg::Array::BIND_OFF;
        bool normalize=false, preserve=false;
        GLint size=0;
        GLenum type=0;
        explicit ArrayRevision(const osg::Array* value=nullptr) : owner(value)
        {
            if (!value) return;
            buffer=value->getBufferObject(); revision=value->getModifiedCount(); elements=value->getNumElements();
            bufferIndex=value->getBufferIndex(); binding=value->getBinding(); normalize=value->getNormalize();
            preserve=value->getPreserveDataType(); size=value->getDataSize(); type=value->getDataType();
        }
        bool matches(const osg::Array* value) const
        {
            return owner.get()==value && (!value || (value->getDataVariance()!=osg::Object::DYNAMIC
                && buffer.get()==value->getBufferObject() && revision==value->getModifiedCount()
                && elements==value->getNumElements() && bufferIndex==value->getBufferIndex()
                && binding==value->getBinding() && normalize==value->getNormalize()
                && preserve==value->getPreserveDataType() && size==value->getDataSize() && type==value->getDataType()));
        }
    };

    struct PrimitiveRevision
    {
        osg::ref_ptr<const osg::PrimitiveSet> owner;
        osg::ref_ptr<const osg::BufferObject> buffer;
        unsigned revision=0, indices=0, bufferIndex=0;
        GLenum mode=0;
        int instances=0;
        explicit PrimitiveRevision(const osg::PrimitiveSet* value=nullptr) : owner(value)
        {
            if (!value) return;
            revision=value->getModifiedCount(); indices=value->getNumIndices(); mode=value->getMode();
            instances=value->getNumInstances(); buffer=value->getBufferObject(); bufferIndex=value->getBufferIndex();
        }
        bool matches(const osg::PrimitiveSet* value) const
        {
            return owner.get()==value && value && value->getDataVariance()!=osg::Object::DYNAMIC
                && revision==value->getModifiedCount() && indices==value->getNumIndices() && mode==value->getMode()
                && instances==value->getNumInstances() && buffer.get()==value->getBufferObject()
                && bufferIndex==value->getBufferIndex();
        }
    };

    struct Recipe
    {
        std::array<ArrayRevision,MaximumArrays> arrays;
        std::array<PrimitiveRevision,MaximumPrimitives> primitives;
        std::array<osg::ref_ptr<const osg::BufferObject>,MaximumArrays+MaximumPrimitives> buffers;
        unsigned textureCount=0, attributeCount=0, arrayCount=0, primitiveCount=0, bufferCount=0;
        bool qualified=false;

        static const osg::Array* arrayAt(const osg::Geometry& value,unsigned i)
        {
            switch(i)
            {
                case 0:return value.getVertexArray(); case 1:return value.getNormalArray();
                case 2:return value.getColorArray(); case 3:return value.getSecondaryColorArray();
                case 4:return value.getFogCoordArray(); default:break;
            }
            i-=5;
            if (i<value.getNumTexCoordArrays()) return value.getTexCoordArray(i);
            return value.getVertexAttribArray(i-value.getNumTexCoordArrays());
        }
        explicit Recipe(const osg::Geometry& value)
        {
            textureCount=value.getNumTexCoordArrays(); attributeCount=value.getNumVertexAttribArrays();
            arrayCount=5+textureCount+attributeCount; primitiveCount=value.getNumPrimitiveSets();
            if (textureCount>8 || attributeCount>16 || arrayCount>arrays.size() || !primitiveCount
                || primitiveCount>primitives.size() || !value.getVertexArray()) return;
            const auto addBuffer=[&](const osg::BufferObject* buffer) {
                if (!buffer) return true;
                if (buffer->getUsage()!=GL_STATIC_DRAW_ARB || buffer->getCopyDataAndReleaseGLBufferObject()
                    || buffer->getNumBufferData()>MaximumArrays+MaximumPrimitives) return false;
                for(unsigned i=0;i<bufferCount;++i) if(buffers[i].get()==buffer) return true;
                if(bufferCount==buffers.size()) return false;
                buffers[bufferCount++]=buffer; return true;
            };
            for(unsigned i=0;i<arrayCount;++i)
            {
                const auto* array=arrayAt(value,i); arrays[i]=ArrayRevision(array);
                if(array && (array->getDataVariance()==osg::Object::DYNAMIC || array->getModifiedCallback()
                    || !addBuffer(array->getBufferObject()))) return;
                if(array && array->getBufferObject() && array->getBufferObject()->getTarget()!=GL_ARRAY_BUFFER_ARB) return;
                if(array && array->getBinding()==osg::Array::BIND_PER_VERTEX && !array->getBufferObject()) return;
            }
            for(unsigned i=0;i<primitiveCount;++i)
            {
                const auto* primitive=value.getPrimitiveSet(i);
                if(!primitive || primitive->getDataVariance()==osg::Object::DYNAMIC || primitive->getModifiedCallback()
                    || !addBuffer(primitive->getBufferObject())) return;
                if(primitive->getBufferObject() && primitive->getBufferObject()->getTarget()!=GL_ELEMENT_ARRAY_BUFFER_ARB) return;
                // Unknown PrimitiveSet overrides and indirect commands cannot enter.
                const auto type=primitive->getType();
                if((type!=osg::PrimitiveSet::DrawArraysPrimitiveType || typeid(*primitive)!=typeid(osg::DrawArrays))
                    && (type!=osg::PrimitiveSet::DrawElementsUBytePrimitiveType || typeid(*primitive)!=typeid(osg::DrawElementsUByte))
                    && (type!=osg::PrimitiveSet::DrawElementsUShortPrimitiveType || typeid(*primitive)!=typeid(osg::DrawElementsUShort))
                    && (type!=osg::PrimitiveSet::DrawElementsUIntPrimitiveType || typeid(*primitive)!=typeid(osg::DrawElementsUInt))) return;
                primitives[i]=PrimitiveRevision(primitive);
            }
            // No foreign BufferData may resize a shared storage layout behind
            // unchanged arrays. Shared geometry is allowed only when its exact
            // BufferData population is the same complete immutable recipe.
            qualified=storageClosed();
        }
        bool storageClosed() const
        {
            for(unsigned i=0;i<arrayCount;++i)
                if(arrays[i].buffer && arrays[i].buffer->getTarget()!=GL_ARRAY_BUFFER_ARB) return false;
            for(unsigned i=0;i<primitiveCount;++i)
                if(primitives[i].buffer && primitives[i].buffer->getTarget()!=GL_ELEMENT_ARRAY_BUFFER_ARB) return false;
            for(unsigned i=0;i<bufferCount;++i)
            {
                const auto& buffer=*buffers[i];
                if(buffer.getUsage()!=GL_STATIC_DRAW_ARB || buffer.getCopyDataAndReleaseGLBufferObject()
                    || buffer.getNumBufferData()>MaximumArrays+MaximumPrimitives) return false;
                for(unsigned j=0;j<buffer.getNumBufferData();++j)
                {
                    const auto* data=buffer.getBufferData(j);
                    bool known=false;
                    for(unsigned a=0;a<arrayCount && !known;++a) known=data && data==arrays[a].owner.get();
                    for(unsigned p=0;p<primitiveCount && !known;++p) known=data && data==primitives[p].owner.get();
                    if(!known) return false;
                }
            }
            return true;
        }
        bool matches(const osg::Geometry& value) const
        {
            if(!qualified || value.getDataVariance()==osg::Object::DYNAMIC || !value.getUseVertexBufferObjects()
                || value.getNumTexCoordArrays()!=textureCount || value.getNumVertexAttribArrays()!=attributeCount
                || value.getNumPrimitiveSets()!=primitiveCount || !storageClosed()) return false;
            for(unsigned i=0;i<arrayCount;++i) if(!arrays[i].matches(arrayAt(value,i))) return false;
            for(unsigned i=0;i<primitiveCount;++i) if(!primitives[i].matches(value.getPrimitiveSet(i))) return false;
            return true;
        }
        bool cleanBuffers(unsigned context) const
        {
            for(unsigned i=0;i<bufferCount;++i)
                if(const auto* gl=buffers[i]->getGLBufferObject(context);!gl || gl->isDirty()) return false;
            return true;
        }
    };

    class Geometry final : public osg::Geometry
    {
        struct Context
        {
            unsigned id=std::numeric_limits<unsigned>::max();
            osg::observer_ptr<osg::GraphicsContext> owner;
            osg::observer_ptr<osg::VertexArrayState> arrays;
            bool aliasing=false;
            std::array<unsigned,13> locations{};
            std::array<osg::ref_ptr<osg::GLBufferObject>,MaximumArrays+MaximumPrimitives> storage{};
            std::array<GLuint,MaximumArrays+MaximumPrimitives> names{};
            std::array<osg::GLsizeiptr,MaximumArrays> arrayOffsets{};
            std::array<osg::GLsizeiptr,MaximumPrimitives> indexOffsets{};
            unsigned storageCount=0;
            bool storageQualified=false;
            Context()=default;
            Context(unsigned context,osg::GraphicsContext* graphics,osg::VertexArrayState* vertexArrays,
                bool usesAliasing,std::array<unsigned,13> aliases)
                : id(context),owner(graphics),arrays(vertexArrays),aliasing(usesAliasing),locations(aliases) {}
            static std::array<unsigned,13> aliases(osg::State& state)
            {
                std::array<unsigned,13> result{ state.getVertexAlias()._location,state.getNormalAlias()._location,
                    state.getColorAlias()._location,state.getSecondaryColorAlias()._location,state.getFogCoordAlias()._location };
                const auto& texture=state.getTexCoordAliasList();
                for(unsigned i=0;i<8;++i) result[5+i]=i<texture.size()?texture[i]._location:std::numeric_limits<unsigned>::max();
                return result;
            }
            bool matches(osg::State& state,osg::VertexArrayState* live) const
            {
                return id==state.getContextID() && owner.valid() && owner.get()==state.getGraphicsContext()
                    && arrays.valid() && arrays.get()==live && aliasing==state.getUseVertexAttributeAliasing()
                    && (!aliasing || locations==aliases(state));
            }
            bool captureStorage(const Recipe& recipe,unsigned context)
            {
                storageQualified=false;storageCount=recipe.bufferCount;
                for(unsigned i=0;i<recipe.bufferCount;++i)
                {
                    auto* live=recipe.buffers[i]->getGLBufferObject(context);
                    if(!live || live->isDirty() || live->getBufferObject()!=recipe.buffers[i].get()
                        || !live->getGLObjectID()) return false;
                    storage[i]=live;names[i]=live->getGLObjectID();
                }
                for(unsigned i=0;i<recipe.arrayCount;++i)
                    if(const auto* buffer=recipe.arrays[i].buffer.get())
                        arrayOffsets[i]=buffer->getGLBufferObject(context)->getOffset(recipe.arrays[i].bufferIndex);
                for(unsigned i=0;i<recipe.primitiveCount;++i)
                    if(const auto* buffer=recipe.primitives[i].buffer.get())
                        indexOffsets[i]=buffer->getGLBufferObject(context)->getOffset(recipe.primitives[i].bufferIndex);
                return storageQualified=true;
            }
            bool matchesStorage(const Recipe& recipe,unsigned context) const
            {
                if(!storageQualified || storageCount!=recipe.bufferCount) return false;
                for(unsigned i=0;i<storageCount;++i)
                {
                    auto* live=recipe.buffers[i]->getGLBufferObject(context);
                    if(!live || live!=storage[i].get() || live->isDirty() || live->getGLObjectID()!=names[i]
                        || live->getBufferObject()!=recipe.buffers[i].get()) return false;
                }
                for(unsigned i=0;i<recipe.arrayCount;++i)
                    if(const auto* buffer=recipe.arrays[i].buffer.get();buffer
                        && arrayOffsets[i]!=buffer->getGLBufferObject(context)->getOffset(recipe.arrays[i].bufferIndex)) return false;
                for(unsigned i=0;i<recipe.primitiveCount;++i)
                    if(const auto* buffer=recipe.primitives[i].buffer.get();buffer
                        && indexOffsets[i]!=buffer->getGLBufferObject(context)->getOffset(recipe.primitives[i].bufferIndex)) return false;
                return true;
            }
        };
    public:
        Geometry() : mRecipe(*this) {setUseVertexArrayObject(false);}
        Geometry(const Geometry& other,const osg::CopyOp& copy=osg::CopyOp::SHALLOW_COPY)
            : osg::Geometry(other,copy),mRecipe(*this) {setUseVertexArrayObject(false);} // Clones require fresh producer admission.
        Geometry(const osg::Geometry& other,Recipe recipe,std::uint64_t charge)
            : osg::Geometry(other,osg::CopyOp::SHALLOW_COPY),mRecipe(std::move(recipe)),mCharge(charge)
        {setUseVertexArrayObject(true);}
        META_Object(SceneUtilStaticVao,Geometry)
        ~Geometry() override {if(mCharge) Counters::liveBytes.fetch_sub(mCharge,std::memory_order_relaxed);}

        void compileGLObjects(osg::RenderInfo& info) const override
        {
            // Native Geometry compilation unbinds raw VBO/EBO and VAO state.
            // Restore the actual caller's bindings and matching OSG caches.
            BufferPreparationScope scope(info.getState());
            osg::Geometry::compileGLObjects(info);
            if(auto* state=info.getState();state && state->getContextID()<mContexts.size()
                && mRecipe.matches(*this) && mRecipe.cleanBuffers(state->getContextID()))
                mContexts[state->getContextID()].captureStorage(mRecipe,state->getContextID());
        }

        osg::VertexArrayState* createVertexArrayStateImplementation(osg::RenderInfo& info) const override
        {
            auto* arrays=osg::Geometry::createVertexArrayStateImplementation(info);
            auto* state=info.getState();
            // Both ordinary first draw and ICO construction pass this exact
            // native seam. Keep completed producer VAOs through first use.
            // Context IDs own independent records; no cross-context reader of
            // another draw owner's record or GL wait is introduced.
            if(mCharge && arrays && state && state->getContextID()<mContexts.size()
                && state->getGraphicsContext() && state->getGraphicsContext()->isCurrent())
                mContexts[state->getContextID()]={ state->getContextID(),state->getGraphicsContext(),arrays,
                    state->getUseVertexAttributeAliasing(),Context::aliases(*state) };
            return arrays;
        }

        void drawImplementation(osg::RenderInfo& info) const override
        {
            auto* state=info.getState();
            if(state && state->getContextID()<mRetired.size()) mRetired[state->getContextID()]=nullptr;
            if(!mCharge || !state || !getUseVertexArrayObject()
                || !state->useVertexArrayObject(true) || !state->useVertexBufferObject(true))
            {++Counters::contextFallbacks; osg::Geometry::drawImplementation(info); return;}
            auto* arrays=state->getCurrentVertexArrayState();
            auto* context=state->getGraphicsContext();
            if(!arrays || !arrays->getVertexArrayObject() || !context || !context->isCurrent())
            {++Counters::contextFallbacks; osg::Geometry::drawImplementation(info); return;}
            ++Counters::draws;
            auto* found=state->getContextID()<mContexts.size()?&mContexts[state->getContextID()]:nullptr;
            const bool contextValid=found && found->matches(*state,arrays);
            const bool producerValid=mRecipe.matches(*this);
            const bool clean=mRecipe.cleanBuffers(state->getContextID());
            const bool storageValid=found && found->matchesStorage(mRecipe,state->getContextID());
            if(!producerValid)
            {
                // Unexpected producer changes may introduce client vertex or
                // index pointers. Such pointers cannot use a nonzero ARB VAO;
                // a retained EBO must never interpret a host pointer as an
                // offset. Use the native ordinary global VAS for this draw.
                ++Counters::producerFallbacks;
                if(found) found->storageQualified=false;
                arrays->setRequiresSetArrays(true);
                state->setCurrentToGlobalVertexArrayState();
                auto* ordinary=state->getCurrentVertexArrayState();
                state->bindVertexArrayObject(ordinary);
                state->disableAllVertexArrays();
                ordinary->resetBufferObjectPointers();
                ordinary->setRequiresSetArrays(true);
                auto* extensions=state->get<osg::GLExtensions>();
                extensions->glBindBuffer(GL_ARRAY_BUFFER_ARB,0);
                extensions->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER_ARB,0);
                osg::Geometry::drawImplementation(info);
                // Geometry still carries its admitted VAO flag and therefore
                // skips its usual ordinary unbind. Close both actual bindings
                // and the matching global VAS cache explicitly.
                extensions->glBindBuffer(GL_ARRAY_BUFFER_ARB,0);
                extensions->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER_ARB,0);
                ordinary->resetBufferObjectPointers();
                return;
            }
            if(!contextValid && found)
            {
                // A changed alias namespace/context never consumes an old VAO.
                // Use OSG's own current-context constructor; no guessed binding
                // locations, raw global VAO state or borrowed frame inputs.
                osg::ref_ptr<osg::VertexArrayState> replacement=createVertexArrayState(info);
                // Drawable::draw retains a raw old VAS across drawInner and
                // writes it in its epilogue. Retain that owner through the
                // complete outer call, until this context's next draw begins.
                mRetired[state->getContextID()]=_vertexArrayStateList[state->getContextID()];
                _vertexArrayStateList[state->getContextID()]=replacement;
                arrays=replacement;
                state->setCurrentVertexArrayState(arrays); state->bindVertexArrayObject(arrays);
                *found={ state->getContextID(),context,arrays,state->getUseVertexAttributeAliasing(),Context::aliases(*state) };
            }
            if(!producerValid || !clean || !storageValid || !found)
            {
                // Array::dirty does not call Geometry::dirtyGLObjects in pinned
                // OSG. Clear its pointer dispatch cache as well as the VAO flag;
                // resized/buffer-replaced inputs then take full native setup.
                // Geometry's native VAO creates only the dispatchers actually
                // used by this mesh. State::disableAllVertexArrays assumes the
                // global VAS, whose five legacy dispatchers always exist, and
                // dereferences null unused normal/color/fog dispatchers here.
                // Reset only existing native dispatchers; aliases may share one
                // dispatcher, and repeated disable preserves its cleared cache.
                const auto reset=[&](osg::VertexArrayState::ArrayDispatch* dispatch) {
                    if(dispatch) arrays->disable(dispatch,*state);
                };
                reset(arrays->_vertexArray); reset(arrays->_normalArray); reset(arrays->_colorArray);
                reset(arrays->_secondaryColorArray); reset(arrays->_fogCoordArray);
                for(const auto& dispatch : arrays->_texCoordArrays) reset(dispatch);
                for(const auto& dispatch : arrays->_vertexAttribArrays) reset(dispatch);
                arrays->setRequiresSetArrays(true);
                if(!producerValid) ++Counters::producerFallbacks;
                if(!found) ++Counters::contextFallbacks;
            }
            const bool reuse=contextValid && producerValid && clean && storageValid && !arrays->getRequiresSetArrays();
            // GL_ARRAY_BUFFER binding is global to the context, unlike vertex
            // attribute pointers/EBO bindings in a VAO. Another ordinary draw
            // or native compilation can leave raw binding 0 while this VAS
            // still caches its previous VBO. Full setup must force native
            // buffer binds before passing byte offsets to glVertexPointer.
            if(!reuse) arrays->resetBufferObjectPointers();
            if(reuse) ++Counters::layoutReuses; else ++Counters::layoutSetups;
            // Overall/per-primitive attributes and primitive dispatch remain
            // OSG's ordinary implementation. Only native array-layout setup is
            // retained; topology/state sorting/dynamic barriers are untouched.
            osg::Geometry::drawImplementation(info);
            if(!reuse && found && producerValid && mRecipe.cleanBuffers(state->getContextID()))
                found->captureStorage(mRecipe,state->getContextID());
        }
    private:
        Recipe mRecipe;
        std::uint64_t mCharge=0;
        mutable std::array<Context,4> mContexts;
        mutable std::array<osg::ref_ptr<osg::VertexArrayState>,4> mRetired;
    };

    inline bool eligible(const osg::Geometry& geometry)
    {
        return typeid(geometry)==typeid(osg::Geometry) && geometry.getDataVariance()!=osg::Object::DYNAMIC
            && geometry.getUseVertexBufferObjects() && !geometry.getUseVertexArrayObject()
            && !geometry.containsDeprecatedData() && !geometry.getUpdateCallback() && !geometry.getEventCallback()
            && !geometry.getCullCallback() && !geometry.getDrawCallback() && !geometry.getCreateVertexArrayStateCallback();
    }
    inline osg::ref_ptr<Geometry> make(const osg::Geometry& original)
    {
        if(!eligible(original)) {++Counters::declined;return {};}
        Recipe recipe(original);
        if(!recipe.qualified) {++Counters::declined;return {};}
        constexpr auto charge=sizeof(Geometry);
        auto used=Counters::liveBytes.load(std::memory_order_relaxed);
        do {if(charge>MaximumBytes-(std::min)(used,MaximumBytes)) {++Counters::declined;return {};}}
        while(!Counters::liveBytes.compare_exchange_weak(used,used+charge,std::memory_order_relaxed));
        try {auto result=new Geometry(original,std::move(recipe),charge);++Counters::installed;return result;}
        catch(const std::bad_alloc&) {Counters::liveBytes.fetch_sub(charge,std::memory_order_relaxed);++Counters::declined;return {};}
    }

    // Runs only on the private ordinary paging producer after optimizer/shadow
    // certification and before ICO publication. Unsupported/custom branches,
    // dynamic geometry and asset/indirect receipt owners stay unchanged.
    inline void install(osg::Group& root,bool enabled)
    {
        if(!enabled) return;
        struct Visitor final : osg::NodeVisitor
        {
            unsigned visited=0;
            Visitor():osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {setNodeMaskOverride(~0u);}
            void apply(osg::Node& node) override
            {
                if(++visited>MaximumNodes) return;
                if(node.getDataVariance()==osg::Object::DYNAMIC || node.getUpdateCallback() || node.getEventCallback()
                    || node.getCullCallback()) {++Counters::declined;return;}
                auto* group=node.asGroup();
                if(!group || (typeid(node)!=typeid(osg::Group) && typeid(node)!=typeid(osg::Geode)
                    && typeid(node)!=typeid(osg::MatrixTransform))) {++Counters::declined;return;}
                for(unsigned i=0;i<group->getNumChildren();++i)
                {
                    auto* geometry=group->getChild(i)->asGeometry();
                    if(!geometry) {group->getChild(i)->accept(*this);continue;}
                    if(++visited>MaximumNodes) return;
                    if(geometry->getNumParents()!=1) {++Counters::declined;continue;}
                    if(auto candidate=make(*geometry)) group->setChild(i,candidate);
                }
            }
        } visitor;
        root.accept(visitor);
    }
}

#endif

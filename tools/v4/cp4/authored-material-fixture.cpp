// Synthetic inherited OSG state with the office bottle's complete semantic
// combination. No copyrighted NIF/textures or user configuration is required.
#include <apps/openmw/mwrender/v4effectcapture.hpp>
#include <components/vfs/archive.hpp>
#include <components/vfs/file.hpp>
#include <osg/Group>
#include <sstream>
#include <stdexcept>

namespace RepairFixtures
{
    namespace
    {
        class File final : public VFS::File
        {
        public:
            Files::IStreamPtr open() override { return std::make_unique<std::istringstream>("synthetic material pixels"); }
            std::filesystem::file_time_type getLastModified() const override { return {}; }
            std::string getStem() const override { return "material"; }
        };
        class Archive final : public VFS::Archive
        {
            File mFile;
        public:
            void listResources(VFS::FileMap& out) override
            {
                for (const char* role : {"diffuseMap", "bumpMap", "envMap", "normalMap", "specularMap", "glossMap"})
                    out.insert_or_assign(VFS::Path::Normalized(std::string("textures/") + role + ".dds"), &mFile);
            }
            bool contains(VFS::Path::NormalizedView path) const override { return path.value().starts_with("textures/"); }
            std::string getDescription() const override { return "synthetic-authored-material-corpus"; }
        };
    }

    RenderCore::ImmediateEffectDraw captureAuthoredMaterial()
    {
        VFS::Manager vfs; vfs.addArchive(std::make_unique<Archive>()); vfs.buildIndex();
        NifRender::TextureIdentityCache identities(vfs);
        auto parent = osg::ref_ptr<osg::Group>(new osg::Group);
        auto geometry = osg::ref_ptr<osg::Geometry>(new osg::Geometry);
        parent->addChild(geometry); geometry->setName("synthetic-inherited-sphere-bump");
        auto positions = osg::ref_ptr<osg::Vec3Array>(new osg::Vec3Array);
        for (auto v : {osg::Vec3(-4,-4,-5), osg::Vec3(4,-4,-5), osg::Vec3(4,4,-5), osg::Vec3(-4,4,-5)})
            positions->push_back(v);
        auto normals = osg::ref_ptr<osg::Vec3Array>(new osg::Vec3Array); normals->push_back({0,0,1});
        auto colors = osg::ref_ptr<osg::Vec4Array>(new osg::Vec4Array); colors->push_back({1,1,1,1});
        auto uv = osg::ref_ptr<osg::Vec2Array>(new osg::Vec2Array);
        for (auto v : {osg::Vec2(0,0), osg::Vec2(1,0), osg::Vec2(1,1), osg::Vec2(0,1)}) uv->push_back(v);
        geometry->setVertexArray(positions); geometry->setNormalArray(normals, osg::Array::BIND_OVERALL);
        geometry->setColorArray(colors, osg::Array::BIND_OVERALL);
        geometry->addPrimitiveSet(new osg::DrawArrays(GL_QUADS,0,4));
        // Keep explicit texture stages even when coordinates alias UV0. The
        // sphere stage intentionally has no authored UV array.
        unsigned unit = 0;
        for (const char* role : {"diffuseMap", "bumpMap", "envMap", "normalMap", "specularMap", "glossMap"})
        {
            auto image = osg::ref_ptr<osg::Image>(new osg::Image);
            image->allocateImage(1,1,1,GL_RGBA,GL_UNSIGNED_BYTE);
            image->setFileName(std::string("textures/") + role + ".dds");
            auto texture = osg::ref_ptr<osg::Texture2D>(new osg::Texture2D(image)); texture->setName(role);
            parent->getOrCreateStateSet()->setTextureAttribute(unit, texture);
            if (unit != 2) geometry->setTexCoordArray(unit, uv);
            ++unit;
        }
        auto* state = parent->getOrCreateStateSet();
        auto material = osg::ref_ptr<SceneUtil::Material>(new SceneUtil::Material);
        material->setVertexColorMode(SceneUtil::VertexColorModes::None);
        material->setDiffuse({0,0,0,1});
        material->setAmbient({0,0,0,1});
        material->setSpecular({0,0,0,1});
        state->setAttribute(material);
        state->addUniform(new osg::Uniform("envMapColor", osg::Vec4(.4f,.2f,.1f,1)));
        state->addUniform(new osg::Uniform("bumpMapMatrix", osg::Matrix2(.25f,.1f,-.2f,.5f)));
        state->addUniform(new osg::Uniform("envMapLumaBias", osg::Vec2(0,.25f)));
        RenderCore::ImmediateEffectDraw draw; std::string error;
        if (!MWRender::v4_effect_detail::captureGeometry(*geometry,{parent.get()},vfs,"synthetic-authored",draw,error,&identities))
            throw std::runtime_error(error);
        return draw;
    }
}

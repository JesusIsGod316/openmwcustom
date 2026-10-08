#ifdef OPENMW_HAVE_DIRECTXTEX
// v3gpumemory deliberately removes legacy near/far macros. Parse the SDK's
// FAR-qualified GUID declarations before that cleanup, including DXGI 1.4.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXTex.h>
#include <dxgi1_4.h>
#endif

#include "selectivehlod.hpp"

#include "texturetype.hpp"
#include "persistentopaquepackets.hpp"
#include "pagingwork.hpp"
#include "generatedstaticlod.hpp"
#include "assetpaging.hpp"
#include <components/resource/assetgpubudget.hpp>
#include <components/resource/derivedassetcache.hpp>
#include <components/resource/speculativebudget.hpp>
#include <components/shader/assetgeometrycontract.hpp>
#include <components/nifosg/matrixtransform.hpp>

#include <osg/Geometry>
#include <osg/AlphaFunc>
#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/PositionAttitudeTransform>
#include <osg/Program>
#include <osg/Shader>
#include <osg/StateSet>
#include <osg/TexEnv>
#include <osg/Texture2D>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <typeinfo>
#include <vector>

namespace SceneUtil
{
    namespace
    {
        constexpr unsigned ChartSize = 256, Gutter = 128, TileSize = ChartSize + 2 * Gutter;
        constexpr unsigned SafeMaxLod = 7, MaxCharts = 16;
        struct Leaf
        {
            const osg::Geometry* geometry;
            osg::Matrix matrix;
            osg::ref_ptr<osg::StateSet> state;
            const osg::Texture2D* texture = nullptr;
            std::array<const osg::Texture2D*,3> textures{};
            unsigned chart = 0;
            unsigned family = 0;
        };
        struct Family {osg::ref_ptr<osg::StateSet> state;bool colors=false;};
        struct Role {std::string name;};
        struct Chart
        {
            const osg::Texture2D* texture;
            std::array<const osg::Texture2D*,3> textures{};
            osg::Vec2 minimum{std::numeric_limits<float>::max(),std::numeric_limits<float>::max()};
            osg::Vec2 maximum{-std::numeric_limits<float>::max(),-std::numeric_limits<float>::max()};
            std::array<int,2> origin{};
            std::array<unsigned,2> periods{1,1};
        };
        void extend(Chart& chart,const osg::Vec2Array& uv)
        {
            for(const auto& value:uv) for(unsigned axis=0;axis<2;++axis)
            {
                if(!std::isfinite(value[axis]) || std::abs(value[axis])>4096.f)
                    throw std::runtime_error("nonfinite or unbounded source UV domain");
                chart.minimum[axis]=std::min(chart.minimum[axis],value[axis]);
                chart.maximum[axis]=std::max(chart.maximum[axis],value[axis]);
            }
        }
        void qualifyDomain(Chart& chart)
        {
            for(unsigned axis=0;axis<2;++axis)
            {
                const auto wrap=chart.texture->getWrap(axis?osg::Texture::WRAP_T:osg::Texture::WRAP_S);
                if(wrap!=osg::Texture::REPEAT && wrap!=osg::Texture::CLAMP_TO_EDGE)
                    throw std::runtime_error("border or mirrored sampler requires a separate bake contract");
                if(chart.minimum[axis]>=0.f && chart.maximum[axis]<=1.f) continue;
                if(wrap!=osg::Texture::REPEAT)
                    throw std::runtime_error("clamped source UV outside the unit domain");
                chart.origin[axis]=static_cast<int>(std::floor(chart.minimum[axis]));
                const unsigned needed=static_cast<unsigned>(std::max(1.,std::ceil(double(chart.maximum[axis]))-chart.origin[axis]));
                while(chart.periods[axis]<needed && chart.periods[axis]<4) chart.periods[axis]*=2;
                if(chart.periods[axis]<needed)
                    throw std::runtime_error("source UV domain exceeds four baked repeat periods");
            }
        }
        void qualifyRoles(Leaf& leaf,const osg::Vec2Array& uv,std::vector<Role>& roles)
        {
            std::vector<Role> found;
            for(unsigned unit=0;unit<leaf.state->getTextureAttributeList().size();++unit)
            {
                const auto& attributes=leaf.state->getTextureAttributeList()[unit];
                if(attributes.empty()) continue;
                const auto* texture=dynamic_cast<const osg::Texture2D*>(leaf.state->getTextureAttribute(unit,osg::StateAttribute::TEXTURE));
                if(!texture || unit>=3 || unit!=found.size() || !texture->getImage())
                    throw std::runtime_error("material role unit layout is not qualified");
                const auto& name=getTextureType(*leaf.state,*texture,unit);
                if((unit==0 && name!="diffuseMap") || (unit==1 && name!="normalMap" && name!="specularMap")
                    || (unit==2 && (name!="specularMap" || found[1].name!="normalMap")))
                    throw std::runtime_error("extra, parallax or reconstructed material role");
                leaf.textures[unit]=texture;found.push_back({name});
                if(unit)
                {
                    const auto* coordinates=dynamic_cast<const osg::Vec2Array*>(leaf.geometry->getTexCoordArray(unit));
                    if(!coordinates || coordinates->getDataVariance()==osg::Object::DYNAMIC || *coordinates!=uv)
                        throw std::runtime_error("material roles require exact co-registered source UVs");
                }
            }
            if(found.empty()) throw std::runtime_error("missing diffuse material role");
            if(roles.empty()) roles=found;
            else
            {
                if(roles.size()!=found.size()) throw std::runtime_error("material role families differ");
                for(unsigned unit=0;unit<found.size();++unit)
                    if(roles[unit].name!=found[unit].name) throw std::runtime_error("material role/unit families differ");
            }
            bool normal=false;for(const auto& role:found) normal|=role.name=="normalMap";
            for(unsigned unit=1;unit<leaf.geometry->getNumTexCoordArrays();++unit)
            {
                const auto* coordinates=leaf.geometry->getTexCoordArray(unit);
                if(!coordinates) continue;
                if(unit<found.size()) continue;
                if(unit!=7 || !normal) throw std::runtime_error("additional UV or tangent semantics");
            }
            const auto* tangents=dynamic_cast<const osg::Vec4Array*>(leaf.geometry->getTexCoordArray(7));
            if(normal)
            {
                if(!tangents || tangents->getDataVariance()==osg::Object::DYNAMIC || tangents->size()!=uv.size()
                    || tangents->getBinding()!=osg::Array::BIND_PER_VERTEX)
                    throw std::runtime_error("normal-map role requires an immutable per-vertex tangent stream");
                for(const auto& tangent:*tangents)
                {
                    for(unsigned axis=0;axis<4;++axis) if(!std::isfinite(tangent[axis])) throw std::runtime_error("nonfinite source tangent");
                    if(std::abs(std::abs(tangent.w())-1.f)>1e-5f || osg::Vec3(tangent.x(),tangent.y(),tangent.z()).length2()<1e-12f)
                        throw std::runtime_error("unqualified source tangent orientation");
                }
            }
        }
        struct DrawReceipt final:PersistentOpaquePackets::RetainedOpaqueDrawCallback
        {
            explicit DrawReceipt(std::uint64_t indices):mIndices(indices){}
            void drawImplementation(osg::RenderInfo& info,const osg::Drawable* drawable) const override
            {
                if(guard && !guard->current(info.getState()))
                {
                    ++HlodCounters::materialDrawFallbacks;
                    GeneratedStaticLod::drawOriginals(info,originals,osg::Matrixd::identity(),drawable->getStateSet(),candidateStates);
                    return;
                }
                ++HlodCounters::consumedDraws;HlodCounters::consumedIndices+=mIndices;drawable->drawImplementation(info);
            }
            std::uint64_t mIndices;
            std::shared_ptr<GeneratedStaticLod::MaterialGuard> guard;
            std::vector<GeneratedStaticLod::OriginalDraw> originals,proofSources;
            std::vector<osg::ref_ptr<const osg::StateSet>> candidateStates;
        };

        bool finite(const osg::Matrix& matrix)
        {
            for (unsigned i = 0; i < 16; ++i)
                if (!std::isfinite(matrix.ptr()[i])) return false;
            return true;
        }
        bool uniformPositiveTransform(const osg::Matrix& m)
        {
            if (!finite(m) || std::abs(m(0,3)) > 1e-6 || std::abs(m(1,3)) > 1e-6
                || std::abs(m(2,3)) > 1e-6 || std::abs(m(3,3) - 1.) > 1e-6) return false;
            const osg::Vec3d a(m(0,0),m(0,1),m(0,2)), b(m(1,0),m(1,1),m(1,2)), c(m(2,0),m(2,1),m(2,2));
            const double scale2 = a.length2();
            return scale2 > 1e-12 && std::abs(b.length2() - scale2) < scale2 * 1e-5
                && std::abs(c.length2() - scale2) < scale2 * 1e-5
                && std::abs(a*b) < scale2 * 1e-5 && std::abs(a*c) < scale2 * 1e-5
                && std::abs(b*c) < scale2 * 1e-5 && (a ^ b) * c > 0.;
        }
        bool loaderEmptyMarker(const osg::Node& node,unsigned depth=0,unsigned* visited=nullptr)
        {
            unsigned rootVisited=0;if(!visited) visited=&rootVisited;
            unsigned record=~0u;
            // NifLoader emits hidden collision/marker transform hierarchies
            // after deliberately skipping their meshes. This narrow case has
            // no rendering population to replace. Names and node masks alone
            // never certify arbitrary hidden geometry or application callbacks.
            if(depth>32 || ++*visited>4096 || node.getDataVariance()!=osg::Object::STATIC
                || !node.getUserValue("recordIndex",record) || record==~0u
                || node.getUpdateCallback() || node.getEventCallback() || node.getCullCallback()
                || node.getComputeBoundingSphereCallback() || node.asDrawable()
                || (typeid(node)!=typeid(osg::Group) && typeid(node)!=typeid(osg::MatrixTransform)
                    && typeid(node)!=typeid(NifOsg::MatrixTransform))) return false;
            if(const auto* state=node.getStateSet();state && (state->getDataVariance()==osg::Object::DYNAMIC
                || state->getUpdateCallback() || state->getEventCallback())) return false;
            if(const auto* transform=dynamic_cast<const osg::MatrixTransform*>(&node))
                if(transform->getReferenceFrame()!=osg::Transform::RELATIVE_RF || !finite(transform->getMatrix())) return false;
            const auto* group=node.asGroup();
            for(unsigned i=0;i<group->getNumChildren();++i)
                if(!loaderEmptyMarker(*group->getChild(i),depth+1,visited)) return false;
            return true;
        }
        bool immutable(const osg::Node& node, unsigned depth=0, unsigned* visited=nullptr)
        {
            unsigned rootVisited=0;
            if(!visited) visited=&rootVisited;
            if(depth>32 || ++*visited>4096) return false;
            if(!node.getNodeMask()) return loaderEmptyMarker(node,depth,visited);
            if (node.getDataVariance() == osg::Object::DYNAMIC || node.getUpdateCallback()
                || node.getEventCallback() || node.getCullCallback() || node.getComputeBoundingSphereCallback()
                || node.getNodeMask()!=~0u) return false;
            int applyMode=0;std::string prefix;
            if((node.getUserValue("applyMode",applyMode) && applyMode==4) || node.getUserValue("shaderPrefix",prefix)) return false;
            if (const auto* ss = node.getStateSet(); ss && (ss->getDataVariance() == osg::Object::DYNAMIC
                || ss->getUpdateCallback() || ss->getEventCallback())) return false;
            if (const auto* d = node.asDrawable())
                return typeid(*d) == typeid(osg::Geometry) && !d->getDrawCallback()
                    && !d->getComputeBoundingBoxCallback();
            if (const auto* t = node.asTransform())
            {
                if (typeid(node)!=typeid(osg::MatrixTransform) && typeid(node)!=typeid(NifOsg::MatrixTransform)
                    && typeid(node)!=typeid(osg::PositionAttitudeTransform)) return false;
                if (t->getReferenceFrame() != osg::Transform::RELATIVE_RF) return false;
                osg::Matrix matrix;
                if (!t->computeLocalToWorldMatrix(matrix, nullptr) || !uniformPositiveTransform(matrix)) return false;
            }
            else if (typeid(node)!=typeid(osg::Group) && typeid(node)!=typeid(osg::Geode))
                return false;
            const auto* group = node.asGroup();
            if (!group) return false;
            for (unsigned i = 0; i < group->getNumChildren(); ++i)
                if (!immutable(*group->getChild(i),depth+1,visited)) return false;
            return true;
        }
        bool simpleState(const osg::StateSet& ss)
        {
            if (ss.getDataVariance() == osg::Object::DYNAMIC || ss.getUpdateCallback() || ss.getEventCallback()
                || ss.getRenderingHint() == osg::StateSet::TRANSPARENT_BIN
                || (!ss.getBinName().empty() && ss.getBinName()!="RenderBin")) return false;
            for (const auto& [mode, flags] : ss.getModeList())
            {
                // ShaderVisitor's opaque closure deliberately disables the
                // fixed-function alpha test with PROTECTED. Keep that exact
                // harmless forced-off state; other override ancestry is refused.
                const bool protectedOpaqueAlpha=mode==GL_ALPHA_TEST && !(flags & osg::StateAttribute::ON)
                    && !(flags & osg::StateAttribute::OVERRIDE);
                if ((flags & (osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED)) != 0
                    && !protectedOpaqueAlpha) return false;
                if ((mode == GL_BLEND || mode == GL_ALPHA_TEST || mode == 0x809e /* GL_SAMPLE_ALPHA_TO_COVERAGE */)
                    && (flags & osg::StateAttribute::ON)) return false;
            }
            for (const auto& [type, pair] : ss.getAttributeList())
            {
                if (pair.first->getDataVariance() == osg::Object::DYNAMIC
                    || pair.second & (osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED)) return false;
                switch (type.first)
                {
                    case osg::StateAttribute::PROGRAM: case osg::StateAttribute::MATERIAL:
                    case osg::StateAttribute::CULLFACE: case osg::StateAttribute::FRONTFACE:
                    case osg::StateAttribute::DEPTH: case osg::StateAttribute::POLYGONOFFSET: break;
                    case osg::StateAttribute::ALPHAFUNC:
                        if(static_cast<const osg::AlphaFunc*>(pair.first.get())->getFunction()!=osg::AlphaFunc::ALWAYS) return false;
                        break;
                    case osg::StateAttribute::BLENDFUNC: break; // GL_BLEND is proven disabled throughout ancestry.
                    default: return false;
                }
            }
            for (const auto& [name, uniform] : ss.getUniformList())
            {
                if(name=="projectionMatrix") return false;
                if(name.starts_with("texMat"))
                {
                    osg::Matrixf transform;
                    if(!uniform.first->get(transform) || transform!=osg::Matrixf::identity()) return false;
                }
                if(name=="useTreeAnim") {bool enabled=true;if(!uniform.first->get(enabled) || enabled) return false;}
                if (uniform.first->getDataVariance() == osg::Object::DYNAMIC || uniform.first->getUpdateCallback()
                    || uniform.first->getEventCallback()
                    || uniform.second & (osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED)) return false;
            }
            for (const auto& list : ss.getTextureAttributeList())
                for (const auto& [type, pair] : list)
                {
                    if (pair.second & (osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED)) return false;
                    if (type.first != osg::StateAttribute::TEXTURE && type.first != TextureType::AttributeType
                        && type.first != osg::StateAttribute::TEXENV) return false;
                    if (type.first == osg::StateAttribute::TEXENV
                        && static_cast<const osg::TexEnv*>(pair.first.get())->getMode() != osg::TexEnv::MODULATE) return false;
                }
            return true;
        }
        void collect(const osg::Node& node, const osg::Matrix& parent, const osg::StateSet& inherited,
            std::vector<Leaf>& leaves)
        {
            if(!node.getNodeMask())
            {
                if(!loaderEmptyMarker(node)) throw std::runtime_error("unqualified hidden graph population");
                return;
            }
            osg::ref_ptr<osg::StateSet> effective = new osg::StateSet(inherited, osg::CopyOp::SHALLOW_COPY);
            if (const auto* ss = node.getStateSet())
            {
                if (!simpleState(*ss)) throw std::runtime_error("inherited or leaf state is not qualified");
                effective->merge(*ss);
            }
            osg::Matrix matrix = parent;
            if (const auto* transform = node.asTransform())
                if (!transform->computeLocalToWorldMatrix(matrix, nullptr) || !uniformPositiveTransform(matrix))
                    throw std::runtime_error("nonuniform, mirrored, singular or custom transform");
            if (const auto* geometry = dynamic_cast<const osg::Geometry*>(node.asDrawable()))
            {
                leaves.push_back({geometry, matrix, effective});
                return;
            }
            if (const auto* group = node.asGroup())
                for (unsigned i = 0; i < group->getNumChildren(); ++i) collect(*group->getChild(i), matrix, *effective, leaves);
        }
        bool shippedProgram(const osg::StateSet& state,GeneratedStaticLod::RefusalInfo* info=nullptr,
            std::map<const osg::Program*,Shader::AssetGeometry::Snapshot>* validated=nullptr)
        {
            const auto* program = dynamic_cast<const osg::Program*>(state.getAttribute(osg::StateAttribute::PROGRAM));
            if (!program) {if(info) info->detail=GeneratedStaticLod::Detail::ShaderFamily;return false;}
            std::optional<Shader::AssetGeometry::Snapshot> proof;
            if(validated) if(const auto found=validated->find(program);found!=validated->end()) proof=found->second;
            if(!proof)
            {
                const auto reason=Shader::AssetGeometry::validate(*program);
                if(reason!=Shader::AssetGeometry::Refusal::None)
                {if(info) {info->program=program;info->detail=GeneratedStaticLod::shaderDetail(static_cast<unsigned>(reason));}return false;}
                proof=Shader::AssetGeometry::capture(*program,{&state});
            }
            if(!proof) {if(info) info->detail=GeneratedStaticLod::Detail::ShaderDefines;return false;}
            // PBR atlases preserve co-registered diffuse/normal/specular roles.
            // The adjusted/parallax UV and reconstructed normal lanes require
            // independent atlas recipes and retain the original representation.
            for(const char* name:{"parallax","diffuseParallax","reconstructNormalZ"})
                if(const auto value=proof->define(name); !value.empty() && value!="0")
                {if(info) info->detail=GeneratedStaticLod::Detail::ShaderFeatures;return false;}
            unsigned roles=0;
            for(unsigned unit=0;unit<state.getTextureAttributeList().size();++unit)
                if(const auto* texture=state.getTextureAttribute(unit,osg::StateAttribute::TEXTURE);texture && texture->asTexture())
                {
                    const auto& name=getTextureType(state,*texture->asTexture(),unit);
                    const auto sampler=proof->samplers().find(name);
                    if(unit>=3 || sampler==proof->samplers().end() || sampler->second!=int(unit)
                        || proof->define(name)!="1" || proof->define(name+"UV")!=std::to_string(unit))
                    {if(info) info->detail=GeneratedStaticLod::Detail::ShaderBindings;return false;}
                    ++roles;
                }
            if(roles!=proof->samplers().size()) {if(info) info->detail=GeneratedStaticLod::Detail::ShaderBindings;return false;}
            if(validated) validated->insert_or_assign(program,*proof);return true;
        }
        std::uint64_t submissions(const osg::Node& node)
        {
            if (const auto* geometry = dynamic_cast<const osg::Geometry*>(node.asDrawable()))
                return geometry->getNumPrimitiveSets();
            std::uint64_t value = 0;
            if (const auto* group = node.asGroup())
                for (unsigned i = 0; i < group->getNumChildren(); ++i) value += submissions(*group->getChild(i));
            return value;
        }
        struct Rgba { unsigned char r,g,b,a; };
        std::vector<Rgba> decodeBase(const osg::Image& image)
        {
            if (!image.data() || image.s() < 1 || image.t() < 1 || image.r() != 1
                || image.s() > 2048 || image.t() > 2048) throw std::runtime_error("image extent or payload is not qualified");
            const unsigned w = image.s(), h = image.t();
            std::vector<Rgba> result(std::size_t(w) * h);
            const unsigned format = image.getPixelFormat();
            if (!image.isCompressed())
            {
                if (image.getDataType() != GL_UNSIGNED_BYTE || (format != GL_RGBA && format != GL_RGB
                    && format != GL_BGRA && format != GL_BGR)) throw std::runtime_error("unknown diffuse channel format");
                const unsigned channels = format == GL_RGBA || format == GL_BGRA ? 4 : 3;
                for (unsigned y=0;y<h;++y) for (unsigned x=0;x<w;++x)
                {
                    const auto* p = image.data(x,y);
                    result[std::size_t(y)*w+x] = format == GL_BGRA || format == GL_BGR
                        ? Rgba{p[2],p[1],p[0],channels == 4 ? p[3] : static_cast<unsigned char>(255)}
                        : Rgba{p[0],p[1],p[2],channels == 4 ? p[3] : static_cast<unsigned char>(255)};
                }
                return result;
            }
            // Frozen DDS adapter: BC1/BC2/BC3 only. BC5 normals and BC7 remain
            // unqualified; alpha is retained as data (including diffuse specular).
            const bool bc1 = format == 0x83f0 || format == 0x83f1 || format == 0x8c4c || format == 0x8c4d;
            const bool bc2 = format == 0x83f2 || format == 0x8c4e;
            const bool bc3 = format == 0x83f3 || format == 0x8c4f;
            if (!bc1 && !bc2 && !bc3) throw std::runtime_error("unsupported compressed diffuse format");
            const unsigned blockBytes = bc1 ? 8 : 16, bw = (w+3)/4, bh = (h+3)/4;
            if (image.getImageSizeInBytes() < bw*bh*blockBytes) throw std::runtime_error("truncated diffuse blocks");
#ifdef OPENMW_HAVE_DIRECTXTEX
            DirectX::Image input{};
            input.width=w;input.height=h;
            input.format=bc1?DXGI_FORMAT_BC1_UNORM:(bc2?DXGI_FORMAT_BC2_UNORM:DXGI_FORMAT_BC3_UNORM);
            input.rowPitch=bw*blockBytes;input.slicePitch=bw*bh*blockBytes;
            input.pixels=const_cast<unsigned char*>(image.data());
            DirectX::ScratchImage decoded;
            if(FAILED(DirectX::Decompress(input,DXGI_FORMAT_R8G8B8A8_UNORM,decoded)))
                throw std::runtime_error("qualified BC diffuse decoder declined payload");
            const auto* rgba=decoded.GetImage(0,0,0);
            if(!rgba || rgba->width!=w || rgba->height!=h) throw std::runtime_error("BC diffuse decoder extent mismatch");
            for(unsigned y=0;y<h;++y) std::memcpy(result.data()+std::size_t(y)*w,rgba->pixels+y*rgba->rowPitch,w*sizeof(Rgba));
#else
            throw std::runtime_error("compressed atlas sources need the pinned CPU decoder");
#endif
            return result;
        }
        bool srgb(const osg::Image& image)
        {
            const unsigned f = image.getInternalTextureFormat();
            return f == 0x8c41 || f == 0x8c43 || (f >= 0x8c4c && f <= 0x8c4f);
        }
        float linear(unsigned char c, bool s)
        {
            const float v = c/255.f;
            return s ? (v <= .04045f ? v/12.92f : std::pow((v+.055f)/1.055f,2.4f)) : v;
        }
        unsigned char encode(float v, bool s)
        {
            if (s) v = v <= .0031308f ? v*12.92f : 1.055f*std::pow(v,1.f/2.4f)-.055f;
            return static_cast<unsigned char>(std::clamp(std::lround(v*255.f),0l,255l));
        }
        std::vector<Rgba> resizeRect(const std::vector<Rgba>& source, unsigned w, unsigned h,
            unsigned width,unsigned height,bool s)
        {
            std::vector<Rgba> result(std::size_t(width)*height);
            for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x)
            {
                if(!x) PagingWorkScope::checkpoint();
                const unsigned x0=x*w/width, x1=std::max(x0+1,(x+1)*w/width);
                const unsigned y0=y*h/height, y1=std::max(y0+1,(y+1)*h/height);
                std::array<float,4> value{}; unsigned count=0;
                for(unsigned sy=y0;sy<std::min(y1,h);++sy) for(unsigned sx=x0;sx<std::min(x1,w);++sx)
                {
                    const auto& p=source[std::size_t(sy)*w+sx];
                    value[0]+=linear(p.r,s);value[1]+=linear(p.g,s);value[2]+=linear(p.b,s);value[3]+=p.a/255.f;++count;
                }
                result[std::size_t(y)*width+x]={encode(value[0]/count,s),encode(value[1]/count,s),encode(value[2]/count,s),encode(value[3]/count,false)};
            }
            return result;
        }
        std::vector<Rgba> resize(const std::vector<Rgba>& source,unsigned w,unsigned h,unsigned size,bool s)
        {return resizeRect(source,w,h,size,size,s);}
        std::vector<Rgba> bakeChart(const Chart& chart,bool s,unsigned role)
        {
            // The origin is an integer source period, so translating it does
            // not change REPEAT sampling. Bake complete power-of-two periods
            // rather than clamping vertices or wrapping them independently:
            // the UV remap remains affine across each original triangle.
            const auto& image=*chart.textures[role]->getImage();
            const unsigned w=ChartSize/chart.periods[0],h=ChartSize/chart.periods[1];
            const auto period=resizeRect(decodeBase(image),image.s(),image.t(),w,h,s);
            std::vector<Rgba> result(std::size_t(ChartSize)*ChartSize);
            for(unsigned y=0;y<ChartSize;++y) for(unsigned x=0;x<ChartSize;++x)
                result[std::size_t(y)*ChartSize+x]=period[std::size_t(y%h)*w+x%w];
            return result;
        }
    }

    bool certifySelectiveHlodTemplate(const osg::Node& original,GeneratedStaticLod::RefusalInfo* info)
    {
        const bool valid=immutable(original);
        if(info)
        {
            *info={};
            if(!valid)
            {
                unsigned nodes=0;
                const auto detail=[&](const auto& self,const osg::Node& node,unsigned depth)->bool {
                    if(++nodes>4096 || depth>32) return false;
                    info->node=&node;
                    if(node.getDataVariance()==osg::Object::DYNAMIC) {info->detail=GeneratedStaticLod::Detail::Dynamic;return true;}
                    if(node.getUpdateCallback() || node.getEventCallback() || node.getCullCallback() || node.getComputeBoundingSphereCallback())
                    {info->detail=GeneratedStaticLod::Detail::GraphCallback;return true;}
                    if(!node.getNodeMask() && !loaderEmptyMarker(node)) {info->detail=GeneratedStaticLod::Detail::HiddenGraph;return true;}
                    if(const auto* group=node.asGroup()) for(unsigned i=0;i<group->getNumChildren();++i) if(self(self,*group->getChild(i),depth+1)) return true;
                    return false;
                };
                if(!detail(detail,original,0)) info->detail=GeneratedStaticLod::Detail::GraphType;
            }
        }
        return valid;
    }

    bool qualifySelectiveHlodTemplate(const osg::Node& original,bool requireShippedObjectShader,GeneratedStaticLod::RefusalInfo* info)
    {
        if(info) *info={};
        try
        {
            if(!certifySelectiveHlodTemplate(original,info)) return false;
            if(info) info->node=&original;
            // Diagnosis only: this sieve never admits a previously refused
            // material. One subtype is retained for the bounded first example.
            if(info)
            {
                unsigned nodes=0;
                const auto diagnose=[&](const auto& self,const osg::Node& node,unsigned depth)->bool {
                    if(++nodes>4096 || depth>32) return false;
                    if(const auto* state=node.getStateSet())
                    {
                        info->node=&node;
                        if(state->getMode(GL_BLEND)&osg::StateAttribute::ON) {info->detail=GeneratedStaticLod::Detail::Blend;return true;}
                        if((state->getMode(GL_ALPHA_TEST)&osg::StateAttribute::ON) || state->getRenderingHint()==osg::StateSet::TRANSPARENT_BIN)
                        {info->detail=GeneratedStaticLod::Detail::Alpha;return true;}
                        if(const auto* alpha=dynamic_cast<const osg::AlphaFunc*>(state->getAttribute(osg::StateAttribute::ALPHAFUNC));alpha && alpha->getFunction()!=GL_ALWAYS)
                        {info->detail=GeneratedStaticLod::Detail::Alpha;return true;}
                    }
                    if(const auto* group=node.asGroup()) for(unsigned i=0;i<group->getNumChildren();++i) if(self(self,*group->getChild(i),depth+1)) return true;
                    return false;
                };
                if(diagnose(diagnose,original,0)) return false;
            }
            osg::ref_ptr<osg::StateSet> empty=new osg::StateSet;
            std::vector<Leaf> leaves;collect(original,osg::Matrix::identity(),*empty,leaves);
            if(leaves.empty() || leaves.size()>64) return false;
            std::size_t totalVertices=0;
            std::vector<Role> roles;
            std::map<const osg::Program*,Shader::AssetGeometry::Snapshot> programs;
            for(auto& leaf:leaves)
            {
                const auto& g=*leaf.geometry;
                const auto* positions=dynamic_cast<const osg::Vec3Array*>(g.getVertexArray());
                const auto* normals=dynamic_cast<const osg::Vec3Array*>(g.getNormalArray());
                const auto* uv=dynamic_cast<const osg::Vec2Array*>(g.getTexCoordArray(0));
                if(!positions || !normals || !uv || positions->empty() || normals->size()!=positions->size()
                    || uv->size()!=positions->size() || g.getNumVertexAttribArrays() || g.getSecondaryColorArray() || g.getFogCoordArray()
                    || g.getNumPrimitiveSets()!=1 || !g.getPrimitiveSet(0)->getDrawElements()
                    || g.getPrimitiveSet(0)->getMode()!=GL_TRIANGLES || g.getPrimitiveSet(0)->getNumInstances()>1) return false;
                totalVertices+=positions->size();if(totalVertices>250000) return false;
                const auto* texture=dynamic_cast<const osg::Texture2D*>(leaf.state->getTextureAttribute(0,osg::StateAttribute::TEXTURE));
                if(!texture || !texture->getImage() || getTextureType(*leaf.state,*texture,0)!="diffuseMap") return false;
                qualifyRoles(leaf,*uv,roles);
                Chart domain{texture};extend(domain,*uv);qualifyDomain(domain);
                if(info) info->node=leaf.geometry;
                if(requireShippedObjectShader && !shippedProgram(*leaf.state,info,&programs)) return false;
            }
            ++HlodCounters::certified;return true;
        }
        catch(const std::exception&) {return false;}
    }

    HlodBuildResult buildSelectiveHlod(const osg::Node& certifiedSource, const osg::Node& optimizedControl,
        const HlodBuildOptions& options)
    {
        HlodBuildResult result;
        ++HlodCounters::requests;
        result.controlSubmissions = submissions(optimizedControl);
        try
        {
            if(options.reserveGpu && (!PagingWorkScope::optionalOptimization() || !Resource::SpeculativeScope::active()))
                throw std::runtime_error("production H1 requires the admitted optional worker scope");
            if (!immutable(certifiedSource)) throw std::runtime_error("source ancestry certificate no longer valid");
            if (!optimizedControl.getBound().valid() || optimizedControl.getBound().radius() > options.maxClusterRadius)
                throw std::runtime_error("cluster is not spatially bounded");
            osg::ref_ptr<osg::StateSet> empty = new osg::StateSet;
            std::vector<Leaf> leaves;
            collect(certifiedSource, osg::Matrix::identity(), *empty, leaves);
            if (leaves.size() < 2 || leaves.size() > options.maxSourceGeometries)
                throw std::runtime_error("source geometry admission limit");
            std::vector<Chart> charts;
            std::vector<Family> families;
            std::vector<Role> roles;
            std::map<const osg::Program*,Shader::AssetGeometry::Snapshot> programs;
            bool colorSpace=false;
            bool colorsPresent=false;
            float anisotropy=1.f;
            for(auto& leaf : leaves)
            {
                PagingWorkScope::checkpoint();
                const osg::Geometry& g=*leaf.geometry;
                const auto* vertices=dynamic_cast<const osg::Vec3Array*>(g.getVertexArray());
                const auto* normals=dynamic_cast<const osg::Vec3Array*>(g.getNormalArray());
                const auto* uv=dynamic_cast<const osg::Vec2Array*>(g.getTexCoordArray(0));
                if (!vertices || !normals || !uv || vertices->empty() || normals->size()!=vertices->size()
                    || uv->size()!=vertices->size() || g.getNormalBinding()!=osg::Geometry::BIND_PER_VERTEX
                    || g.getNumPrimitiveSets()!=1 || g.getPrimitiveSet(0)->getMode()!=GL_TRIANGLES
                    || !g.getPrimitiveSet(0)->getDrawElements() || g.getPrimitiveSet(0)->getNumIndices()%3
                    || g.getPrimitiveSet(0)->getNumIndices()<3 || g.getPrimitiveSet(0)->getNumIndices()>1500000
                    || g.getPrimitiveSet(0)->getNumInstances()>1)
                    throw std::runtime_error("unsupported position/normal/UV/index semantics");
                if(vertices->getDataVariance()==osg::Object::DYNAMIC || normals->getDataVariance()==osg::Object::DYNAMIC
                    || uv->getDataVariance()==osg::Object::DYNAMIC || g.getPrimitiveSet(0)->getDataVariance()==osg::Object::DYNAMIC)
                    throw std::runtime_error("dynamic source streams");
                if(vertices->size()>options.maxVertices || result.sourceVertices>options.maxVertices-vertices->size())
                    throw std::runtime_error("vertex admission limit");
                if (g.getNumVertexAttribArrays() || g.getSecondaryColorArray() || g.getFogCoordArray())
                    throw std::runtime_error("custom vertex effects");
                qualifyRoles(leaf,*uv,roles);
                for(std::size_t v=0;v<vertices->size();++v)
                {
                    for(unsigned axis=0;axis<3;++axis) if(!std::isfinite((*vertices)[v][axis]) || !std::isfinite((*normals)[v][axis]))
                        throw std::runtime_error("nonfinite vertex data");
                }
                for(unsigned i=0;i<g.getPrimitiveSet(0)->getNumIndices();++i)
                    if(g.getPrimitiveSet(0)->index(i)>=vertices->size()) throw std::runtime_error("out of range source index");
                const osg::Array* colors=g.getColorArray();
                if(colors && ((!dynamic_cast<const osg::Vec4Array*>(colors) && !dynamic_cast<const osg::Vec4ubArray*>(colors))
                    || colors->getDataVariance()==osg::Object::DYNAMIC
                    || colors->getNumElements()!=vertices->size() || g.getColorBinding()!=osg::Geometry::BIND_PER_VERTEX))
                    throw std::runtime_error("unsupported color stream");
                if(const auto* values=dynamic_cast<const osg::Vec4Array*>(colors))
                    for(const auto& value:*values) for(unsigned axis=0;axis<4;++axis)
                        if(!std::isfinite(value[axis])) throw std::runtime_error("nonfinite vertex color data");
                colorsPresent=colorsPresent || colors!=nullptr;
                leaf.texture=dynamic_cast<const osg::Texture2D*>(leaf.state->getTextureAttribute(0,osg::StateAttribute::TEXTURE));
                if(!leaf.texture || !leaf.texture->getImage() || leaf.texture->getNumImages()!=1
                    || leaf.texture->getDataVariance()==osg::Object::DYNAMIC || leaf.texture->getImage()->getDataVariance()==osg::Object::DYNAMIC
                    || getTextureType(*leaf.state,*leaf.texture,0)!="diffuseMap") throw std::runtime_error("requires one immutable diffuse binding");
                const auto* image=leaf.texture->getImage();
                if(!image->data() || image->s()<1 || image->s()>2048 || image->t()<1 || image->t()>2048 || image->r()!=1)
                    throw std::runtime_error("image source snapshot admission limit");
                if(leaf.texture->getFilter(osg::Texture::MIN_FILTER)!=osg::Texture::LINEAR_MIPMAP_LINEAR
                    || leaf.texture->getFilter(osg::Texture::MAG_FILTER)!=osg::Texture::LINEAR
                    || !std::isfinite(leaf.texture->getMaxAnisotropy()) || leaf.texture->getMaxAnisotropy()<1.f
                    || leaf.texture->getMaxAnisotropy()>16.f || leaf.texture->getShadowComparison()
                    || leaf.texture->getLODBias()!=0.f || leaf.texture->getMinLOD()>0.f)
                    throw std::runtime_error("sampler recipe is not qualified");
                for(auto axis : {osg::Texture::WRAP_S,osg::Texture::WRAP_T})
                    if(leaf.texture->getWrap(axis)!=osg::Texture::REPEAT && leaf.texture->getWrap(axis)!=osg::Texture::CLAMP_TO_EDGE)
                        throw std::runtime_error("border or mirrored sampler requires a separate bake contract");
                if(options.requireShippedObjectShader && !shippedProgram(*leaf.state,nullptr,&programs))
                    throw std::runtime_error("shader feature closure is not the shipped objects program");
                osg::ref_ptr<osg::StateSet> nontexture=new osg::StateSet(*leaf.state,osg::CopyOp::SHALLOW_COPY);
                for(unsigned unit=0;unit<roles.size();++unit)
                {
                    nontexture->removeTextureAttribute(unit,osg::StateAttribute::TEXTURE);
                    const auto* binding=leaf.textures[unit];const auto* roleImage=binding->getImage();
                    if(binding->getNumImages()!=1 || binding->getDataVariance()==osg::Object::DYNAMIC
                        || roleImage->getDataVariance()==osg::Object::DYNAMIC || !roleImage->data() || roleImage->s()<1
                        || roleImage->s()>2048 || roleImage->t()<1 || roleImage->t()>2048 || roleImage->r()!=1)
                        throw std::runtime_error("material role image snapshot admission limit");
                    if(binding->getFilter(osg::Texture::MIN_FILTER)!=leaf.texture->getFilter(osg::Texture::MIN_FILTER)
                        || binding->getFilter(osg::Texture::MAG_FILTER)!=leaf.texture->getFilter(osg::Texture::MAG_FILTER)
                        || binding->getWrap(osg::Texture::WRAP_S)!=leaf.texture->getWrap(osg::Texture::WRAP_S)
                        || binding->getWrap(osg::Texture::WRAP_T)!=leaf.texture->getWrap(osg::Texture::WRAP_T)
                        || binding->getMaxAnisotropy()!=leaf.texture->getMaxAnisotropy() || binding->getShadowComparison()
                        || binding->getLODBias()!=0.f || binding->getMinLOD()>0.f)
                        throw std::runtime_error("material role sampler families differ");
                    if(unit && srgb(*roleImage)) throw std::runtime_error("normal/specular role must use linear transfer");
                }
                if(families.empty()) {colorSpace=srgb(*leaf.texture->getImage());anisotropy=leaf.texture->getMaxAnisotropy();}
                else if(colorSpace!=srgb(*leaf.texture->getImage())
                    || anisotropy!=leaf.texture->getMaxAnisotropy()
                    || leaf.texture->getWrap(osg::Texture::WRAP_S)!=leaves.front().texture->getWrap(osg::Texture::WRAP_S)
                    || leaf.texture->getWrap(osg::Texture::WRAP_T)!=leaves.front().texture->getWrap(osg::Texture::WRAP_T))
                    throw std::runtime_error("scalar/material/shader/sampler families differ");
                auto family=std::find_if(families.begin(),families.end(),[&](const Family& value){
                    return value.colors==(colors!=nullptr) && value.state->compare(*nontexture,true)==0;});
                if(family==families.end()) {leaf.family=static_cast<unsigned>(families.size());families.push_back({nontexture,colors!=nullptr});}
                else leaf.family=static_cast<unsigned>(family-families.begin());
                auto chart=std::find_if(charts.begin(),charts.end(),[&](const Chart& value){
                    for(unsigned unit=0;unit<roles.size();++unit) if(value.textures[unit]->getImage()!=leaf.textures[unit]->getImage()) return false;
                    return true;});
                if(chart==charts.end()) {leaf.chart=static_cast<unsigned>(charts.size());charts.push_back({leaf.texture,leaf.textures});}
                else leaf.chart=static_cast<unsigned>(chart-charts.begin());
                extend(charts[leaf.chart],*uv);
                result.sourceVertices+=vertices->size();result.sourceTriangles+=g.getPrimitiveSet(0)->getNumIndices()/3;
            }
            std::vector<const osg::Image*> diffuseImages;
            for(const auto& chart:charts) if(std::find(diffuseImages.begin(),diffuseImages.end(),chart.texture->getImage())==diffuseImages.end())
                diffuseImages.push_back(chart.texture->getImage());
            result.distinctDiffuseImages=diffuseImages.size();
            for(auto& chart:charts) qualifyDomain(chart);
            if(charts.size()<2 || charts.size()>MaxCharts || result.controlSubmissions<=1)
                throw std::runtime_error("no residual differing-texture submissions beyond optimized control");
            if(result.sourceVertices>options.maxVertices) throw std::runtime_error("vertex admission limit");
            const unsigned grid=charts.size()<=4?2:4, extent=grid*TileSize;
            const unsigned samplingMaxLod=std::min(SafeMaxLod,static_cast<unsigned>(std::floor(std::log2(Gutter/anisotropy))));
            osg::Image::MipmapDataType offsets;
            std::size_t total=0;
            for(unsigned n=extent;n;n>>=1) {if(total) offsets.push_back(static_cast<unsigned>(total));total+=std::size_t(n)*n*4;}
            result.atlasBytes=total*roles.size();
            std::size_t sourceBytes=0;
            for(const auto& chart:charts) for(unsigned unit=0;unit<roles.size();++unit)
                sourceBytes+=std::size_t(chart.textures[unit]->getImage()->s())*chart.textures[unit]->getImage()->t()*sizeof(Rgba);
            const bool tangentsPresent=std::any_of(roles.begin(),roles.end(),[](const Role& role){return role.name=="normalMap";});
            const std::uint64_t outputBytes=result.atlasBytes+result.sourceVertices*(sizeof(osg::Vec3)*2+sizeof(osg::Vec2)
                +(tangentsPresent?sizeof(osg::Vec4):0)
                +(colorsPresent?sizeof(osg::Vec4):0))+result.sourceTriangles*3*sizeof(unsigned);
            const std::uint64_t peakBytes=2*result.atlasBytes+sourceBytes+outputBytes+charts.size()*ChartSize*ChartSize*sizeof(Rgba)*roles.size()
                +Resource::DerivedAssets::MetadataScratchBytes;
            if(peakBytes>options.maxHostBytes)
                throw std::runtime_error("host source/scratch/output admission limit");
            Resource::SpeculativeScope::Stage hostReservation(peakBytes,outputBytes);
            if(options.reserveGpu)
            {
                const std::uint64_t geometryBytes=result.sourceVertices*(sizeof(osg::Vec3)*2+sizeof(osg::Vec2)
                    +(tangentsPresent?sizeof(osg::Vec4):0)
                    +(colorsPresent?sizeof(osg::Vec4):0))+result.sourceTriangles*3*sizeof(unsigned);
                result.gpuReservation=Resource::AssetGpuBudget::reserve(result.atlasBytes+geometryBytes);
                if(!result.gpuReservation)
                { result.retryable=true; throw std::runtime_error("shared GPU admission denied or sample stale"); }
            }
            // This cache stores only the immutable atlas texture dependency,
            // never geometry, transforms or a publication generation. Exact
            // source pixel/block bytes are already owned by the winning loader
            // snapshot. No worker reopens a path and risks a different winner.
            std::array<osg::ref_ptr<osg::Texture2D>,3> atlasTextures;
            for(unsigned role=0;role<roles.size();++role)
            {
            const bool roleColorSpace=role==0 && colorSpace;
            Resource::DerivedAssets::Sha256 sourceDigest;
            for(const auto& chart:charts)
            {
                const auto& image=*chart.textures[role]->getImage();
                const std::array<std::uint64_t,9> metadata{static_cast<unsigned>(image.s()),static_cast<unsigned>(image.t()),
                    image.getPixelFormat(),static_cast<unsigned>(image.getInternalTextureFormat()),image.getDataType(),
                    static_cast<unsigned>(chart.texture->getWrap(osg::Texture::WRAP_S)),static_cast<unsigned>(chart.texture->getWrap(osg::Texture::WRAP_T)),
                    image.getPacking(),static_cast<unsigned>(image.getOrigin())};
                sourceDigest.append({reinterpret_cast<const std::uint8_t*>(metadata.data()),sizeof(metadata)});
                const std::array<std::int64_t,4> domain{chart.origin[0],chart.origin[1],chart.periods[0],chart.periods[1]};
                sourceDigest.append({reinterpret_cast<const std::uint8_t*>(domain.data()),sizeof(domain)});
                const auto path=Resource::DerivedAssets::digest(image.getFileName());
                sourceDigest.append({reinterpret_cast<const std::uint8_t*>(path.data()),path.size()});
                std::size_t sourceSize=image.getImageSizeInBytes();
                if(image.isCompressed())
                {
                    const unsigned f=image.getPixelFormat();
                    const bool bc1=f==0x83f0 || f==0x83f1 || f==0x8c4c || f==0x8c4d;
                    const bool bc2=f==0x83f2 || f==0x8c4e,bc3=f==0x83f3 || f==0x8c4f;
                    if(!bc1 && !bc2 && !bc3) throw std::runtime_error("unsupported compressed diffuse cache role");
                    sourceSize=std::size_t((image.s()+3)/4)*((image.t()+3)/4)*(bc1?8:16);
                }
                sourceDigest.append({image.data(),sourceSize});
            }
            const auto atlasKey=Resource::DerivedAssets::key(sourceDigest.finish(),
                "H1-atlas-v3-DXTex-4feb3e11-rgba-colortransfer-chart256-gutter128-repeat4-clamp-safemax7-fullmips-role-"+roles[role].name);
            auto cached=Resource::DerivedAssets::cache().read(atlasKey,total);
            const bool cacheHit=cached && cached->size()==total;
            std::vector<std::vector<Rgba>> images;
            if(!cacheHit)
                for(const auto& chart:charts) images.push_back(bakeChart(chart,roleColorSpace,role));
            auto* bytes=new unsigned char[total]{};
            osg::ref_ptr<osg::Image> atlas=new osg::Image;
            atlas->setImage(extent,extent,1,roleColorSpace?0x8c43:GL_RGBA8,GL_RGBA,GL_UNSIGNED_BYTE,bytes,osg::Image::USE_NEW_DELETE,1);
            atlas->setMipmapLevels(offsets);atlas->setDataVariance(osg::Object::STATIC);
            atlas->setOrigin(charts.front().texture->getImage()->getOrigin());
            if(cacheHit) std::memcpy(bytes,cached->data(),total);
            else for(unsigned level=0,n=extent;n;n>>=1,++level)
            {
                PagingWorkScope::checkpoint();
                auto* pixels=reinterpret_cast<Rgba*>(atlas->getMipmapData(level));
                if(level<=SafeMaxLod)
                {
                    const unsigned size=ChartSize>>level, gutter=Gutter>>level, tile=TileSize>>level;
                    for(unsigned chart=0;chart<charts.size();++chart)
                    {
                        const auto image=level?resize(images[chart],ChartSize,ChartSize,size,roleColorSpace):images[chart];
                        const bool repeatS=charts[chart].texture->getWrap(osg::Texture::WRAP_S)==osg::Texture::REPEAT;
                        const bool repeatT=charts[chart].texture->getWrap(osg::Texture::WRAP_T)==osg::Texture::REPEAT;
                        for(unsigned y=0;y<tile;++y) for(unsigned x=0;x<tile;++x)
                        {
                            int sx=int(x)-int(gutter),sy=int(y)-int(gutter);
                            sx=repeatS?(sx%int(size)+int(size))%int(size):std::clamp(sx,0,int(size)-1);
                            sy=repeatT?(sy%int(size)+int(size))%int(size):std::clamp(sy,0,int(size)-1);
                            pixels[std::size_t(chart/grid*tile+y)*n+chart%grid*tile+x]=image[std::size_t(sy)*size+sx];
                        }
                    }
                }
                else
                {
                    const auto* previous=reinterpret_cast<const Rgba*>(atlas->getMipmapData(level-1));
                    const auto image=resize(std::vector<Rgba>(previous,previous+std::size_t(n*2)*(n*2)),n*2,n*2,n,roleColorSpace);
                    std::memcpy(pixels,image.data(),image.size()*sizeof(Rgba));
                }
            }
            if(!cacheHit) Resource::DerivedAssets::cache().write(atlasKey,{bytes,total});
            osg::ref_ptr<osg::Texture2D> texture=new osg::Texture2D(atlas);
            texture->setDataVariance(osg::Object::STATIC);texture->setName(roles[role].name);
            texture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR_MIPMAP_LINEAR);
            texture->setFilter(osg::Texture::MAG_FILTER,osg::Texture::LINEAR);
            texture->setWrap(osg::Texture::WRAP_S,osg::Texture::CLAMP_TO_EDGE);texture->setWrap(osg::Texture::WRAP_T,osg::Texture::CLAMP_TO_EDGE);
            texture->setMaxAnisotropy(anisotropy);texture->setMaxLOD(static_cast<float>(samplingMaxLod));texture->setResizeNonPowerOfTwoHint(false);
            atlasTextures[role]=texture;
            }
            osg::ref_ptr<osg::Group> bounded = new osg::Group;
            bounded->setInitialBound(optimizedControl.getBound());
            bounded->setDataVariance(osg::Object::STATIC);
            // A proxy family replays only its matching original control draws.
            // Whole-cluster replay from every proxy would duplicate geometry.
            const auto sourcePaths=GeneratedStaticLod::originalDraws(certifiedSource);
            const auto controlPaths=GeneratedStaticLod::originalDraws(optimizedControl);
            if(&certifiedSource!=&optimizedControl && !GeneratedStaticLod::installOriginalDrawReceipts(const_cast<osg::Node&>(optimizedControl)))
                throw std::runtime_error("unqualified original control receipt");
            std::vector<std::vector<GeneratedStaticLod::OriginalDraw>> familyControls(families.size()),familySources(families.size());
            for(const auto& draw:sourcePaths)
            {
                const auto found=std::find_if(leaves.begin(),leaves.end(),[&](const auto& leaf){return leaf.geometry==draw.geometry;});
                if(found==leaves.end()) throw std::runtime_error("source replay lineage missing");
                familySources[found->family].push_back(draw);
            }
            for(const auto& draw:controlPaths)
            {
                const auto* callback=draw.geometry->getDrawCallback();
                if(callback && !dynamic_cast<const PersistentOpaquePackets::RetainedOpaqueDrawCallback*>(callback))
                    throw std::runtime_error("unqualified control replay callback");
                osg::ref_ptr<osg::StateSet> effective=new osg::StateSet;
                for(const auto& node:draw.path) if(node->getStateSet()) effective->merge(*node->getStateSet());
                if(!simpleState(*effective)) throw std::runtime_error("unqualified control replay material");
                for(unsigned role=0;role<roles.size();++role) effective->removeTextureAttribute(role,osg::StateAttribute::TEXTURE);
                const bool colors=draw.geometry->getColorArray()!=nullptr;
                const auto family=std::find_if(families.begin(),families.end(),[&](const auto& value){return value.colors==colors && value.state->compare(*effective,true)==0;});
                if(family==families.end()) throw std::runtime_error("control replay family differs from atlas source");
                familyControls[family-families.begin()].push_back(draw);
            }
            for(unsigned familyIndex=0;familyIndex<families.size();++familyIndex)
            {
            if(familyControls[familyIndex].empty()) throw std::runtime_error("empty original replay family");
            auto& family=families[familyIndex];
            for(unsigned role=0;role<roles.size();++role) family.state->setTextureAttribute(role,atlasTextures[role],osg::StateAttribute::ON);
            family.state->setDataVariance(osg::Object::STATIC);
            osg::ref_ptr<osg::Geometry> proxy=new osg::Geometry;
            osg::ref_ptr<osg::Vec3Array> vertices=new osg::Vec3Array,normals=new osg::Vec3Array;
            osg::ref_ptr<osg::Vec2Array> uv=new osg::Vec2Array;
            osg::ref_ptr<osg::Vec4Array> colors=new osg::Vec4Array;
            osg::ref_ptr<osg::Vec4Array> tangents=new osg::Vec4Array;
            osg::ref_ptr<osg::DrawElementsUInt> indices=new osg::DrawElementsUInt(GL_TRIANGLES);
            for(const auto& leaf:leaves)
            {
                if(leaf.family!=familyIndex) continue;
                const auto& g=*leaf.geometry; const unsigned base=static_cast<unsigned>(vertices->size());
                const auto& source=*static_cast<const osg::Vec3Array*>(g.getVertexArray());
                const auto& sourceNormals=*static_cast<const osg::Vec3Array*>(g.getNormalArray());
                const auto& sourceUv=*static_cast<const osg::Vec2Array*>(g.getTexCoordArray(0));
                for(unsigned v=0;v<source.size();++v)
                {
                    if(!(v%1024)) PagingWorkScope::checkpoint();
                    vertices->push_back(source[v]*leaf.matrix);
                    osg::Vec3 normal=osg::Matrix::transform3x3(sourceNormals[v],leaf.matrix);normal.normalize();normals->push_back(normal);
                    const auto& chart=charts[leaf.chart];
                    uv->push_back(osg::Vec2((leaf.chart%grid*TileSize+Gutter+(sourceUv[v].x()-chart.origin[0])/chart.periods[0]*ChartSize)/extent,
                        (leaf.chart/grid*TileSize+Gutter+(sourceUv[v].y()-chart.origin[1])/chart.periods[1]*ChartSize)/extent));
                    if(tangentsPresent)
                    {
                        const auto& original=(*static_cast<const osg::Vec4Array*>(g.getTexCoordArray(7)))[v];
                        osg::Vec3 tangent=osg::Matrix::transform3x3(osg::Vec3(original.x(),original.y(),original.z()),leaf.matrix);
                        tangent.normalize();tangents->push_back(osg::Vec4(tangent,original.w()));
                    }
                    if(family.colors)
                    {
                        if(const auto* floatColors=dynamic_cast<const osg::Vec4Array*>(g.getColorArray())) colors->push_back((*floatColors)[v]);
                        else {const auto& c=(*static_cast<const osg::Vec4ubArray*>(g.getColorArray()))[v];colors->push_back(osg::Vec4(c[0]/255.f,c[1]/255.f,c[2]/255.f,c[3]/255.f));}
                    }
                }
                for(unsigned i=0;i<g.getPrimitiveSet(0)->getNumIndices();++i) indices->push_back(base+g.getPrimitiveSet(0)->index(i));
            }
            proxy->setVertexArray(vertices);proxy->setNormalArray(normals,osg::Array::BIND_PER_VERTEX);
            proxy->setTexCoordArray(0,uv,osg::Array::BIND_PER_VERTEX);
            for(unsigned role=1;role<roles.size();++role) proxy->setTexCoordArray(role,uv,osg::Array::BIND_PER_VERTEX);
            if(tangentsPresent) proxy->setTexCoordArray(7,tangents,osg::Array::BIND_PER_VERTEX);
            if(family.colors) proxy->setColorArray(colors,osg::Array::BIND_PER_VERTEX);
            proxy->addPrimitiveSet(indices);proxy->setStateSet(family.state);proxy->setUseVertexBufferObjects(true);proxy->setUseDisplayList(false);
            proxy->setDataVariance(osg::Object::STATIC);proxy->setName("SelectiveFarMaterialAtlasHLOD");
            // This representation retains exactly the same positions/triangles:
            // L1 handles triangle reduction separately. Each qualified role has
            // an atlas; each exact scalar/shader family has one real draw across
            // previously incompatible source texture tuples.
            osg::ref_ptr<DrawReceipt> callback=new DrawReceipt(indices->size());
            proxy->setDrawCallback(callback);
            callback->candidateStates.emplace_back(proxy->getStateSet());
            callback->originals=familyControls[familyIndex];callback->proofSources=familySources[familyIndex];
            auto proof=callback->originals;proof.insert(proof.end(),callback->proofSources.begin(),callback->proofSources.end());
            GeneratedStaticLod::OriginalDraw tier{proxy,{proxy.get()},osg::Matrixd::identity()};
            callback->guard=GeneratedStaticLod::MaterialGuard::captureDraws(proof,tier,options.requireShippedObjectShader);
            if(!callback->guard) throw std::runtime_error("atlas source/control material replay proof unavailable");
            bounded->addChild(proxy);
            result.proxySubmissions+=proxy->getNumPrimitiveSets();
            }
            if(result.proxySubmissions>=result.controlSubmissions) throw std::runtime_error("no actual submission reduction");
            // Adopt the incremental atlas/vertex/index representation while its
            // original cooking reservation still exists. Guard snapshots own
            // separate canonical charges; control and source arrays stay shared.
            AssetPaging::retainExperimentalOwner(*bounded,result.atlasBytes+outputBytes
                +result.proxySubmissions*8192ull+65536,&optimizedControl);
            result.representation=bounded;
            ++HlodCounters::built;HlodCounters::controlSubmissions+=result.controlSubmissions;
            HlodCounters::proxySubmissions+=result.proxySubmissions;HlodCounters::atlasBytes+=result.atlasBytes;
        }
        catch(const PagingWorkCancelled&) {throw;}
        catch(const Resource::SpeculativeDeferred& error)
        {result.retryable=true;result.rejection=error.what();result.representation=nullptr;result.gpuReservation.reset();++HlodCounters::refused;}
        catch(const std::exception& error) {result.rejection=error.what();result.representation=nullptr;result.gpuReservation.reset();++HlodCounters::refused;}
        return result;
    }
}

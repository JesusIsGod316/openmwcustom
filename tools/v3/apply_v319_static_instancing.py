import os
import re
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def patch_once(path: Path, old: str, new: str, label: str) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label} anchor mismatch: {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")


# ---------------------------------------------------------------------------
# ObjectPaging: hardware-instance repeated distant static templates.
# Mode 0 = exact legacy behavior.
# Mode 1 = conservative: only replace repeated templates legacy merge rejects.
# Mode 2 = aggressive: prefer instancing for eligible repeated templates even
#          when legacy geometry merging would otherwise duplicate vertices.
# ---------------------------------------------------------------------------
obj = ROOT / "apps/openmw/mwrender/objectpaging.cpp"
text = obj.read_text(encoding="utf-8")

include_anchor = "#include <limits>\n"
if text.count(include_anchor) != 1:
    raise RuntimeError("V3.19 P1 objectpaging include anchor mismatch")
text = text.replace(
    include_anchor,
    "#include <limits>\n#include <cstdlib>\n\n#include <osg/Geometry>\n#include <osg/Uniform>\n",
    1,
)

create_anchor = "    osg::ref_ptr<osg::Node> ObjectPaging::createChunk(float size, const osg::Vec2f& center, bool activeGrid,\n"
if text.count(create_anchor) != 1:
    raise RuntimeError("V3.19 P1 createChunk anchor mismatch")
helper = r'''    namespace
    {
        constexpr unsigned int sV319MaxInstancesPerBatch = 8;

        int getV319StaticInstancingMode()
        {
            static const int mode = [] {
                const char* value = std::getenv("OPENMW_V319_STATIC_INSTANCING");
                if (!value || !*value)
                    return 0;
                return std::clamp(std::atoi(value), 0, 2);
            }();
            return mode;
        }

        struct V319InstanceCandidate
        {
            osg::Matrixf mMatrix;
            osg::ref_ptr<osg::Group> mLegacyNode;
            float mScale = 1.f;
        };

        class V319UnsupportedInstanceGraphVisitor : public osg::NodeVisitor
        {
        public:
            V319UnsupportedInstanceGraphVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Transform&) override { mUnsupported = true; }
            void apply(osg::MatrixTransform&) override { mUnsupported = true; }
            void apply(osg::LOD&) override { mUnsupported = true; }

            bool mUnsupported = false;
        };

        osg::BoundingBox v319TransformBoundingBox(const osg::BoundingBox& source, const osg::Matrixf& matrix)
        {
            osg::BoundingBox result;
            if (!source.valid())
                return result;

            const float xs[] = { source.xMin(), source.xMax() };
            const float ys[] = { source.yMin(), source.yMax() };
            const float zs[] = { source.zMin(), source.zMax() };
            for (float x : xs)
                for (float y : ys)
                    for (float z : zs)
                        result.expandBy(osg::Vec3f(x, y, z) * matrix);
            return result;
        }

        class V319ConfigureInstancedGeometryVisitor : public osg::NodeVisitor
        {
        public:
            explicit V319ConfigureInstancedGeometryVisitor(const std::vector<osg::Matrixf>& matrices)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mMatrices(matrices)
            {
            }

            void apply(osg::Geometry& geometry) override
            {
                mFoundGeometry = true;
                const osg::BoundingBox sourceBound = geometry.getBoundingBox();
                osg::BoundingBox instanceBound;
                for (const osg::Matrixf& matrix : mMatrices)
                    instanceBound.expandBy(v319TransformBoundingBox(sourceBound, matrix));
                if (instanceBound.valid())
                    geometry.setInitialBound(instanceBound);

                geometry.setUseDisplayList(false);
                geometry.setUseVertexBufferObjects(true);
                for (unsigned int i = 0; i < geometry.getNumPrimitiveSets(); ++i)
                    geometry.getPrimitiveSet(i)->setNumInstances(static_cast<int>(mMatrices.size()));
            }

            bool mFoundGeometry = false;

        private:
            const std::vector<osg::Matrixf>& mMatrices;
        };

        osg::ref_ptr<osg::Group> buildV319InstancedBatch(const osg::Node* source,
            const std::vector<osg::Matrixf>& matrices, float scale, const LODRange& lodDistances,
            osg::Node::NodeMask copyMask)
        {
            if (!source || matrices.size() < 2 || matrices.size() > sV319MaxInstancesPerBatch || scale <= 0.f)
                return nullptr;

            CopyOp instanceCopy(false, copyMask);
            instanceCopy.setCopyFlags(osg::CopyOp::DEEP_COPY_NODES | osg::CopyOp::DEEP_COPY_DRAWABLES
                | osg::CopyOp::DEEP_COPY_ARRAYS | osg::CopyOp::DEEP_COPY_PRIMITIVES);
            instanceCopy.mDistances = lodDistances / scale;

            osg::ref_ptr<osg::Group> flattened = new osg::Group;
            instanceCopy.copy(source, flattened);
            if (!flattened->getNumChildren())
                return nullptr;

            SceneUtil::Optimizer optimizer;
            optimizer.setIsOperationPermissibleForObjectCallback(new CanOptimizeCallback);
            optimizer.optimize(flattened,
                SceneUtil::Optimizer::FLATTEN_STATIC_TRANSFORMS | SceneUtil::Optimizer::REMOVE_REDUNDANT_NODES);

            // Hardware instancing inserts the per-reference transform in the shader.
            // Any remaining scene-graph transform or live LOD would make transform/LOD
            // semantics ambiguous, so fail closed to the exact legacy path.
            V319UnsupportedInstanceGraphVisitor unsupported;
            flattened->accept(unsupported);
            if (unsupported.mUnsupported)
                return nullptr;

            V319ConfigureInstancedGeometryVisitor configure(matrices);
            flattened->accept(configure);
            if (!configure.mFoundGeometry)
                return nullptr;

            osg::ref_ptr<osg::Group> batch = new osg::Group;
            osg::StateSet* stateSet = batch->getOrCreateStateSet();
            stateSet->addUniform(new osg::Uniform("v319StaticInstanceCount", static_cast<int>(matrices.size())));
            osg::ref_ptr<osg::Uniform> transforms
                = new osg::Uniform(osg::Uniform::FLOAT_MAT4, "v319StaticInstanceMatrix", sV319MaxInstancesPerBatch);
            const osg::Matrixf identity = osg::Matrixf::identity();
            for (unsigned int i = 0; i < sV319MaxInstancesPerBatch; ++i)
                transforms->setElement(i, i < matrices.size() ? matrices[i] : identity);
            stateSet->addUniform(transforms);
            batch->addChild(flattened);
            batch->setDataVariance(osg::Object::STATIC);
            return batch;
        }
    }

'''
text = text.replace(create_anchor, helper + create_anchor, 1)

pair_anchor = "            unsigned int numinstances = 0;\n            for (const PagedCellRef* refPtr : pair.second.mInstances)\n"
if text.count(pair_anchor) != 1:
    raise RuntimeError("V3.19 P1 pair-loop anchor mismatch")
text = text.replace(
    pair_anchor,
    "            unsigned int numinstances = 0;\n"
    "            const int v319InstancingMode = getV319StaticInstancingMode();\n"
    "            const bool v319CandidatePair = !activeGrid && v319InstancingMode > 0\n"
    "                && pair.second.mInstances.size() >= 2 && (!merge || v319InstancingMode >= 2);\n"
    "            std::vector<V319InstanceCandidate> v319Candidates;\n"
    "            bool v319UniformScale = true;\n"
    "            float v319Scale = 0.f;\n"
    "            for (const PagedCellRef* refPtr : pair.second.mInstances)\n",
    1,
)

transform_anchor = r'''                const osg::Vec3f nodeScale(ref.mScale, ref.mScale, ref.mScale);

                osg::ref_ptr<osg::Group> trans;
'''
if text.count(transform_anchor) != 1:
    raise RuntimeError("V3.19 P1 transform anchor mismatch")
text = text.replace(
    transform_anchor,
    r'''                const osg::Vec3f nodeScale(ref.mScale, ref.mScale, ref.mScale);

                osg::Matrixf v319InstanceMatrix;
                v319InstanceMatrix.preMultTranslate(nodePos);
                v319InstanceMatrix.preMultRotate(nodeAttitude);
                v319InstanceMatrix.preMultScale(nodeScale);

                osg::ref_ptr<osg::Group> trans;
''',
    1,
)

matrix_anchor = r'''                    osg::Matrixf matrix;
                    matrix.preMultTranslate(nodePos);
                    matrix.preMultRotate(nodeAttitude);
                    matrix.preMultScale(nodeScale);
                    trans = new osg::MatrixTransform(matrix);
'''
if text.count(matrix_anchor) != 1:
    raise RuntimeError("V3.19 P1 merge matrix anchor mismatch")
text = text.replace(matrix_anchor, r'''                    trans = new osg::MatrixTransform(v319InstanceMatrix);
''', 1)

attach_anchor = r'''                osg::Group* const attachTo = merge ? mergeGroup : group;
                attachTo->addChild(trans);
                ++numinstances;
            }
            if (numinstances > 0)
'''
if text.count(attach_anchor) != 1:
    raise RuntimeError("V3.19 P1 attach anchor mismatch")
text = text.replace(
    attach_anchor,
    r'''                osg::Group* const attachTo = merge ? mergeGroup : group;
                attachTo->addChild(trans);

                if (v319CandidatePair && ref.mScale > 0.f)
                {
                    if (v319Candidates.empty())
                        v319Scale = ref.mScale;
                    else if (std::abs(v319Scale - ref.mScale) > 1e-5f)
                        v319UniformScale = false;
                    v319Candidates.push_back({ v319InstanceMatrix, trans, ref.mScale });
                }
                ++numinstances;
            }

            if (v319CandidatePair && v319UniformScale && v319Candidates.size() >= 2)
            {
                std::vector<osg::ref_ptr<osg::Group>> v319Batches;
                bool v319BuiltAll = true;
                for (std::size_t first = 0; first < v319Candidates.size(); first += sV319MaxInstancesPerBatch)
                {
                    const std::size_t count
                        = std::min<std::size_t>(sV319MaxInstancesPerBatch, v319Candidates.size() - first);
                    if (count < 2)
                    {
                        v319BuiltAll = false;
                        break;
                    }
                    std::vector<osg::Matrixf> matrices;
                    matrices.reserve(count);
                    for (std::size_t i = 0; i < count; ++i)
                        matrices.push_back(v319Candidates[first + i].mMatrix);
                    osg::ref_ptr<osg::Group> batch
                        = buildV319InstancedBatch(cnode, matrices, v319Scale, lodDistances, copyMask);
                    if (!batch)
                    {
                        v319BuiltAll = false;
                        break;
                    }
                    v319Batches.push_back(batch);
                }

                if (v319BuiltAll && !v319Batches.empty())
                {
                    for (const V319InstanceCandidate& candidate : v319Candidates)
                    {
                        group->removeChild(candidate.mLegacyNode);
                        mergeGroup->removeChild(candidate.mLegacyNode);
                    }
                    for (const osg::ref_ptr<osg::Group>& batch : v319Batches)
                    {
                        group->addChild(batch);
                        if (compile)
                        {
                            stateToCompile._mode = osgUtil::GLObjectsVisitor::COMPILE_DISPLAY_LISTS;
                            batch->accept(stateToCompile);
                        }
                    }
                }
            }

            if (numinstances > 0)
''',
    1,
)

if "OPENMW_V319_STATIC_INSTANCING" not in text or "setNumInstances" not in text:
    raise RuntimeError("V3.19 P1 ObjectPaging markers missing")
obj.write_text(text, encoding="utf-8", newline="\n")


# ---------------------------------------------------------------------------
# GLSL 1.20 vertex paths. Uniform count 0 is exact legacy behavior. The
# instancing extension is a native OSG-supported path; eight matrices keep the
# uniform footprint conservative for the existing shader stack.
# ---------------------------------------------------------------------------
shader_header = r'''#extension GL_ARB_draw_instanced : enable

const int V319_STATIC_INSTANCE_MAX = 8;
uniform int v319StaticInstanceCount;
uniform mat4 v319StaticInstanceMatrix[V319_STATIC_INSTANCE_MAX];

vec4 v319StaticInstanceVertex(vec4 vertex)
{
    return v319StaticInstanceCount > 0 ? v319StaticInstanceMatrix[gl_InstanceID] * vertex : vertex;
}

mat3 v319StaticInstanceNormal()
{
    return v319StaticInstanceCount > 0 ? mat3(v319StaticInstanceMatrix[gl_InstanceID]) : mat3(1.0);
}

'''

for rel in (
    "files/shaders/compatibility/objects.vert",
    "files/shaders/compatibility/bs/default.vert",
    "files/shaders/compatibility/bs/nolighting.vert",
):
    path = ROOT / rel
    shader = path.read_text(encoding="utf-8")
    version = "#version 120\n\n"
    if shader.count(version) != 1:
        raise RuntimeError(f"V3.19 P1 shader version anchor mismatch: {rel}")
    shader = shader.replace(version, version + shader_header, 1)

    main_anchor = "void main(void)\n{\n"
    if shader.count(main_anchor) != 1:
        raise RuntimeError(f"V3.19 P1 shader main anchor mismatch: {rel}")
    shader = shader.replace(
        main_anchor,
        main_anchor
        + "    vec4 v319Vertex = v319StaticInstanceVertex(gl_Vertex);\n"
        + "    mat3 v319NormalMatrix = v319StaticInstanceNormal();\n\n",
        1,
    )
    shader = shader.replace("modelToClip(gl_Vertex)", "modelToClip(v319Vertex)")
    shader = shader.replace("modelToView(gl_Vertex)", "modelToView(v319Vertex)")

    if rel.endswith("objects.vert") or rel.endswith("bs/default.vert"):
        if "normalToViewMatrix = gl_NormalMatrix;" not in shader:
            raise RuntimeError(f"V3.19 P1 normal matrix anchor missing: {rel}")
        shader = shader.replace(
            "normalToViewMatrix = gl_NormalMatrix;",
            "normalToViewMatrix = gl_NormalMatrix * v319NormalMatrix;",
        )
        shader = shader.replace(
            "normalize(gl_NormalMatrix * passNormal)", "normalize(normalToViewMatrix * passNormal)"
        )
    else:
        shader = shader.replace(
            "gl_NormalMatrix * normalize(gl_Normal.xyz)",
            "gl_NormalMatrix * v319NormalMatrix * normalize(gl_Normal.xyz)",
        )
        shader = shader.replace(
            "normalize(gl_NormalMatrix * passNormal)",
            "normalize(gl_NormalMatrix * v319NormalMatrix * passNormal)",
        )

    if "v319StaticInstanceMatrix" not in shader or "v319Vertex" not in shader:
        raise RuntimeError(f"V3.19 P1 shader marker missing: {rel}")
    path.write_text(shader, encoding="utf-8", newline="\n")

shadow = ROOT / "files/shaders/compatibility/shadowcasting.vert"
shader = shadow.read_text(encoding="utf-8")
version = "#version 120\n\n"
if shader.count(version) != 1:
    raise RuntimeError("V3.19 P1 shadow shader version anchor mismatch")
shader = shader.replace(version, version + shader_header, 1)
main_anchor = "void main(void)\n{\n"
if shader.count(main_anchor) != 1:
    raise RuntimeError("V3.19 P1 shadow main anchor mismatch")
shader = shader.replace(main_anchor, main_anchor + "    vec4 v319Vertex = v319StaticInstanceVertex(gl_Vertex);\n\n", 1)
shader = shader.replace("gl_ModelViewProjectionMatrix * gl_Vertex", "gl_ModelViewProjectionMatrix * v319Vertex")
shader = shader.replace("gl_ModelViewMatrix * gl_Vertex", "gl_ModelViewMatrix * v319Vertex")
if "v319StaticInstanceMatrix" not in shader or "v319Vertex" not in shader:
    raise RuntimeError("V3.19 P1 shadow shader marker missing")
shadow.write_text(shader, encoding="utf-8", newline="\n")


# ---------------------------------------------------------------------------
# Runtime modes: preserve mode 103 as promoted control and add both P1 rungs.
# ---------------------------------------------------------------------------
launcher = ROOT / "tools/v3/launchers/V3_Lab.ps1"
text = launcher.read_text(encoding="utf-8")

menu108 = [line for line in text.splitlines() if line.startswith("Write-Host '108 = V3.19")]
if len(menu108) != 1:
    raise RuntimeError(f"V3.19 P1 launcher menu108 mismatch: {len(menu108)}")
text = text.replace(
    menu108[0],
    menu108[0]
    + "\nWrite-Host '109 = V3.19 P1 conservative static instancing + focus cadence2 + OSG auto'"
    + "\nWrite-Host '110 = V3.19 P1 aggressive static instancing + focus cadence2 + OSG auto'",
    1,
)

if text.count("Enter 1 through 108") != 1:
    raise RuntimeError("V3.19 P1 launcher choice prompt mismatch")
text = text.replace("Enter 1 through 108", "Enter 1 through 110", 1)
if text.count(",'108'))") != 1:
    raise RuntimeError("V3.19 P1 launcher allowed-list mismatch")
text = text.replace(",'108'))", ",'108','109','110'))", 1)

osg_default = "$V319OsgThreading = ''"
if text.count(osg_default) != 1:
    raise RuntimeError("V3.19 P1 launcher defaults mismatch")
text = text.replace(osg_default, osg_default + "\n$V319StaticInstancing = '0'", 1)

line103 = next((line for line in text.splitlines() if line.lstrip().startswith("'103'")), None)
line108 = next((line for line in text.splitlines() if line.lstrip().startswith("'108'")), None)
if not line103 or not line108:
    raise RuntimeError("V3.19 P1 launcher mode anchors missing")
body103 = line103[line103.index("{") + 1 : line103.rindex("}")].strip()
if "v319-focus2" not in body103:
    raise RuntimeError("V3.19 P1 expected focus2 control body")
mode109 = body103.replace("v319-focus2", "v319-p1-instancing-conservative", 1) + "; $V319StaticInstancing = '1'"
mode110 = body103.replace("v319-focus2", "v319-p1-instancing-aggressive", 1) + "; $V319StaticInstancing = '2'"
text = text.replace(
    line108,
    line108 + "\n        '109' { " + mode109 + " }\n        '110' { " + mode110 + " }",
    1,
)

manifest = '    "v319_osg_threading=$V319OsgThreading",'
if text.count(manifest) != 1:
    raise RuntimeError("V3.19 P1 launcher manifest mismatch")
text = text.replace(manifest, manifest + '\n    "v319_static_instancing=$V319StaticInstancing",', 1)

launch = "    $process = Start-Process -FilePath $Exe -WorkingDirectory $GameDir -PassThru"
if text.count(launch) != 1:
    raise RuntimeError("V3.19 P1 launcher start mismatch")
text = text.replace(launch, "    $env:OPENMW_V319_STATIC_INSTANCING = $V319StaticInstancing\n" + launch, 1)

for marker in (
    "Enter 1 through 110",
    "v319-p1-instancing-conservative",
    "v319-p1-instancing-aggressive",
    "v319_static_instancing=$V319StaticInstancing",
    "OPENMW_V319_STATIC_INSTANCING",
):
    if marker not in text:
        raise RuntimeError(f"V3.19 P1 launcher marker missing: {marker}")
launcher.write_text(text, encoding="utf-8", newline="\n")

print("V3.19 P1 static instancing modes 109/110 added")

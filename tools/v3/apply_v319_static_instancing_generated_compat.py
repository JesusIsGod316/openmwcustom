import base64
import hashlib
import io
import os
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()
base = HERE / "apply_v319_static_instancing.py"
source = base.read_text(encoding="utf-8")

# Capture the exact generated P0 shader sources before P1 runs. The P1 source
# layer historically rewrote these files globally; P1b restores them byte-for-
# byte and creates opt-in runtime variants instead.
SHADER_RELS = (
    "files/shaders/compatibility/objects.vert",
    "files/shaders/compatibility/bs/default.vert",
    "files/shaders/compatibility/bs/nolighting.vert",
    "files/shaders/compatibility/shadowcasting.vert",
)
control_source_bytes = {rel: (ROOT / rel).read_bytes() for rel in SHADER_RELS}

# Reconstruct the promoted Rafael PBR overlay so instanced variants are based on
# the actual runtime shader payload when that overlay replaces a base shader.
overlay_dir = ROOT / "files/shaders/v3overlay"
overlay_parts = (
    ("part00.b64", 8000),
    ("part01.b64", 8000),
    ("part02.b64", 8000),
    ("part03.b64", 8000),
    ("part04.b64", 8000),
    ("part05.b64", 8000),
    ("part06.b64", 8000),
    ("part07.b64", 4992),
)
overlay_b64 = ""
for name, expected_length in overlay_parts:
    payload = (overlay_dir / name).read_bytes().decode("ascii")
    if len(payload) < expected_length:
        raise RuntimeError(
            f"V3.19 P1b Rafael overlay chunk truncated: {name} "
            f"{len(payload)} < {expected_length}"
        )
    overlay_b64 += payload[:expected_length]

overlay_zip = base64.b64decode(overlay_b64)
overlay_sha = hashlib.sha256(overlay_zip).hexdigest()
if overlay_sha != "6f42a686e2a6a9038bbd4a9e2d0d1be6d8812b9b3e681d8b569560c2fe110255":
    raise RuntimeError(f"V3.19 P1b Rafael overlay checksum mismatch: {overlay_sha}")

runtime_control_bytes = dict(control_source_bytes)
runtime_control_origin = {rel: "generated-p0-source" for rel in SHADER_RELS}
with zipfile.ZipFile(io.BytesIO(overlay_zip), "r") as overlay:
    names = overlay.namelist()
    for rel in SHADER_RELS:
        suffix = rel.removeprefix("files/shaders/")
        matches = [name for name in names if name.replace("\\", "/").endswith(suffix)]
        if len(matches) > 1:
            raise RuntimeError(f"V3.19 P1b ambiguous Rafael overlay match for {rel}: {matches}")
        if matches:
            runtime_control_bytes[rel] = overlay.read(matches[0])
            runtime_control_origin[rel] = f"rafael-overlay:{matches[0]}"

# V3.12 replaces the pristine one-line attachTo selection with mutable spatial
# routing; V3.15 extends that same generated block with packetized routing. P1
# runs after both layers, so preserve their routing verbatim and anchor only on
# the invariant tail shared by the generated source.
old_anchor = '''attach_anchor = r\'''                osg::Group* const attachTo = merge ? mergeGroup : group;
                attachTo->addChild(trans);
                ++numinstances;
            }
            if (numinstances > 0)
\'''\n'''
new_anchor = '''attach_anchor = r\'''                attachTo->addChild(trans);
                ++numinstances;
            }
            if (numinstances > 0)
\'''\n'''
if source.count(old_anchor) != 1:
    raise RuntimeError(f"V3.19 P1 compat attach-anchor source mismatch: {source.count(old_anchor)}")
source = source.replace(old_anchor, new_anchor, 1)

old_replacement = '''    r\'''                osg::Group* const attachTo = merge ? mergeGroup : group;
                attachTo->addChild(trans);

                if (v319CandidatePair && ref.mScale > 0.f)
'''
new_replacement = '''    r\'''                attachTo->addChild(trans);

                if (v319CandidatePair && ref.mScale > 0.f)
'''
if source.count(old_replacement) != 1:
    raise RuntimeError(f"V3.19 P1 compat attach-replacement source mismatch: {source.count(old_replacement)}")
source = source.replace(old_replacement, new_replacement, 1)

# The runtime-mode layer writes the same "$V319OsgThreading = ''" substring
# into the defaults block and several switch cases. The original P1 count guard
# therefore sees multiple matches even though the defaults assignment is valid.
# Scope the insertion to the unique two-line defaults block instead.
old_defaults = '''osg_default = "$V319OsgThreading = ''"
if text.count(osg_default) != 1:
    raise RuntimeError("V3.19 P1 launcher defaults mismatch")
text = text.replace(osg_default, osg_default + "\\n$V319StaticInstancing = '0'", 1)
'''
new_defaults = '''osg_default = "$V319FocusCadence = '1'\\n$V319OsgThreading = ''"
if text.count(osg_default) != 1:
    raise RuntimeError("V3.19 P1 launcher defaults mismatch")
text = text.replace(osg_default, osg_default + "\\n$V319StaticInstancing = '0'", 1)
'''
if source.count(old_defaults) != 1:
    raise RuntimeError(f"V3.19 P1 compat launcher-default source mismatch: {source.count(old_defaults)}")
source = source.replace(old_defaults, new_defaults, 1)

# Execute the original P1 layer for ObjectPaging + launcher semantics. Its
# global shader edits are intentionally captured only as a compatibility check;
# P1b restores the control shaders and emits separate variants below.
exec(compile(source, str(base), "exec"), {"__file__": str(base), "__name__": "__main__"})

for rel, before in control_source_bytes.items():
    path = ROOT / rel
    generated_p1 = path.read_bytes()
    if generated_p1 == before:
        raise RuntimeError(f"V3.19 P1b expected original P1 shader mutation did not occur: {rel}")
    path.write_bytes(before)
    if path.read_bytes() != before:
        raise RuntimeError(f"V3.19 P1b failed restoring byte-identical control shader: {rel}")

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


def build_instanced_variant(rel: str, raw: bytes) -> bytes:
    shader = raw.decode("utf-8").replace("\r\n", "\n")
    version = "#version 120\n\n"
    if shader.count(version) != 1:
        raise RuntimeError(f"V3.19 P1b runtime shader version anchor mismatch: {rel}")
    shader = shader.replace(version, version + shader_header, 1)

    main_anchor = "void main(void)\n{\n"
    if shader.count(main_anchor) != 1:
        raise RuntimeError(f"V3.19 P1b runtime shader main anchor mismatch: {rel}")

    if rel.endswith("shadowcasting.vert"):
        shader = shader.replace(
            main_anchor,
            main_anchor + "    vec4 v319Vertex = v319StaticInstanceVertex(gl_Vertex);\n\n",
            1,
        )
        if shader.count("gl_ModelViewProjectionMatrix * gl_Vertex") < 1:
            raise RuntimeError("V3.19 P1b shadow MVP anchor missing")
        shader = shader.replace(
            "gl_ModelViewProjectionMatrix * gl_Vertex",
            "gl_ModelViewProjectionMatrix * v319Vertex",
        )
        shader = shader.replace(
            "gl_ModelViewMatrix * gl_Vertex",
            "gl_ModelViewMatrix * v319Vertex",
        )
    else:
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
                raise RuntimeError(f"V3.19 P1b normal matrix anchor missing: {rel}")
            shader = shader.replace(
                "normalToViewMatrix = gl_NormalMatrix;",
                "normalToViewMatrix = gl_NormalMatrix * v319NormalMatrix;",
            )
            shader = shader.replace(
                "normalize(gl_NormalMatrix * passNormal)",
                "normalize(normalToViewMatrix * passNormal)",
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
        raise RuntimeError(f"V3.19 P1b variant marker missing after transform: {rel}")
    return shader.encode("utf-8")


variant_relpaths = {}
provenance_lines = [
    "version=V3.19-P1b",
    "mode0_shader_control=byte-identical",
    f"rafael_overlay_sha256={overlay_sha}",
]
for rel in SHADER_RELS:
    path = ROOT / rel
    variant = path.with_name(path.stem + "_v319_instanced" + path.suffix)
    variant_rel = variant.relative_to(ROOT).as_posix()
    variant_bytes = build_instanced_variant(rel, runtime_control_bytes[rel])
    variant.write_bytes(variant_bytes)
    variant_relpaths[rel] = variant_rel

    source_sha = hashlib.sha256(control_source_bytes[rel]).hexdigest()
    restored_sha = hashlib.sha256(path.read_bytes()).hexdigest()
    runtime_sha = hashlib.sha256(runtime_control_bytes[rel]).hexdigest()
    variant_sha = hashlib.sha256(variant_bytes).hexdigest()
    if source_sha != restored_sha:
        raise RuntimeError(f"V3.19 P1b control shader hash changed after restore: {rel}")
    provenance_lines.append(
        f"{rel}|source_control_sha256={source_sha}|restored_sha256={restored_sha}"
        f"|runtime_control_sha256={runtime_sha}|variant_sha256={variant_sha}"
        f"|runtime_origin={runtime_control_origin[rel]}"
    )

# The shader resource list is explicit. Ensure all generated variants are copied
# into resources before the Rafael overlay is extracted.
shader_cmake = ROOT / "files/shaders/CMakeLists.txt"
cmake_text = shader_cmake.read_text(encoding="utf-8")
cmake_anchor = """    compatibility/objects.vert
    compatibility/objects.frag
"""
if cmake_text.count(cmake_anchor) != 1:
    raise RuntimeError("V3.19 P1b shader CMake objects anchor mismatch")
cmake_text = cmake_text.replace(
    cmake_anchor,
    """    compatibility/objects.vert
    compatibility/objects_v319_instanced.vert
    compatibility/objects.frag
""",
    1,
)
cmake_anchor = """    compatibility/shadowcasting.vert
    compatibility/shadowcasting.frag
"""
if cmake_text.count(cmake_anchor) != 1:
    raise RuntimeError("V3.19 P1b shader CMake shadow anchor mismatch")
cmake_text = cmake_text.replace(
    cmake_anchor,
    """    compatibility/shadowcasting.vert
    compatibility/shadowcasting_v319_instanced.vert
    compatibility/shadowcasting.frag
""",
    1,
)
cmake_anchor = """    compatibility/bs/default.vert
    compatibility/bs/default.frag
    compatibility/bs/nolighting.vert
    compatibility/bs/nolighting.frag
"""
if cmake_text.count(cmake_anchor) != 1:
    raise RuntimeError("V3.19 P1b shader CMake BS anchor mismatch")
cmake_text = cmake_text.replace(
    cmake_anchor,
    """    compatibility/bs/default.vert
    compatibility/bs/default_v319_instanced.vert
    compatibility/bs/default.frag
    compatibility/bs/nolighting.vert
    compatibility/bs/nolighting_v319_instanced.vert
    compatibility/bs/nolighting.frag
""",
    1,
)
for rel in variant_relpaths.values():
    cmake_rel = rel.removeprefix("files/shaders/")
    if cmake_text.count(cmake_rel) != 1:
        raise RuntimeError(f"V3.19 P1b shader resource variant not listed exactly once: {cmake_rel}")
shader_cmake.write_text(cmake_text, encoding="utf-8", newline="\n")

# Normal object shaders: mode 0 follows the original getProgram(prefix, ...)
# path verbatim. Modes 1/2 opt into a distinct vertex shader while retaining the
# same fragment shader, defines, program template, and sampler bindings.
shader_visitor = ROOT / "components/shader/shadervisitor.cpp"
visitor_text = shader_visitor.read_text(encoding="utf-8")
include_anchor = '#include "shadervisitor.hpp"\n\n#include <set>\n'
if visitor_text.count(include_anchor) != 1:
    raise RuntimeError("V3.19 P1b ShaderVisitor include anchor mismatch")
visitor_text = visitor_text.replace(
    include_anchor,
    '#include "shadervisitor.hpp"\n\n#include <cstdlib>\n#include <set>\n#include <stdexcept>\n',
    1,
)
namespace_anchor = "namespace Shader\n{\n    /**\n"
if visitor_text.count(namespace_anchor) != 1:
    raise RuntimeError("V3.19 P1b ShaderVisitor namespace anchor mismatch")
visitor_text = visitor_text.replace(
    namespace_anchor,
    r'''namespace Shader
{
    namespace
    {
        bool v319StaticInstancingShaderVariantsEnabled()
        {
            static const bool enabled = [] {
                const char* value = std::getenv("OPENMW_V319_STATIC_INSTANCING");
                return value && *value && std::atoi(value) > 0;
            }();
            return enabled;
        }

        bool v319UsesInstancedVertexVariant(const std::string& shaderPrefix)
        {
            return shaderPrefix == "objects" || shaderPrefix == "bs/default" || shaderPrefix == "bs/nolighting";
        }
    }

    /**
''',
    1,
)
program_anchor = "        auto program = mShaderManager.getProgram(shaderPrefix, defineMap, mProgramTemplate, samplers);\n"
if visitor_text.count(program_anchor) != 1:
    raise RuntimeError("V3.19 P1b ShaderVisitor program anchor mismatch")
visitor_text = visitor_text.replace(
    program_anchor,
    r'''        osg::ref_ptr<osg::Program> program;
        if (v319StaticInstancingShaderVariantsEnabled() && v319UsesInstancedVertexVariant(shaderPrefix))
        {
            auto vertexShader = mShaderManager.getShader(shaderPrefix + "_v319_instanced.vert", defineMap);
            auto fragmentShader = mShaderManager.getShader(shaderPrefix + ".frag", defineMap);
            if (!vertexShader || !fragmentShader)
                throw std::runtime_error("V3.19 P1b failed initializing instanced shader: " + shaderPrefix);
            program = mShaderManager.getProgram(
                std::move(vertexShader), std::move(fragmentShader), mProgramTemplate, samplers);
        }
        else
            program = mShaderManager.getProgram(shaderPrefix, defineMap, mProgramTemplate, samplers);
''',
    1,
)
for marker in (
    "v319StaticInstancingShaderVariantsEnabled",
    'shaderPrefix + "_v319_instanced.vert"',
    "program = mShaderManager.getProgram(shaderPrefix, defineMap, mProgramTemplate, samplers);",
):
    if marker not in visitor_text:
        raise RuntimeError(f"V3.19 P1b ShaderVisitor routing marker missing: {marker}")
shader_visitor.write_text(visitor_text, encoding="utf-8", newline="\n")

# Shadow casting has its own program construction path. Keep shadowcasting.vert
# exact for mode 0; select only the P1b vertex variant for modes 1/2.
shadow_cpp = ROOT / "components/sceneutil/mwshadowtechnique.cpp"
shadow_text = shadow_cpp.read_text(encoding="utf-8")
include_anchor = "#include <sstream>\n#include <vector>\n"
if shadow_text.count(include_anchor) != 1:
    raise RuntimeError("V3.19 P1b shadow technique include anchor mismatch")
shadow_text = shadow_text.replace(
    include_anchor,
    "#include <cstdlib>\n#include <sstream>\n#include <vector>\n",
    1,
)
helper_anchor = "using namespace osgShadow;\nusing namespace SceneUtil;\n\n#define dbl_max"
if shadow_text.count(helper_anchor) != 1:
    raise RuntimeError("V3.19 P1b shadow technique helper anchor mismatch")
shadow_text = shadow_text.replace(
    helper_anchor,
    r'''using namespace osgShadow;
using namespace SceneUtil;

bool v319StaticInstancingShadowVariantEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("OPENMW_V319_STATIC_INSTANCING");
        return value && *value && std::atoi(value) > 0;
    }();
    return enabled;
}

#define dbl_max''',
    1,
)
casting_anchor = '    osg::ref_ptr<osg::Shader> castingVertexShader = shaderManager.getShader("shadowcasting.vert");\n'
if shadow_text.count(casting_anchor) != 1:
    raise RuntimeError("V3.19 P1b shadow casting shader anchor mismatch")
shadow_text = shadow_text.replace(
    casting_anchor,
    r'''    const char* castingVertexShaderName = v319StaticInstancingShadowVariantEnabled()
        ? "shadowcasting_v319_instanced.vert"
        : "shadowcasting.vert";
    osg::ref_ptr<osg::Shader> castingVertexShader = shaderManager.getShader(castingVertexShaderName);
''',
    1,
)
for marker in (
    "v319StaticInstancingShadowVariantEnabled",
    '"shadowcasting_v319_instanced.vert"',
    ': "shadowcasting.vert";',
):
    if marker not in shadow_text:
        raise RuntimeError(f"V3.19 P1b shadow routing marker missing: {marker}")
shadow_cpp.write_text(shadow_text, encoding="utf-8", newline="\n")

(ROOT / "V3.19-P1-SHADER-CONTROL.txt").write_text(
    "\n".join(provenance_lines) + "\n", encoding="utf-8", newline="\n"
)

print("V3.19 P1b semantic-control shader variants applied; mode 0 base shaders restored byte-for-byte")

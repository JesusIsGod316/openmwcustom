import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_once(path: Path, old: str, new: str, label: str) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label} anchor mismatch: {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")


# P1b correctness repair: make non-instanced shared-shader state deterministic.
# V3.12/V3.15 rewrite everything after this unique group creation, so anchor only
# on the stable line that survives the complete generated-source lineage.
objectpaging = ROOT / "apps/openmw/mwrender/objectpaging.cpp"
replace_once(
    objectpaging,
    "        osg::ref_ptr<osg::Group> group = new osg::Group;\n",
    """        osg::ref_ptr<osg::Group> group = new osg::Group;
        // V3.19 P1b: shared shader programs must always see an explicit legacy
        // zero-state outside an instanced batch. Child batch StateSets override
        // this value with their positive instance count.
        group->getOrCreateStateSet()->addUniform(new osg::Uniform("v319StaticInstanceCount", 0));
""",
    "V3.19 P1b ObjectPaging zero-state",
)

# P1 reused tangent-composed normalToViewMatrix for viewNormal. That changes
# normal-mapped material/shadow behavior even when instancing is disabled.
# Keep only the declarations globally, then branch in main and preserve P0 math.
p1_header = """#extension GL_ARB_draw_instanced : enable

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

"""
p1b_header = """#extension GL_ARB_draw_instanced : enable

const int V319_STATIC_INSTANCE_MAX = 8;
uniform int v319StaticInstanceCount;
uniform mat4 v319StaticInstanceMatrix[V319_STATIC_INSTANCE_MAX];

"""

main_old = """void main(void)
{
    vec4 v319Vertex = v319StaticInstanceVertex(gl_Vertex);
    mat3 v319NormalMatrix = v319StaticInstanceNormal();

"""
main_new = """void main(void)
{
    // P1b keeps the P0 path literal unless this draw is explicitly instanced.
    vec4 v319Vertex = gl_Vertex;
    mat3 v319BaseNormalToView = gl_NormalMatrix;
    if (v319StaticInstanceCount > 0)
    {
        mat4 v319Instance = v319StaticInstanceMatrix[gl_InstanceID];
        v319Vertex = v319Instance * gl_Vertex;
        v319BaseNormalToView = gl_NormalMatrix * mat3(v319Instance);
    }

"""

shader_paths = (
    ROOT / "files/shaders/compatibility/objects.vert",
    ROOT / "files/shaders/compatibility/bs/default.vert",
    ROOT / "files/shaders/compatibility/bs/nolighting.vert",
)
for path in shader_paths:
    replace_once(path, p1_header, p1b_header, f"V3.19 P1b shader header {path}")
    replace_once(path, main_old, main_new, f"V3.19 P1b shader main {path}")

objects = ROOT / "files/shaders/compatibility/objects.vert"
text = objects.read_text(encoding="utf-8")
if text.count("normalToViewMatrix = gl_NormalMatrix * v319NormalMatrix;") != 1:
    raise RuntimeError("V3.19 P1b objects base-normal anchor mismatch")
text = text.replace(
    "normalToViewMatrix = gl_NormalMatrix * v319NormalMatrix;",
    "normalToViewMatrix = v319BaseNormalToView;",
    1,
)
if text.count("normalize(normalToViewMatrix * passNormal)") != 1:
    raise RuntimeError("V3.19 P1b objects view-normal anchor mismatch")
text = text.replace(
    "normalize(normalToViewMatrix * passNormal)",
    "normalize(v319BaseNormalToView * passNormal)",
    1,
)
particle_old = "orthoDepthMapCoord = ((depthSpaceMatrix * model) * vec4(gl_Vertex.xyz, 1.0)).xyz;"
if text.count(particle_old) != 1:
    raise RuntimeError("V3.19 P1b objects particle-occlusion anchor mismatch")
text = text.replace(
    particle_old,
    "orthoDepthMapCoord = ((depthSpaceMatrix * model) * v319Vertex).xyz;",
    1,
)
objects.write_text(text, encoding="utf-8", newline="\n")

bs_default = ROOT / "files/shaders/compatibility/bs/default.vert"
text = bs_default.read_text(encoding="utf-8")
if text.count("normalToViewMatrix = gl_NormalMatrix * v319NormalMatrix;") != 1:
    raise RuntimeError("V3.19 P1b BS default base-normal anchor mismatch")
text = text.replace(
    "normalToViewMatrix = gl_NormalMatrix * v319NormalMatrix;",
    "normalToViewMatrix = v319BaseNormalToView;",
    1,
)
if text.count("normalize(normalToViewMatrix * passNormal)") != 1:
    raise RuntimeError("V3.19 P1b BS default view-normal anchor mismatch")
text = text.replace(
    "normalize(normalToViewMatrix * passNormal)",
    "normalize(v319BaseNormalToView * passNormal)",
    1,
)
bs_default.write_text(text, encoding="utf-8", newline="\n")

bs_nolighting = ROOT / "files/shaders/compatibility/bs/nolighting.vert"
text = bs_nolighting.read_text(encoding="utf-8")
old_falloff = "gl_NormalMatrix * v319NormalMatrix * normalize(gl_Normal.xyz)"
old_shadow = "normalize(gl_NormalMatrix * v319NormalMatrix * passNormal)"
if text.count(old_falloff) != 1 or text.count(old_shadow) != 1:
    raise RuntimeError(
        f"V3.19 P1b BS nolighting normal anchors mismatch: "
        f"{text.count(old_falloff)}, {text.count(old_shadow)}"
    )
text = text.replace(old_falloff, "v319BaseNormalToView * normalize(gl_Normal.xyz)", 1)
text = text.replace(old_shadow, "normalize(v319BaseNormalToView * passNormal)", 1)
bs_nolighting.write_text(text, encoding="utf-8", newline="\n")

shadow = ROOT / "files/shaders/compatibility/shadowcasting.vert"
replace_once(shadow, p1_header, p1b_header, "V3.19 P1b shadow header")
replace_once(
    shadow,
    """void main(void)
{
    vec4 v319Vertex = v319StaticInstanceVertex(gl_Vertex);

""",
    """void main(void)
{
    // P1b: the non-instanced shadow path is the original P0 vertex exactly.
    vec4 v319Vertex = gl_Vertex;
    if (v319StaticInstanceCount > 0)
        v319Vertex = v319StaticInstanceMatrix[gl_InstanceID] * gl_Vertex;

""",
    "V3.19 P1b shadow main",
)

# Extend executable provenance so this artifact cannot be mistaken for P1.
engine = ROOT / "apps/openmw/engine.cpp"
text = engine.read_text(encoding="utf-8")
identity = "openmw-custom-v3.19-cpu-p1"
if text.count(identity) != 1:
    raise RuntimeError(f"V3.19 P1b identity anchor mismatch: {text.count(identity)}")
text = text.replace(identity, identity + " / openmw-custom-v3.19-cpu-p1b", 1)
engine.write_text(text, encoding="utf-8", newline="\n")

# Fail closed on the exact correctness properties P1b exists to enforce.
objectpaging_text = objectpaging.read_text(encoding="utf-8")
if objectpaging_text.count('new osg::Uniform("v319StaticInstanceCount", 0)') != 1:
    raise RuntimeError("V3.19 P1b deterministic zero-state missing")
for path in shader_paths:
    shader = path.read_text(encoding="utf-8")
    if "v319StaticInstanceVertex(" in shader or "v319StaticInstanceNormal(" in shader:
        raise RuntimeError(f"V3.19 P1b stale shared helper remains: {path}")
    if "vec4 v319Vertex = gl_Vertex;" not in shader or "v319BaseNormalToView = gl_NormalMatrix;" not in shader:
        raise RuntimeError(f"V3.19 P1b literal legacy path missing: {path}")
    if "if (v319StaticInstanceCount > 0)" not in shader:
        raise RuntimeError(f"V3.19 P1b opt-in instance branch missing: {path}")

objects_text = objects.read_text(encoding="utf-8")
bs_default_text = bs_default.read_text(encoding="utf-8")
if "normalize(normalToViewMatrix * passNormal)" in objects_text:
    raise RuntimeError("V3.19 P1b objects tangent-space shadow-normal regression remains")
if "normalize(normalToViewMatrix * passNormal)" in bs_default_text:
    raise RuntimeError("V3.19 P1b BS tangent-space shadow-normal regression remains")
if "orthoDepthMapCoord = ((depthSpaceMatrix * model) * v319Vertex).xyz;" not in objects_text:
    raise RuntimeError("V3.19 P1b particle-occlusion instance transform missing")

print("V3.19 P1b visual correctness isolation applied")

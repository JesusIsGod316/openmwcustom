from pathlib import Path
import os

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_once(path: Path, old: str, new: str, label: str) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"V3.18 NIS compile-fix {label} anchor mismatch in {path}: found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")


def chunk_shader_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    begin_token = 'inline constexpr const char* kComputeShader = R"V318NIS('
    end_token = ')V318NIS";'
    if text.count(begin_token) != 1 or text.count(end_token) != 1:
        raise RuntimeError("V3.18 NIS shader-header anchor mismatch before MSVC literal chunking")

    begin = text.index(begin_token)
    shader_begin = begin + len(begin_token)
    shader_end = text.index(end_token, shader_begin)
    shader = text[shader_begin:shader_end]

    # MSVC C2026 limits the size of an individual string literal. Keep every raw
    # literal comfortably below that ceiling and concatenate at runtime once.
    # Split on line boundaries so the emitted GLSL remains byte-for-byte identical.
    max_chunk = 12000
    chunks: list[str] = []
    offset = 0
    while offset < len(shader):
        target = min(offset + max_chunk, len(shader))
        if target < len(shader):
            cut = shader.rfind("\n", offset, target)
            if cut <= offset:
                cut = target
            else:
                cut += 1
        else:
            cut = len(shader)
        chunks.append(shader[offset:cut])
        offset = cut

    if not chunks or "".join(chunks) != shader:
        raise RuntimeError("V3.18 NIS shader chunk reconstruction failed")

    raw_parts: list[str] = []
    for index, chunk in enumerate(chunks):
        delimiter = f"V318N{index}"
        if len(delimiter) > 16:
            raise RuntimeError("V3.18 NIS raw-string delimiter exceeded C++ limit")
        if f'){delimiter}"' in chunk:
            raise RuntimeError(f"V3.18 NIS raw-string delimiter collision in chunk {index}")
        raw_parts.append(f'        R"{delimiter}({chunk}){delimiter}"')

    replacement = (
        "inline const std::string& computeShader()\n"
        "{\n"
        "    static constexpr std::string_view parts[] = {\n"
        + ",\n".join(raw_parts)
        + "\n    };\n"
        "    static const std::string shader = [] {\n"
        "        std::string out;\n"
        f"        out.reserve({len(shader)}u);\n"
        "        for (std::string_view part : parts)\n"
        "            out.append(part.data(), part.size());\n"
        "        return out;\n"
        "    }();\n"
        "    return shader;\n"
        "}\n"
    )

    new_text = text[:begin] + replacement + text[shader_end + len(end_token):]
    if '#include <string>' not in new_text:
        new_text = new_text.replace(
            '#pragma once\n\n',
            '#pragma once\n\n#include <string>\n#include <string_view>\n\n',
            1,
        )
    path.write_text(new_text, encoding="utf-8", newline="\n")


# osg::ref_ptr<T>'s destructor calls T::unref(), so T must be complete wherever
# the owning class destructor is instantiated. nisscaler.hpp intentionally only
# forward-declares osg::BindImageTexture to avoid exposing the heavy OSG header to
# every PingPongCanvas consumer. Make NisScaler destruction out-of-line instead;
# nisscaler.cpp already includes <osg/BindImageTexture>, so the type is complete
# at the one point where ref_ptr<BindImageTexture> is destroyed.
hpp = ROOT / "apps/openmw/mwrender/nisscaler.hpp"
replace_once(
    hpp,
    "        NisScaler();\n\n        // Returns a native-resolution NIS output texture on success. Returns\n",
    "        NisScaler();\n        ~NisScaler();\n\n        // Returns a native-resolution NIS output texture on success. Returns\n",
    "destructor declaration",
)

# osg::buffered_value<T> stores values in std::vector<T> and returns T&. T=bool
# selects std::vector<bool>'s proxy-reference specialization, which cannot satisfy
# that API under MSVC (C2440 in run 33274105585). Preserve boolean semantics with
# ordinary byte storage so operator[] returns a real unsigned-char reference.
replace_once(
    hpp,
    "        mutable osg::buffered_value<bool> mLoggedActive;\n",
    "        mutable osg::buffered_value<unsigned char> mLoggedActive;\n",
    "buffered active-log storage",
)

cpp = ROOT / "apps/openmw/mwrender/nisscaler.cpp"
replace_once(
    cpp,
    "    void NisScaler::resizeGLObjectBuffers(unsigned int maxSize)\n",
    "    NisScaler::~NisScaler() = default;\n\n    void NisScaler::resizeGLObjectBuffers(unsigned int maxSize)\n",
    "out-of-line destructor definition",
)

# The OpenSceneGraph GL compatibility headers used by the pinned Windows vcpkg
# dependency expose the ARB floating-point internal-format token rather than the
# newer unsuffixed GL_RGBA32F spelling. OSG itself uses GL_RGBA32F_ARB for this
# format, so use the compatibility spelling here too.
replace_once(cpp, "setInternalFormat(GL_RGBA32F);", "setInternalFormat(GL_RGBA32F_ARB);", "RGBA32F token")

# MSVC also rejects the full NVIDIA compute shader as one enormous raw string
# literal (C2026). Emit <=12 KiB raw-string chunks and reconstruct the source once
# at runtime before osg::Shader receives it.
shader_hpp = ROOT / "apps/openmw/mwrender/v318_nis_shader.hpp"
chunk_shader_header(shader_hpp)
replace_once(
    cpp,
    "new osg::Shader(osg::Shader::COMPUTE, V318Nis::kComputeShader)",
    "new osg::Shader(osg::Shader::COMPUTE, V318Nis::computeShader())",
    "chunked shader accessor",
)

# Fail closed if a future generator edit accidentally removes any known Windows
# compile correction or reintroduces osg::buffered_value<bool>.
hpp_text = hpp.read_text(encoding="utf-8")
cpp_text = cpp.read_text(encoding="utf-8")
shader_text = shader_hpp.read_text(encoding="utf-8")
assert "class BindImageTexture;" in hpp_text
assert "~NisScaler();" in hpp_text
assert "osg::buffered_value<bool>" not in hpp_text
assert "osg::buffered_value<unsigned char> mLoggedActive;" in hpp_text
assert "#include <osg/BindImageTexture>" in cpp_text
assert "NisScaler::~NisScaler() = default;" in cpp_text
assert "GL_RGBA32F_ARB" in cpp_text and "GL_RGBA32F);" not in cpp_text
assert "V318Nis::computeShader()" in cpp_text
assert "inline const std::string& computeShader()" in shader_text
assert "static constexpr std::string_view parts[]" in shader_text
assert 'kComputeShader = R"V318NIS(' not in shader_text
assert "NVScaler(gl_WorkGroupID.xy, gl_LocalInvocationID.x);" in shader_text

print("V3.18 NIS Windows compile fixes applied: OSG completeness, buffered<bool> avoidance, ARB float format, chunked shader literals")

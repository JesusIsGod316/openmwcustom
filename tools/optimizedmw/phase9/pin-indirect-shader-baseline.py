"""Regenerate the reviewable exact raw-template baseline. Never run at runtime.

Regeneration changes the producer contract and requires another source audit.
CRLF and LF are equivalent transport encodings; every other byte is compared.
"""
from pathlib import Path


def cpp_literal(data: bytes) -> str:
    """Keep every template byte while producing whitespace-clean C++ source."""
    escapes = {9: r'\t', 10: r'\n', 13: r'\r', 34: r'\"', 92: r'\\'}
    return '"' + ''.join(escapes.get(byte, chr(byte) if 32 <= byte < 127
                                    else f'\\{byte:03o}') for byte in data) + '"'


root = Path(__file__).resolve().parents[3]
parts = ["// Pinned from promoted source 716b5c4549; changes require shader-contract review.\n",
         "#ifndef OPENMW_SHADER_INDIRECTPROJECTIONBASELINE_H\n#define OPENMW_SHADER_INDIRECTPROJECTIONBASELINE_H\n",
         "#include <string_view>\nnamespace Shader::IndirectProjection {\n",
         "inline bool baselineRaw(std::string_view name, std::string_view source) {\n",
         "std::string_view expected;\n"]
for path in sorted((root / 'files/shaders').rglob('*')):
    if not path.is_file() or path.suffix not in ('.glsl', '.vert', '.frag', '.geom', '.comp', '.tese', '.tesc'):
        continue
    name = path.relative_to(root / 'files/shaders').as_posix()
    text = path.read_text(encoding='utf-8').replace('\r\n', '\n')
    parts.append(f'if (name == "{name}") expected =\n')
    lines = text.encode('utf-8').splitlines(keepends=True)
    parts.extend(cpp_literal(line) + '\n' for line in lines or [b''])
    parts.append(';\n')
parts += ["if (expected.empty()) return false;\nstd::size_t i = 0;\n",
          "for (char c : source) { if (c == '\\r') continue; if (i == expected.size() || expected[i++] != c) return false; }\n",
          "return i == expected.size();\n}\n}\n#endif\n"]
(root / 'components/shader/indirectprojectionbaseline.hpp').write_text(''.join(parts), encoding='utf-8')

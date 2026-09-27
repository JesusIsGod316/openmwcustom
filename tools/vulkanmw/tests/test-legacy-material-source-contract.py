#!/usr/bin/env python3
"""Catch stale literal predicates before building the real VSG shader smoke.

This is an early source-contract check, not a GLSL/pixel test. The Windows
legacy-material smoke still tests uniform packing, descriptors, compiled shader
variants and realization using the actual production shader string.
"""
from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SHADER = ROOT / "components/render/backend/vsg/legacymaterialshader.cpp"
SMOKE = ROOT / "tools/v4/cp3b3/legacy-material-shader-smoke.cpp"


def fragment_literal(cpp: str) -> str:
    match = re.search(
        r'LegacyCompatibilityFragmentShader\s*=\s*R"glsl\((.*?)\)glsl";',
        cpp,
        re.DOTALL,
    )
    if match is None:
        raise ValueError("Production legacy fragment shader literal was not found")
    return match.group(1)


def source_predicates(smoke: str) -> list[str]:
    # These predicates all require presence, not absence. Fail explicitly when
    # the fixture changes shape rather than allowing an empty/vacuous check.
    literals = re.findall(r'source\.find\("((?:[^"\\]|\\.)*)"\)', smoke)
    if not literals:
        raise ValueError("Legacy smoke has no literal source predicates")
    return [json.loads('"' + text + '"') for text in literals]


def missing_predicates(smoke: str, fragment: str) -> list[str]:
    return [text for text in source_predicates(smoke) if text not in fragment]


class LegacyMaterialSourceContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cpp = SHADER.read_text(encoding="utf-8")
        cls.smoke = SMOKE.read_text(encoding="utf-8")
        cls.fragment = fragment_literal(cls.cpp)

    def test_all_actual_smoke_predicates_match_production(self) -> None:
        self.assertEqual(missing_predicates(self.smoke, self.fragment), [])

    def test_every_predicate_detects_its_missing_source(self) -> None:
        for text in set(source_predicates(self.smoke)):
            with self.subTest(predicate=text):
                altered = self.fragment.replace(text, "REMOVED_BY_NEGATIVE_CONTROL")
                self.assertIn(text, missing_predicates(self.smoke, altered))

    def test_old_inlined_checks_reproduce_the_failure(self) -> None:
        old_checks = (
            'source.find("surfaceColor.rgb * effectiveAmbient.rgb * ambient.rgb * scale")',
            'source.find("specularColor * specularStrength * specular.rgb")',
        )
        self.assertEqual(len(missing_predicates("\n".join(old_checks), self.fragment)), 2)

    def test_both_dispatch_paths_pass_complete_material_inputs(self) -> None:
        arguments = "localSurfaceDiffuse, localSurfaceAmbient, localMaterialSpecular, shininess, color);"
        self.assertEqual(self.fragment.count(arguments), 2)
        # The Windows test checks both occurrences, not only the helper body.
        self.assertIn("source.find(localLightArguments, firstLocalLightCall + localLightArguments.size())", self.smoke)

    def test_comment_outside_shader_cannot_satisfy_a_predicate(self) -> None:
        cpp = self.cpp + "\n// EXTERNAL_COMMENT_NOT_SHADER\n"
        smoke = 'source.find("EXTERNAL_COMMENT_NOT_SHADER")'
        self.assertEqual(missing_predicates(smoke, fragment_literal(cpp)), ["EXTERNAL_COMMENT_NOT_SHADER"])

    def test_missing_literal_fails_explicitly(self) -> None:
        with self.assertRaises(ValueError):
            fragment_literal("// no shader literal")

    def test_empty_fixture_fails_explicitly(self) -> None:
        with self.assertRaises(ValueError):
            source_predicates("// no assertions")

    def test_smoke_reports_assertion_exceptions(self) -> None:
        self.assertIn("int main() try", self.smoke)
        self.assertIn("catch (const std::exception& error)", self.smoke)
        self.assertIn('std::cerr << "openmw-vulkan-legacy-material-shader-smoke: " << error.what()', self.smoke)


if __name__ == "__main__":
    unittest.main(verbosity=2)

"""Regression checks for false-positive asset package qualification evidence."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("asset_release", Path(__file__).with_name("verify-asset-shader-release.py"))
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)


class AssetReleaseEvidence(unittest.TestCase):
    def test_actual_contract_population(self):
        text = "ASSET_CONTRACT_PASS stock=1 pbr=1 source_negative=1\n"
        self.assertEqual(gate.record(text, "ASSET_CONTRACT_PASS", {"stock": 1, "pbr": 1})["source_negative"], "1")
        current = "\n".join("ASSET_CONTRACT_REFRESH family=" + family + " cached=1 l1_source=1" for family in ("stock", "pbr"))
        self.assertEqual(set(gate.population(current, "ASSET_CONTRACT_REFRESH", "family", {"stock", "pbr"}, {"cached": 1, "l1_source": 1})), {"stock", "pbr"})
        for bad in ("", current.splitlines()[0], current.replace("family=pbr", "family=stock"), current.replace("l1_source=1", "l1_source=0")):
            with self.subTest(bad=bad), self.assertRaises(gate.QualificationError):
                gate.population(bad, "ASSET_CONTRACT_REFRESH", "family", {"stock", "pbr"}, {"cached": 1, "l1_source": 1})

    def test_zero_engagement_is_not_pass(self):
        with self.assertRaises(gate.QualificationError):
            gate.record("ASSET_MATERIAL_PASS l1=0 h1=1\n", "ASSET_MATERIAL_PASS", {"l1": 1, "h1": 1})

    def test_missing_actual_draw_proof(self):
        with self.assertRaises(gate.QualificationError):
            gate.record("ASSET_MATERIAL_PASS l1=1\n", "ASSET_MATERIAL_PASS", {"l1": 1, "draw_fallback": 1})
        with self.assertRaises(gate.QualificationError):
            gate.record("CHAIN_SHADER_FEATURES normal_map_coverage=0 required_h1_normal_map=1\n",
                        "CHAIN_SHADER_FEATURES", {"normal_map_coverage": 1, "required_h1_normal_map": 1})

    def test_duplicate_terminal_receipts(self):
        with self.assertRaises(gate.QualificationError):
            gate.record("CHAIN_PACKAGE mask=1\nCHAIN_PACKAGE mask=1\n", "CHAIN_PACKAGE", {"mask": 1})

    def test_duplicate_fields(self):
        with self.assertRaises(gate.QualificationError):
            gate.record("CHAIN_PACKAGE mask=0 mask=1\n", "CHAIN_PACKAGE", {"mask": 1})

    def test_skip_cannot_hide_before_positive(self):
        with self.assertRaises(gate.QualificationError):
            gate.record("SKIP: GL unavailable\nASSET_MATERIAL_PASS l1=1\n", "ASSET_MATERIAL_PASS", {"l1": 1})

    def test_mutation_cannot_claim_derived_use(self):
        with self.assertRaises(gate.QualificationError):
            gate.record("CHAIN_SHADER_MUTATION fallback_original=1 derived_draws=1\n", "CHAIN_SHADER_MUTATION", {"fallback_original": 1, "derived_draws": 0})

    def test_changed_input_receipt_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.exe"
            path.write_bytes(b"correct qualified fixture")
            digest = gate.sha(path)
            gate.current_inputs({str(path): digest})
            path.write_bytes(b"changed fixture after validation")
            with self.assertRaises(gate.QualificationError):
                gate.current_inputs({str(path): digest})


if __name__ == "__main__":
    unittest.main()

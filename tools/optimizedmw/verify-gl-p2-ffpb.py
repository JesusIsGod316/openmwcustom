#!/usr/bin/env python3
"""Verify the combined OptimizedMW GL-P2 + full-body first-person source identities."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]

P2 = {
    "apps/openmw/mwrender/objectpaging.cpp": "7df842fddb0b6907aab167b272936ebf4cc71d9f",
    "apps/openmw/mwworld/cellpreloader.cpp": "82c016ebdc14866b7ed81b6f6331ca832a079fc6",
    "apps/openmw/mwworld/cellpreloader.hpp": "d059aa25db95f60eae4b71cb3d6ca3e25866424b",
    "components/sceneutil/pagingwork.hpp": "ad75cc082eb4e463faf5e68b2282fa2dda263aae",
    "components/settings/categories/cells.hpp": "ee2ee5d6b5e7285e07c983bc8b63bd06439ddb63",
    "tools/opimizedmw/gl-p2/readiness-contract.py": "1547148c6bf0a10282f3cab720f6d09288248ee7",
    "tools/opimizedmw/gl-p2/vertex-tests.cpp": "557200c2d2519e1376696afcf4a8f4bb11b85821",
}
FFPB = {
    "apps/openmw/mwrender/animation.cpp": "133217c8d9beb17c79d0607f1e154564998b1921",
    "apps/openmw/mwrender/animation.hpp": "33b37566cdfe1597979b047ea27588d31b7a4212",
    "apps/openmw/mwrender/animblendcontroller.cpp": "1e0b68d6de7225528cb4bc68c5f73adede871461",
    "apps/openmw/mwrender/animblendcontroller.hpp": "3a105b1c3651dc366fed34be4ae27daba7a9175d",
    "apps/openmw/mwrender/hybridanimationtimemap.hpp": "0f3329f091d179f414aa9818495f6d80c8a62223",
    "apps/openmw/mwrender/npcanimation.cpp": "7a2d5b6607b74d02fb33c0bdc8370e9e80e4e77d",
    "apps/openmw/mwrender/npcanimation.hpp": "5db90a29e14e493b07932aa6cb53f6cdedf94b64",
    "apps/openmw_tests/CMakeLists.txt": "405c0073b42913efed761857eeada4a64e5c4c25",
    "apps/openmw_tests/mwrender/testhybridanimationtimemap.cpp": "3647d67b2af2ccb0d710a72bc49646a9763edbed",
    "components/settings/categories/camera.hpp": "3a62d185a2acd1fedb92d9daf6394e01d1b32c42",
}

def blob(path: str) -> str:
    return subprocess.check_output(
        ["git", "rev-parse", f"HEAD:{path}"], cwd=ROOT, text=True
    ).strip()

for group, files in (("P2", P2), ("FFPB", FFPB)):
    for path, expected in files.items():
        actual = blob(path)
        if actual != expected:
            raise SystemExit(f"{group} source drift: {path}: {actual} != {expected}")

settings = (ROOT / "files/settings-default.cfg").read_text(encoding="utf-8")
for key in (
    "full body first person hybrid animations = false",
    "opimizedmw paging optimizer = false",
    "opimizedmw paging readiness split = false",
    "opimizedmw host pressure = false",
    "opimizedmw speculative budget = false",
):
    if settings.count(key) != 1:
        raise SystemExit(f"missing or duplicate combined setting: {key}")

print("OptimizedMW combined GL-P2 + FFPB source identities and settings verified.")

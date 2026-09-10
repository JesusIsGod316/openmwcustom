#!/usr/bin/env python3
"""Fail closed if the consolidated CP4B terrain-streaming contract drifts."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def require(path: str, *markers: str) -> None:
    text = (ROOT / path).read_text(encoding="utf-8")
    missing = [marker for marker in markers if marker not in text]
    if missing:
        raise SystemExit(f"{path}: missing CP4B markers: {missing}")


require(
    "components/rendercore/terrainresidencyplanner.hpp",
    "result.reserve(mDirectionX == 0 && mDirectionY == 0 ? 25 : 30)",
    "inner ? 0u : 1u",
    "stitchMask |= 1u << 0",
    "stitchMask |= 1u << 3",
)
require(
    "components/rendercore/terrainpreparationservice.hpp",
    "maxChunks = 32",
    "maxPreparedBytes = 64u * 1024u * 1024u",
    "stale(work.generation)",
    "terrainMeshPayloadBytes",
)
require(
    "components/rendercore/terrainchunkproducer.hpp",
    "PartiallyApplied",
    "maxNewChunks",
    "targetCompleteAfterBatch",
    "retiredIdentities",
)
require(
    "apps/openmw/mwrender/v4enginerenderbridge.cpp",
    ".maxNewChunks = 4",
    ".maxNewMeshBytes = 8u * 1024u * 1024u",
    "mPendingTerrainPublication",
)
require(
    "apps/openmw/mwrender/v4terrainsource.cpp",
    "indexCache.getIndexBuffer",
    "request.stitchMask",
    "request.lodLevel",
)

print("CP4B terrain streaming contract: PASS")

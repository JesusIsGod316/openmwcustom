import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()

path = ROOT / "apps/openmw/mwrender/renderingmanager.cpp"
text = path.read_text(encoding="utf-8")
old = '''        mObjects->setOcclusionCuller(mOcclusionCuller, occluderMinRadius, occluderMaxRadius, occluderShrinkFactor,
            occluderMeshRes, occluderMaxMeshRes, occluderInsideThreshold, occluderMaxDistance, enableStatics,
            maxTriangles, mOcclusionStorage.get());'''
new = '''        mObjects->setOcclusionCuller(mOcclusionCuller, occluderMinRadius, occluderMaxRadius, occluderShrinkFactor,
            occluderMeshRes, occluderMaxMeshRes, occluderInsideThreshold, occluderMaxDistance, enableStatics,
            Settings::camera().mV34BroadenOcclusion, maxTriangles, mOcclusionStorage.get());'''
count = text.count(old)
if count != 1:
    raise RuntimeError(f"renderingmanager.cpp: expected exactly one V3.4 cache-path call-site match, found {count}")
path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
print("V3.4 compile fix patched setOcclusionCachePath setOcclusionCuller call")

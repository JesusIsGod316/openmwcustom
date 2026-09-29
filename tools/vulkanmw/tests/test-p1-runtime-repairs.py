from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

visitor = (ROOT / "apps/openmw/mwrender/v4animatedobjectcapture.hpp").read_text(encoding="utf-8")
bridge = (ROOT / "apps/openmw/mwrender/v4enginerenderbridge.cpp").read_text(encoding="utf-8")
animation_h = (ROOT / "apps/openmw/mwrender/animation.hpp").read_text(encoding="utf-8")
animation_cpp = (ROOT / "apps/openmw/mwrender/animation.cpp").read_text(encoding="utf-8")
native = (ROOT / "apps/openmw/mwrender/vulkanmw/nativeanimationruntime.cpp").read_text(encoding="utf-8")

required = [
    (visitor, "void apply(osg::LOD& lod) override"),
    (visitor, "lod.getRangeMode() != osg::LOD::DISTANCE_FROM_EYE_POINT"),
    (visitor, "osg::computeLocalToWorld(getNodePath())"),
    (bridge, "source.camera.worldPosition, source.lodScale"),
    (animation_h, "std::string_view groupName;"),
    (animation_cpp, "active->first,"),
    (native, 'group.starts_with("sit")'),
    (native, "seated actor pose retained on evaluated OSG compatibility path"),
]
for text, needle in required:
    if needle not in text:
        raise SystemExit(f"missing P1 runtime repair contract: {needle}")
print("VulkanMW P1 runtime repair source contract: PASS")

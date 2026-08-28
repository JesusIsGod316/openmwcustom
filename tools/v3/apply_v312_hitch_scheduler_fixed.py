from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
base = Path(__file__).with_name("apply_v312_hitch_scheduler.py")
text = base.read_text(encoding="utf-8")

# V3.9 already inserted mV39InitialFrontloadDone between mLastPlayerPos and
# mPagedRefs. Remove the pristine-context edit from the base V3.12 layer and apply
# the state counters against the actual generated V3.11 source after the rest runs.
old_block = '''replace_exact(
    "apps/openmw/mwworld/scene.hpp",
    \'\'\'        osg::Vec3f mLastPlayerPos;

        std::vector<ESM::RefNum> mPagedRefs;\'\'\',
    \'\'\'        osg::Vec3f mLastPlayerPos;

        std::uint64_t mV312EtaTargets = 0;
        std::uint64_t mV312SecondHorizonTargets = 0;

        std::vector<ESM::RefNum> mPagedRefs;\'\'\',
)

'''
if text.count(old_block) != 1:
    raise RuntimeError("Unable to remove pristine V3.12 Scene state edit")
text = text.replace(old_block, "", 1)

exec(compile(text, str(base), "exec"), {"__file__": str(base), "__name__": "__main__"})

scene_h = ROOT / "apps/openmw/mwworld/scene.hpp"
source = scene_h.read_text(encoding="utf-8")
old = '''        osg::Vec3f mLastPlayerPos;

        // V3.9: perform the intentionally expensive multi-view exterior preload
        // only once per Scene lifetime. Subsequent cell-grid changes use normal
        // predictive/background preload and must not repeat startup frontloading.
        bool mV39InitialFrontloadDone = false;

        std::vector<ESM::RefNum> mPagedRefs;'''
new = '''        osg::Vec3f mLastPlayerPos;

        // V3.9: perform the intentionally expensive multi-view exterior preload
        // only once per Scene lifetime. Subsequent cell-grid changes use normal
        // predictive/background preload and must not repeat startup frontloading.
        bool mV39InitialFrontloadDone = false;

        std::uint64_t mV312EtaTargets = 0;
        std::uint64_t mV312SecondHorizonTargets = 0;

        std::vector<ESM::RefNum> mPagedRefs;'''
if source.count(old) != 1:
    raise RuntimeError(f"Generated V3.11 Scene state: expected 1 match, found {source.count(old)}")
scene_h.write_text(source.replace(old, new, 1), encoding="utf-8", newline="\n")
print("V3.12 fixed wrapper patched generated Scene state (1 match)")

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_once(rel, old, new):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"postfx-startup-safety patched {rel}")


# Some post-processing dispatch nodes/passes can exist before their StateSet
# objects are materialized. Diagnostic naming must never dereference those
# optional pointers during renderer startup.
replace_once(
    "apps/openmw/mwrender/pingpongcanvas.cpp",
    '''            const std::string techniqueName = node.mHandle->getName();
            node.mRootStateSet->setName("V3 PostFX " + techniqueName);
            for (std::size_t i = 0; i < node.mPasses.size(); ++i)
                node.mPasses[i].mStateSet->setName("V3 PostFX " + techniqueName + " pass " + std::to_string(i));''',
    '''            const std::string techniqueName = node.mHandle->getName();
            if (node.mRootStateSet)
                node.mRootStateSet->setName("V3 PostFX " + techniqueName);
            for (std::size_t i = 0; i < node.mPasses.size(); ++i)
            {
                if (node.mPasses[i].mStateSet)
                    node.mPasses[i].mStateSet->setName(
                        "V3 PostFX " + techniqueName + " pass " + std::to_string(i));
            }''',
)

print("V3 PostFX startup safety pass completed successfully.")

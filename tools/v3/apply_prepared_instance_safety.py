from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_once(rel, old, new):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"prepared-instance safety patched {rel}")


# Keep OpenMW's existing template/collision preloading ahead of the new
# speculative scene clone. If a preload worker is under pressure, essential
# collision data is therefore ready before it spends time building extra clones.
replace_once(
    "apps/openmw/mwworld/cellpreloader.cpp",
    '''                    mPreloadedObjects.insert(mSceneManager->getTemplate(mesh));
                    mSceneManager->prepareInstance(mesh);
                    if (mPreloadInstances)
                        mPreloadedObjects.insert(mBulletShapeManager->cacheInstance(mesh));
                    else
                        mPreloadedObjects.insert(mBulletShapeManager->getShape(mesh));''',
    '''                    mPreloadedObjects.insert(mSceneManager->getTemplate(mesh));
                    if (mPreloadInstances)
                        mPreloadedObjects.insert(mBulletShapeManager->cacheInstance(mesh));
                    else
                        mPreloadedObjects.insert(mBulletShapeManager->getShape(mesh));
                    if (!mAbort)
                        mSceneManager->prepareInstance(mesh);''',
)

print("V3 Prepared Static Instance safety pass completed successfully.")

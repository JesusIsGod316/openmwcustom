from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected exactly one guarded match, found {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


# Frame-local effects still use the same winning-VFS/content-identity contract
# as persistent resources. Never substitute an image filename for byte identity.
replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    "#include <components/rendercore/effectframe.hpp>\n",
    "#include <components/rendercore/effectframe.hpp>\n#include <components/nifrender/vfsidentity.hpp>\n",
)
replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    """        [[nodiscard]] inline bool captureMaterial(const osg::NodePath& path, const osg::StateSet* drawableState,\n            CapturedMaterial& out, std::string& diagnostic)\n""",
    """        [[nodiscard]] inline bool captureMaterial(const osg::NodePath& path, const osg::StateSet* drawableState,\n            const VFS::Manager& vfs, CapturedMaterial& out, std::string& diagnostic)\n""",
)
replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    """                EffectTextureSnapshot snapshot;\n                snapshot.texture.revision = InitialResourceRevision;\n                snapshot.texture.sourceIdentity = image->getFileName();\n                snapshot.texture.contentIdentity = image->getFileName();\n""",
    """                const NifRender::ResolvedVfsIdentity resolved\n                    = NifRender::resolveTextureVfsIdentity(VFS::Path::NormalizedView(image->getFileName()), vfs);\n                if (!resolved.valid())\n                {\n                    diagnostic = \"evaluated effect texture could not resolve its winning VFS content identity\";\n                    return false;\n                }\n\n                EffectTextureSnapshot snapshot;\n                snapshot.texture.revision = InitialResourceRevision;\n                snapshot.texture.sourceIdentity = std::string(resolved.canonicalPath.value());\n                snapshot.texture.contentIdentity = resolved.contentIdentity;\n""",
)
replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    """        [[nodiscard]] inline bool captureGeometry(const osg::Geometry& geometry, const osg::NodePath& path,\n            std::string identity, RenderCore::ImmediateEffectDraw& draw, std::string& diagnostic)\n""",
    """        [[nodiscard]] inline bool captureGeometry(const osg::Geometry& geometry, const osg::NodePath& path,\n            const VFS::Manager& vfs, std::string identity, RenderCore::ImmediateEffectDraw& draw,\n            std::string& diagnostic)\n""",
)
replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    """            CapturedMaterial captured;\n            if (!captureMaterial(path, geometry.getStateSet(), captured, diagnostic))\n""",
    """            CapturedMaterial captured;\n            if (!captureMaterial(path, geometry.getStateSet(), vfs, captured, diagnostic))\n""",
)
replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    """        [[nodiscard]] inline bool captureParticleSystem(const osgParticle::ParticleSystem& particles,\n            const osg::NodePath& path, std::string_view identityPrefix,\n            std::vector<RenderCore::ImmediateEffectDraw>& draws, std::string& diagnostic)\n""",
    """        [[nodiscard]] inline bool captureParticleSystem(const osgParticle::ParticleSystem& particles,\n            const osg::NodePath& path, const VFS::Manager& vfs, std::string_view identityPrefix,\n            std::vector<RenderCore::ImmediateEffectDraw>& draws, std::string& diagnostic)\n""",
)
replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    """            CapturedMaterial captured;\n            if (!captureMaterial(path, particles.getStateSet(), captured, diagnostic))\n""",
    """            CapturedMaterial captured;\n            if (!captureMaterial(path, particles.getStateSet(), vfs, captured, diagnostic))\n""",
)
replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    """            CaptureVisitor(std::string identityPrefix, bool wholeSubtree)\n                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)\n                , mIdentityPrefix(std::move(identityPrefix))\n                , mWholeSubtree(wholeSubtree)\n""",
    """            CaptureVisitor(std::string identityPrefix, bool wholeSubtree, const VFS::Manager& vfs)\n                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)\n                , mIdentityPrefix(std::move(identityPrefix))\n                , mWholeSubtree(wholeSubtree)\n                , mVfs(vfs)\n""",
)
replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    """                        if (!captureParticleSystem(*particles, getNodePath(), nextIdentity(\"system\"), mResult.draws,\n                                mResult.diagnostic))\n""",
    """                        if (!captureParticleSystem(*particles, getNodePath(), mVfs, nextIdentity(\"system\"),\n                                mResult.draws, mResult.diagnostic))\n""",
)
replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    """                            if (!captureGeometry(*geometry, getNodePath(), nextIdentity(\"geometry\"), draw,\n                                    mResult.diagnostic))\n""",
    """                            if (!captureGeometry(*geometry, getNodePath(), mVfs, nextIdentity(\"geometry\"), draw,\n                                    mResult.diagnostic))\n""",
)
replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    """                            if (!captureParticleSystem(*particles, getNodePath(), nextIdentity(\"system\"),\n                                    mResult.draws, mResult.diagnostic))\n""",
    """                            if (!captureParticleSystem(*particles, getNodePath(), mVfs, nextIdentity(\"system\"),\n                                    mResult.draws, mResult.diagnostic))\n""",
)
replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    """            std::string mIdentityPrefix;\n            bool mWholeSubtree = false;\n""",
    """            std::string mIdentityPrefix;\n            bool mWholeSubtree = false;\n            const VFS::Manager& mVfs;\n""",
)
replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    """    [[nodiscard]] inline V4EffectCaptureResult captureV4AttachedEffects(\n        osg::Node& animationRoot, std::string identityPrefix)\n    {\n        v4_effect_detail::CaptureVisitor visitor(std::move(identityPrefix), false);\n""",
    """    [[nodiscard]] inline V4EffectCaptureResult captureV4AttachedEffects(\n        osg::Node& animationRoot, std::string identityPrefix, const VFS::Manager& vfs)\n    {\n        v4_effect_detail::CaptureVisitor visitor(std::move(identityPrefix), false, vfs);\n""",
)
replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    """    [[nodiscard]] inline V4EffectCaptureResult captureV4WholeEffectSubtree(\n        osg::Node& root, std::string identityPrefix)\n    {\n        v4_effect_detail::CaptureVisitor visitor(std::move(identityPrefix), true);\n""",
    """    [[nodiscard]] inline V4EffectCaptureResult captureV4WholeEffectSubtree(\n        osg::Node& root, std::string identityPrefix, const VFS::Manager& vfs)\n    {\n        v4_effect_detail::CaptureVisitor visitor(std::move(identityPrefix), true, vfs);\n""",
)

replace_once(
    "apps/openmw/mwrender/v4engineframecoordinator.cpp",
    """            V4EffectCaptureResult captured = captureV4WholeEffectSubtree(\n                *bolt.effectRoot, \"magic-projectile:\" + std::to_string(bolt.runtimeId));\n""",
    """            V4EffectCaptureResult captured = captureV4WholeEffectSubtree(\n                *bolt.effectRoot, \"magic-projectile:\" + std::to_string(bolt.runtimeId), mVfs);\n""",
)
replace_once(
    "apps/openmw/mwrender/v4enginerenderbridge.cpp",
    """                V4EffectCaptureResult captured\n                    = captureV4AttachedEffects(*effectRoot, \"actor-effect:\" + *identity);\n""",
    """                V4EffectCaptureResult captured\n                    = captureV4AttachedEffects(*effectRoot, \"actor-effect:\" + *identity, mVfs);\n""",
)

capture = Path("apps/openmw/mwrender/v4effectcapture.hpp").read_text(encoding="utf-8")
for needle in ["resolveTextureVfsIdentity", "resolved.contentIdentity", "const VFS::Manager& mVfs"]:
    if needle not in capture:
        raise RuntimeError(f"evaluated effect VFS identity stage missing {needle!r}")
if "snapshot.texture.contentIdentity = image->getFileName()" in capture:
    raise RuntimeError("evaluated effect still substitutes source path for content identity")

print("CP4F evaluated-effect texture identity canonicalization applied")

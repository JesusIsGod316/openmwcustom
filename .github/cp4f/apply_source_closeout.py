from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected exactly one guarded match, found {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


# CP4F local-map cleanup: native VSG/MyGUI surfaces are not OSG textures.
# Track their binding state explicitly instead of manufacturing an empty
# MyGUIPlatform::OSGTexture as a sentinel.
replace_once(
    "apps/openmw/mwgui/mapwindow.hpp",
    """            std::unique_ptr<MyGUI::ITexture> mMapTexture;\n            std::unique_ptr<MyGUI::ITexture> mFogTexture;\n            int mCellX;\n""",
    """            std::unique_ptr<MyGUI::ITexture> mMapTexture;\n            std::unique_ptr<MyGUI::ITexture> mFogTexture;\n            bool mNativeMapBound = false;\n            bool mNativeFogBound = false;\n            int mCellX;\n""",
)

replace_once(
    "apps/openmw/mwgui/mapwindow.cpp",
    """                entry.mFogWidget->setImageTexture({});\n                entry.mFogTexture.reset();\n""",
    """                entry.mFogWidget->setImageTexture({});\n                entry.mFogTexture.reset();\n                entry.mNativeFogBound = false;\n""",
)

replace_once(
    "apps/openmw/mwgui/mapwindow.cpp",
    """            entry.mMapTexture.reset();\n            entry.mFogTexture.reset();\n        };\n""",
    """            entry.mMapTexture.reset();\n            entry.mFogTexture.reset();\n            entry.mNativeMapBound = false;\n            entry.mNativeFogBound = false;\n        };\n""",
)

replace_once(
    "apps/openmw/mwgui/mapwindow.cpp",
    """                if (!entry.mMapTexture)\n                {\n                    std::string_view textureName\n""",
    """                if (!entry.mNativeMapBound)\n                {\n                    std::string_view textureName\n""",
)

replace_once(
    "apps/openmw/mwgui/mapwindow.cpp",
    """                        // Bind by MyGUI name. The VSG renderer owns this name and\n                        // its ImageView; the empty OSGTexture is only a local UI\n                        // readiness sentinel and is never submitted to MyGUI.\n                        entry.mMapWidget->setImageTexture(textureName);\n                        entry.mMapTexture\n                            = std::make_unique<MyGUIPlatform::OSGTexture>(std::string(), nullptr);\n""",
    """                        // Bind by MyGUI name. The VSG renderer owns this name and\n                        // its ImageView; no foreign OSG texture object participates\n                        // in the native auxiliary-surface path.\n                        entry.mMapWidget->setImageTexture(textureName);\n                        entry.mNativeMapBound = true;\n""",
)

replace_once(
    "apps/openmw/mwgui/mapwindow.cpp",
    """                if (!entry.mFogTexture && mFogOfWarToggled && mFogOfWarEnabled)\n""",
    """                if (!entry.mNativeFogBound && mFogOfWarToggled && mFogOfWarEnabled)\n""",
)

replace_once(
    "apps/openmw/mwgui/mapwindow.cpp",
    """                        entry.mFogWidget->setImageTexture(fogName);\n                        entry.mFogTexture\n                            = std::make_unique<MyGUIPlatform::OSGTexture>(std::string(), nullptr);\n""",
    """                        entry.mFogWidget->setImageTexture(fogName);\n                        entry.mNativeFogBound = true;\n""",
)

cpp = Path("apps/openmw/mwgui/mapwindow.cpp").read_text(encoding="utf-8")
header = Path("apps/openmw/mwgui/mapwindow.hpp").read_text(encoding="utf-8")
if "mNativeMapBound" not in header or "mNativeFogBound" not in header:
    raise RuntimeError("native map binding state was not added")
if "empty OSGTexture is only a local UI" in cpp:
    raise RuntimeError("obsolete native OSGTexture sentinel path remains")
if "if (!entry.mNativeMapBound)" not in cpp or "if (!entry.mNativeFogBound" not in cpp:
    raise RuntimeError("native map/fog binding guards are incomplete")

print("CP4F guarded source closeout patch applied")

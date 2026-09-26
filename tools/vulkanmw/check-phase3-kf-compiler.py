#!/usr/bin/env python3
"""VulkanMW Phase 3B external KF source contract."""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADER = ROOT / "components/render/native/nifkeyframeclip.hpp"
SOURCE = ROOT / "components/render/native/nifkeyframeclip.cpp"
CONTROLLER_HEADER = ROOT / "components/render/native/nifcontrollerprogram.hpp"
CONTROLLER_SOURCE = ROOT / "components/render/native/nifcontrollerprogram.cpp"
RUNTIME = ROOT / "components/render/backend/vsg/runtime-sources.cmake"
KEYFRAME_MANAGER = ROOT / "components/resource/keyframemanager.hpp"

FORBIDDEN = (
    re.compile(r"#\s*include\s*[<\"](?:osg|osgAnimation|osgParticle|osgUtil|osgViewer)/"),
    re.compile(r"\bosg::"),
    re.compile(r"\bSceneUtil::"),
    re.compile(r"\bNifOsg::"),
)


def fail(message: str) -> None:
    print(f"VulkanMW Phase 3B KF contract FAILED: {message}", file=sys.stderr)
    raise SystemExit(1)


def code_only(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return "\n".join(line.split("//", 1)[0] for line in text.splitlines())


def require(path: pathlib.Path, needles: tuple[str, ...]) -> str:
    if not path.is_file():
        fail(f"missing {path.relative_to(ROOT)}")
    text = path.read_text(encoding="utf-8", errors="replace")
    for needle in needles:
        if needle not in text:
            fail(f"{path.relative_to(ROOT)} lost required contract: {needle}")
    return text


def main() -> int:
    for path in (HEADER, SOURCE):
        code = code_only(require(path, ()))
        for pattern in FORBIDDEN:
            if pattern.search(code):
                fail(f"{path.relative_to(ROOT)} imports a legacy scene/controller runtime")

    require(HEADER, (
        "class NativeTextKeyMap",
        "struct NifKeyframeClip",
        "std::map<std::string, NamedTransformTrack",
        "class NifKeyframeClipCompiler",
        "const ToUTF8::StatelessUtf8Encoder* encoder",
        "const ToUTF8::StatelessUtf8Encoder* mEncoder",
        "NifKeyframeCompileResult compile(VFS::Path::NormalizedView path) const;",
    ))

    source = require(SOURCE, (
        "RC_NiSequenceStreamHelper",
        "RC_NiTextKeyExtraData",
        "Misc::StringUtils::split",
        "Misc::StringUtils::trim",
        "Misc::StringUtils::lowerCaseInPlace",
        "RC_NiStringExtraData",
        "RC_NiKeyframeController",
        "NifControllerCompiler::compileTransformTrack",
        "NifControllerCompiler::compileTiming",
        "track.supported()",
        "track.hasKeys()",
        "++result.clip.emptyControllers",
        "Nif::Reader reader(file, mEncoder);",
        "result.clip.controllers.emplace",
    ))

    if ".isActive()" in source or "->isActive()" in source:
        fail("external KF compiler must preserve vanilla/OpenMW behavior that ignores controller active flags")

    require(CONTROLLER_HEADER, (
        "enum class TransformTrackCompileStatus",
        "struct TransformTrackCompileResult",
        "UnsupportedInterpolator",
        "compileTiming(const Nif::NiTimeController& source)",
        "TransformTrackCompileResult compileTransformTrack(",
    ))
    require(CONTROLLER_SOURCE, (
        "TransformTrackCompileStatus::Empty",
        "TransformTrackCompileStatus::UnsupportedInterpolator",
        "NifControllerCompiler::compileTiming",
        "NifControllerCompiler::compileTransformTrack",
    ))
    require(KEYFRAME_MANAGER, (
        "const ToUTF8::StatelessUtf8Encoder* getEncoder() const noexcept",
    ))
    require(RUNTIME, ("components/render/native/nifkeyframeclip.cpp",))

    print("VulkanMW Phase 3B external KF compiler contract PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

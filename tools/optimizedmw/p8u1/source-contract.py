#!/usr/bin/env python3
"""Protect P8U1 integration and packaging; not a runtime performance test."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[3]
checks = 0

def require(value, message):
    global checks
    if not value:
        raise AssertionError(message)
    checks += 1

def text(path):
    return (root / path).read_text(encoding="utf-8")

cfg = text("files/settings-default.cfg")
for setting in ("optimizedmw canonical terrain textures", "optimizedmw composite slicing",
                "optimizedmw object userdata cache", "warm sounds", "optimizedmw setting consistency"):
    require(re.search(r"^" + re.escape(setting) + r" = false$", cfg, re.M), setting + " must default off")
engine = text("apps/openmw/engine.cpp")
require("_endDynamicDrawBlock->block();" in engine, "dynamic safe-point removed")
require(engine.count("mSoundManager->warmStoreSounds();") == 1, "warming discovery must run once at loading")
prepare = engine[engine.index("void OMW::Engine::prepareEngine()") :]
require("mSoundManager->warmStoreSounds();" in prepare, "warming outside loading setup")
sound = text("apps/openmw/mwsound/soundmanagerimp.cpp")
require("#include <components/esm3/loadsoun.hpp>" in sound,
        "warming reads ESM::Sound fields and must include their complete declaration directly")
update = sound[sound.index("void SoundManager::update(float duration)"):]
require("warmStoreSounds(" not in update and "warmCellSounds(" not in update,
        "ordinary update must not discover the world for warming")
require("mWarmQueue.reset(); // join before" in sound, "worker lifetime gate missing")
require("pcmAlreadyCoversEffects" in sound and "offered > 4096" in sound, "audio bounds/PCM separation missing")
texture = text("components/resource/preparedterraintexture.hpp")
for token in ("previous->context.get() == state.getGraphicsContext()", "getModifiedCount()",
              "isDirty(context)", "getForceTextureDownloadGeometry()", "getSubloadCallback()"):
    require(token in texture, "missing texture identity/side-effect condition: " + token)
require("void apply(" not in texture, "draw-time apply must remain stock")
composite = text("components/terrain/compositemaprenderer.cpp")
require("compileUntil" in composite and "mRequired.load" in composite, "required composite progress missing")
lua = text("apps/openmw/mwlua/luamanagerimp.cpp")
require("initializeObjectCaches(mLua.unsafeState(), Settings::lua().mOptimizedMWObjectUserdataCache)" in lua,
        "per-state startup cache initialization missing")
require(lua.index("clearObjectCaches(mLua.unsafeState());") < lua.index("for (int i = 0; i < 5; ++i)"),
        "cache teardown must precede collection")
for path in (root / ".github/workflows").glob("*"):
    require("p8g5" not in path.name.lower(), "unfinished DLSS-only build added")
require(not list((root / "tools/optimizedmw/p8u1").glob("*.bat")), "extra public test BAT added")
print(f"PASS: {checks} source/integration guard checks (not gameplay or full-engine compilation)")

launcher = text("tools/optimizedmw/p8u1/OptimizedMW_Test.ps1")
for name in ("REFERENCE", "COMBINED", "RESOURCE-REPAIR", "LUA-CACHE", "SOUND-WARM", "SHADOW-SETTINGS"):
    require(name in launcher, "missing mode " + name)
require("$CullInputs='true'" in launcher and "$Lod2='false'" in launcher and "$ShadowBatch='false'" in launcher,
    "conservative P8G4 CULL-CPU foundation changed")
require("OPENMW_P8G4_CLEAN_CAPTURE" in launcher and "settings_restore_verified" in launcher, "capture/restore missing")
require("dlss_runtime_available=false" in launcher, "unfinished DLSS misrepresented")
cmake = text("tools/optimizedmw/p8u1/CMakeLists.txt")
require("P8U1_REQUIRE_GAME_SDK" in cmake and "p8u1-sol-object" in cmake and "p8u1-ffmpeg" in cmake,
    "real SDK integration gate missing")
shadow = text("components/sceneutil/mwshadowtechnique.cpp")
require("replaceShadowUniform(_uniforms" in shadow and "shadowTextureExtent" in shadow, "shadow consistency lost")
print("PASS: P8U1 source, six isolated modes and real-SDK gate; publication now authorized")

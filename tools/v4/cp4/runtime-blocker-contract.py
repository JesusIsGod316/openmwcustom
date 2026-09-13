#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[3]
actor = (root / 'components/nifrender/actormodelcomposer.hpp').read_text()
state_hpp = (root / 'components/lua/luastate.hpp').read_text()
state_cpp = (root / 'components/lua/luastate.cpp').read_text()
container = (root / 'components/lua/scriptscontainer.cpp').read_text()
engine = (root / 'apps/openmw/engine.cpp').read_text()
bridge = (root / 'apps/openmw/mwrender/v4enginerenderbridge.cpp').read_text()

required = {
    'forced actor skeleton helper': 'buildForcedActorSkeleton' in actor,
    'first-match duplicate actor bones': 'if (names.find(folded) != names.end())\n                continue;' in actor,
    'direct sandbox ScriptId parameter': 'ScriptId scriptId = {}' in state_hpp,
    'direct container ScriptId handoff': 'ScriptId{ this, scriptId }' in container,
    'runtime forced skeleton publication': 'runtime:forced-actor-skeleton:' in bridge,
}
for label, ok in required.items():
    if not ok:
        raise SystemExit(f'FAIL: missing {label}')

forbidden = {
    'V4-only duplicate-bone rejection': 'ambiguous case-insensitive bone name' in actor,
    'unsafe ScriptId Lua userdata recovery': '.get<sol::optional<ScriptId>>(ScriptsContainer::sScriptIdKey)' in state_cpp,
    'speculative new-game player-cell fallback': 'New-game transitions can establish the authoritative player cell' in engine,
    'old actor canonical-skeleton hard reject': 'active actor source model has no publishable canonical skeleton' in bridge,
}
for label, present in forbidden.items():
    if present:
        raise SystemExit(f'FAIL: {label} remains')

print('CP4F runtime blocker contract: PASS')

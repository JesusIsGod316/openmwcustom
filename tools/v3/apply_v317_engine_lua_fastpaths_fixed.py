from pathlib import Path


# The substantive V3.17 implementation was authored against upstream-style
# engineevents.cpp anchors. The mature V3 stack has already inserted V3.3 event
# attribution includes and an anonymous event-name helper. Rewrite only those two
# anchoring blocks, then execute the exact substantive patch. This preserves all
# earlier V3.3 instrumentation instead of weakening the generated stack.
base = Path(__file__).with_name("apply_v317_engine_lua_fastpaths.py")
text = base.read_text(encoding="utf-8")

old_include = '''replace_exact(
    "apps/openmw/mwlua/engineevents.cpp",
    \'\'\'#include "engineevents.hpp"\\n\\n#include <components/debug/debuglog.hpp>\'\'\',
    \'\'\'#include "engineevents.hpp"\\n\\n#include <cstdlib>\\n\\n#include <components/debug/debuglog.hpp>\'\'\',
)
'''
new_include = '''replace_exact(
    "apps/openmw/mwlua/engineevents.cpp",
    \'\'\'#include <set>\\n#include <type_traits>\'\'\',
    \'\'\'#include <cstdlib>\\n#include <set>\\n#include <type_traits>\'\'\',
)
'''
if text.count(old_include) != 1:
    raise RuntimeError(f"V3.17 engineevents compat: expected 1 include-anchor block, found {text.count(old_include)}")
text = text.replace(old_include, new_include, 1)

old_namespace = '''replace_exact(
    "apps/openmw/mwlua/engineevents.cpp",
    \'\'\'namespace MWLua\\n{{\\n\\n    class EngineEvents::Visitor\'\'\',
    \'\'\'namespace MWLua\\n{{\\n    namespace\\n    {{\\n        bool v317EngineFastPathEnabled()\\n        {{\\n            // Launcher selection happens before process start, so this is immutable\\n            // for the lifetime of the process and cheap after the first query.\\n            static const bool enabled = std::getenv("OPENMW_V317_LUA_OPT") != nullptr;\\n            return enabled;\\n        }}\\n    }}\\n\\n    class EngineEvents::Visitor\'\'\',
)
'''.replace('{{', '{').replace('}}', '}')
new_namespace = '''replace_exact(
    "apps/openmw/mwlua/engineevents.cpp",
    \'\'\'    }}\\n\\n    class EngineEvents::Visitor\'\'\',
    \'\'\'        bool v317EngineFastPathEnabled()\\n        {{\\n            // Launcher selection happens before process start, so this is immutable\\n            // for the lifetime of the process and cheap after the first query.\\n            static const bool enabled = std::getenv("OPENMW_V317_LUA_OPT") != nullptr;\\n            return enabled;\\n        }}\\n    }}\\n\\n    class EngineEvents::Visitor\'\'\',
)
'''.replace('{{', '{').replace('}}', '}')
if text.count(old_namespace) != 1:
    raise RuntimeError(
        f"V3.17 engineevents compat: expected 1 namespace-anchor block, found {text.count(old_namespace)}"
    )
text = text.replace(old_namespace, new_namespace, 1)

exec(compile(text, str(base), "exec"), {"__file__": str(base), "__name__": "__main__"})

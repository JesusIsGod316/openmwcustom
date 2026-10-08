"""Exact serialized setting contracts across curated legacy-name migration.

Only keys are resolved through the engine/launcher alias authority. Values,
categories, source behavior checks, and payload hashes are not substituted.
"""
import json
import re


class SettingsContract:
    def __init__(self, root):
        document = json.loads((root / "files/optimization-settings-aliases.json").read_text(encoding="utf-8"))
        if document["schema_version"] != 1:
            raise ValueError("unsupported settings alias schema")
        self.aliases = {}
        for row in document["aliases"]:
            old = (row["old_section"], row["old_key"])
            if old in self.aliases:
                raise ValueError("duplicate settings alias: " + repr(old))
            self.aliases[old] = (row["new_section"], row["new_key"])
        raw = {}
        section = ""
        for line in (root / "files/settings-default.cfg").read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("["):
                end = line.find("]")
                if end < 0:
                    raise ValueError("unterminated default settings section")
                section, line = line[1:end].strip(), line[end + 1:].strip()
                if not line:
                    continue
            if not section or "=" not in line:
                raise ValueError("invalid default settings line: " + line)
            name, value = (part.strip() for part in line.split("=", 1))
            key = (section, name)
            if key in raw:
                raise ValueError("duplicate default setting: " + repr(key))
            raw[key] = value
        self.defaults = {}
        for key, value in raw.items():
            canonical = self.canonical(*key)
            # Match the engine's same-file precedence: canonical wins in both
            # orders; a legacy spelling still works when it is the sole value.
            if key == canonical or canonical not in raw:
                self.defaults.setdefault(canonical, value)

    def canonical(self, section, key):
        return self.aliases.get((section, key), (section, key))

    def default(self, section, key, expected):
        return self.defaults.get(self.canonical(section, key)) == expected

    def declared(self, source, section, key):
        section, key = self.canonical(section, key)
        # Require the exact serialized section/key pair in a SettingValue
        # initializer, never a comment or a similarly named member alone.
        # Preserve string/character literals while removing C++ comments.
        tokens = r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/'
        source = re.sub(tokens, lambda match: "" if match[0].startswith(("//", "/*")) else match[0], source)
        pattern = (r'\bSettingValue\s*<[^;{}]*>\s+\w+\s*\{\s*mIndex\s*,\s*"'
                   + re.escape(section) + r'"\s*,\s*"' + re.escape(key) + r'"')
        return re.search(pattern, source) is not None

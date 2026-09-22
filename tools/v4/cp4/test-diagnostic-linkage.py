#!/usr/bin/env python3
"""Cheap CMake dependency contract; not a full engine or Windows compilation."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def link_calls(path, target):
    text = re.sub(r'#[^\n]*', '', path.read_text(encoding='utf-8'))
    return re.findall(r'target_link_libraries\s*\(\s*' + re.escape(target) + r'\s+([^)]*)\)', text,
                      flags=re.IGNORECASE)


def main():
    app = ROOT / 'apps/openmw/CMakeLists.txt'
    component = ROOT / 'components/CMakeLists.txt'
    for path, target in ((app, 'openmw-lib'), (component, 'components')):
        calls = link_calls(path, target)
        assert calls, f'{target}: no dependency declarations found'
        styles = {'keyword' if body.split()[0] in ('PUBLIC', 'PRIVATE', 'INTERFACE') else 'plain'
                  for body in calls}
        assert len(styles) == 1, f'{target}: mixed target_link_libraries signatures: {styles}'
    matches = [body for body in link_calls(component, 'components') if 'psapi' in body.split()]
    assert len(matches) == 1, 'Shared recorder PSAPI dependency must be owned by components'
    assert not any('psapi' in body.split() for body in link_calls(app, 'openmw-lib')), 'Do not restrict PSAPI to game target'
    owning_call = 'target_link_libraries(components ' + matches[0] + ')\n'
    with tempfile.TemporaryDirectory(prefix='openmw-diagnostic-linkage-') as temp:
        root = Path(temp)
        (root / 'stub.cpp').write_text('int diagnostic_linkage_fixture() { return 0; }\n')
        preamble = '''cmake_minimum_required(VERSION 3.16)
project(DiagnosticLinkContract LANGUAGES CXX)
add_library(components STATIC stub.cpp)
add_library(openmw-lib STATIC stub.cpp)
target_link_libraries(components shlwapi)
target_link_libraries(openmw-lib components)
'''
        for vulkan in (False, True):
            source = preamble + owning_call
            if vulkan:
                source += 'add_library(v4-marker INTERFACE)\ntarget_link_libraries(openmw-lib v4-marker)\n'
            source += '''get_target_property(deps components INTERFACE_LINK_LIBRARIES)
if(NOT "psapi" IN_LIST deps)
  message(FATAL_ERROR "PSAPI missing from shared consumer link contract")
endif()
'''
            (root / 'CMakeLists.txt').write_text(source)
            command = ['cmake', '-S', str(root), '-B', str(root / ('vulkan' if vulkan else 'opengl'))]
            result = subprocess.run(command, text=True, capture_output=True)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            print('PASS host-CMake Windows dependency contract:', 'Vulkan' if vulkan else 'OpenGL')
        (root / 'CMakeLists.txt').write_text(preamble + 'target_link_libraries(openmw-lib PRIVATE psapi)\n')
        result = subprocess.run(['cmake', '-S', str(root), '-B', str(root / 'rejected-control')],
                                text=True, capture_output=True)
        assert result.returncode != 0 and 'plain signature' in result.stderr, 'Old mixed-signature regression was not rejected'
        print('PASS exact prior mixed-signature control rejected')
    print('Diagnostic dependency contract: PASS (full Windows production build still required)')


if __name__ == '__main__':
    main()

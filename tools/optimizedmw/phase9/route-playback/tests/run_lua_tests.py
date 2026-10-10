"""Execute playback fixtures against the installed engine's exact Lua DLL."""
import argparse
import ctypes
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--dll', type=Path, default=Path('C:/OptimizedMW_Test/lua51.dll'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    lib = ctypes.CDLL(str(args.dll.resolve()))
    lib.luaL_newstate.restype = ctypes.c_void_p
    lib.luaL_openlibs.argtypes = [ctypes.c_void_p]
    lib.luaL_loadbuffer.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p]
    lib.luaL_loadbuffer.restype = ctypes.c_int
    lib.lua_pcall.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
    lib.lua_pcall.restype = ctypes.c_int
    lib.lua_tolstring.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_size_t)]
    lib.lua_tolstring.restype = ctypes.c_char_p
    lib.lua_close.argtypes = [ctypes.c_void_p]
    source = ('arg={[1]=[=[' + root.as_posix() + ']=]}; dofile([=['
              + (root/'tests/playback_tests.lua').as_posix() + ']=])').encode()
    state = lib.luaL_newstate()
    if not state:
        raise RuntimeError('luaL_newstate failed')
    try:
        lib.luaL_openlibs(state)
        code = lib.luaL_loadbuffer(state, source, len(source), b'@playback-fixtures')
        if not code:
            code = lib.lua_pcall(state, 0, 0, 0)
        if code:
            error = lib.lua_tolstring(state, -1, None)
            raise RuntimeError(error.decode(errors='replace') if error else f'Lua error {code}')
    finally:
        lib.lua_close(state)


if __name__ == '__main__':
    main()

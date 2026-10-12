"""Owned private-window lifetime fixture. Never switches or activates a desktop."""
import ctypes as C
from ctypes import wintypes as W
import json
import time

import benchmark_desktop

u, k = benchmark_desktop.bindings()
for dll, name, result, args in [
    (u, 'CreateWindowExW', W.HWND, [W.DWORD, W.LPCWSTR, W.LPCWSTR, W.DWORD,
        C.c_int, C.c_int, C.c_int, C.c_int, W.HWND, W.HANDLE, W.HINSTANCE, W.LPVOID]),
    (u, 'ShowWindow', W.BOOL, [W.HWND, C.c_int]),
    (u, 'DestroyWindow', W.BOOL, [W.HWND]),
    (u, 'PeekMessageW', W.BOOL, [W.LPVOID, W.HWND, W.UINT, W.UINT, W.UINT]),
    (u, 'TranslateMessage', W.BOOL, [W.LPVOID]),
    (u, 'DispatchMessageW', C.c_ssize_t, [W.LPVOID]),
    (k, 'GetModuleHandleW', W.HMODULE, [W.LPCWSTR]),
    (k, 'GetCurrentThreadId', W.DWORD, []),
    (k, 'GetConsoleWindow', W.HWND, []),
]:
    function = getattr(dll, name)
    function.restype, function.argtypes = result, args
desktop = benchmark_desktop.desktop_name(u, u.GetThreadDesktop(k.GetCurrentThreadId()))
if not desktop.startswith('OptimizedMW_Benchmark_') or k.GetConsoleWindow():
    raise RuntimeError('Window fixture requires an owned private desktop without a console')
window = benchmark_desktop.checked(u.CreateWindowExW(0, 'STATIC', 'Private lifetime fixture',
    0x00cf0000, 0, 0, 320, 200, None, None, k.GetModuleHandleW(None), None), 'fixture window')
message = C.create_string_buffer(64)


def pump(seconds):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        while u.PeekMessageW(message, None, 0, 0, 1):
            u.TranslateMessage(message)
            u.DispatchMessageW(message)
        time.sleep(.02)


try:
    u.ShowWindow(window, 1)
    pump(2.5)
    u.ShowWindow(window, 0)
    pump(2.5)
    print(json.dumps({'private_desktop': desktop, 'console_window': 0,
        'visible_then_hidden_before_exit': True}))
finally:
    u.DestroyWindow(window)

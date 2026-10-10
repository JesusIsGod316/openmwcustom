"""Launch an owned benchmark tree on an unswitched Windows desktop.

The process keeps a normal, non-minimized rendering window on a private desktop.
No input injection, foreground activation, desktop switch, or user settings edit.
All descendants are bounded by a kill-on-close job, including crash dialogs.
"""

import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import uuid


class SecurityAttributes(C.Structure):
    _fields_ = [('length', W.DWORD), ('descriptor', W.LPVOID), ('inherit', W.BOOL)]


class StartupInfo(C.Structure):
    _fields_ = [('cb', W.DWORD), ('reserved', W.LPWSTR), ('desktop', W.LPWSTR),
                ('title', W.LPWSTR), ('x', W.DWORD), ('y', W.DWORD),
                ('xsize', W.DWORD), ('ysize', W.DWORD), ('xchars', W.DWORD),
                ('ychars', W.DWORD), ('fill', W.DWORD), ('flags', W.DWORD),
                ('show', W.WORD), ('reserved_size', W.WORD), ('reserved2', W.LPVOID),
                ('stdin', W.HANDLE), ('stdout', W.HANDLE), ('stderr', W.HANDLE)]


class ProcessInfo(C.Structure):
    _fields_ = [('process', W.HANDLE), ('thread', W.HANDLE), ('pid', W.DWORD), ('tid', W.DWORD)]


class BasicJobLimits(C.Structure):
    _fields_ = [('process_time', C.c_longlong), ('job_time', C.c_longlong),
                ('flags', W.DWORD), ('min_working_set', C.c_size_t),
                ('max_working_set', C.c_size_t), ('process_limit', W.DWORD),
                ('affinity', C.c_size_t), ('priority', W.DWORD), ('scheduling', W.DWORD)]


class IoCounters(C.Structure):
    _fields_ = [(n, C.c_ulonglong) for n in ('reads', 'writes', 'others', 'read_bytes', 'write_bytes', 'other_bytes')]


class ExtendedJobLimits(C.Structure):
    _fields_ = [('basic', BasicJobLimits), ('io', IoCounters),
                ('process_memory', C.c_size_t), ('job_memory', C.c_size_t),
                ('peak_process_memory', C.c_size_t), ('peak_job_memory', C.c_size_t)]


def bindings():
    if os.name != 'nt':
        raise RuntimeError('Private desktop benchmarks require Windows')
    u = C.WinDLL('user32', use_last_error=True)
    k = C.WinDLL('kernel32', use_last_error=True)
    for dll, name, result, args in [
        (u, 'CreateDesktopW', W.HANDLE, [W.LPCWSTR, W.LPCWSTR, W.LPVOID, W.DWORD, W.DWORD, W.LPVOID]),
        (u, 'CloseDesktop', W.BOOL, [W.HANDLE]),
        (u, 'OpenInputDesktop', W.HANDLE, [W.DWORD, W.BOOL, W.DWORD]),
        (u, 'GetThreadDesktop', W.HANDLE, [W.DWORD]),
        (u, 'GetWindowThreadProcessId', W.DWORD, [W.HWND, C.POINTER(W.DWORD)]),
        (u, 'IsWindowVisible', W.BOOL, [W.HWND]),
        (u, 'GetUserObjectInformationW', W.BOOL, [W.HANDLE, C.c_int, W.LPVOID, W.DWORD, C.POINTER(W.DWORD)]),
        (k, 'CreateFileW', W.HANDLE, [W.LPCWSTR, W.DWORD, W.DWORD, C.POINTER(SecurityAttributes), W.DWORD, W.DWORD, W.HANDLE]),
        (k, 'CreateProcessW', W.BOOL, [W.LPCWSTR, W.LPWSTR, W.LPVOID, W.LPVOID, W.BOOL, W.DWORD, W.LPVOID, W.LPCWSTR, C.POINTER(StartupInfo), C.POINTER(ProcessInfo)]),
        (k, 'CreateJobObjectW', W.HANDLE, [W.LPVOID, W.LPCWSTR]),
        (k, 'SetInformationJobObject', W.BOOL, [W.HANDLE, C.c_int, W.LPVOID, W.DWORD]),
        (k, 'AssignProcessToJobObject', W.BOOL, [W.HANDLE, W.HANDLE]),
        (k, 'ResumeThread', W.DWORD, [W.HANDLE]),
        (k, 'WaitForSingleObject', W.DWORD, [W.HANDLE, W.DWORD]),
        (k, 'GetExitCodeProcess', W.BOOL, [W.HANDLE, C.POINTER(W.DWORD)]),
        (k, 'TerminateProcess', W.BOOL, [W.HANDLE, W.UINT]),
        (k, 'TerminateJobObject', W.BOOL, [W.HANDLE, W.UINT]),
        (k, 'CloseHandle', W.BOOL, [W.HANDLE]),
        (k, 'OpenProcess', W.HANDLE, [W.DWORD, W.BOOL, W.DWORD]),
        (k, 'IsProcessInJob', W.BOOL, [W.HANDLE, W.HANDLE, C.POINTER(W.BOOL)]),
    ]:
        f = getattr(dll, name)
        f.restype, f.argtypes = result, args
    return u, k


def owned_windows(u, k, desktop, job):
    windows = []
    callback_type = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)

    def inspect(window, unused):
        pid = W.DWORD()
        tid = u.GetWindowThreadProcessId(window, C.byref(pid))
        handle = k.OpenProcess(0x1000, False, pid.value)
        if handle:
            member = W.BOOL()
            try:
                if k.IsProcessInJob(handle, job, C.byref(member)) and member:
                    windows.append({'pid': pid.value, 'thread_id': tid,
                                    'visible_on_private_desktop': bool(u.IsWindowVisible(window))})
            finally:
                k.CloseHandle(handle)
        return True

    callback = callback_type(inspect)
    u.EnumDesktopWindows.argtypes = [W.HANDLE, callback_type, W.LPARAM]
    u.EnumDesktopWindows.restype = W.BOOL
    C.set_last_error(0)
    status = u.EnumDesktopWindows(desktop, callback, 0)
    if not status and C.get_last_error():
        raise C.WinError(C.get_last_error(), 'enumerate private owned windows')
    return windows


def checked(value, operation):
    if not value or value == C.c_void_p(-1).value:
        raise C.WinError(C.get_last_error(), operation)
    return value


def desktop_name(u, handle):
    buffer = C.create_unicode_buffer(512)
    needed = W.DWORD()
    checked(u.GetUserObjectInformationW(handle, 2, buffer, C.sizeof(buffer), C.byref(needed)), 'desktop identity')
    return buffer.value


def input_desktop(u):
    handle = checked(u.OpenInputDesktop(0, False, 1), 'open input desktop')
    try:
        return desktop_name(u, handle)
    finally:
        u.CloseDesktop(handle)


def save(path, value):
    temporary = path.with_suffix(path.suffix + '.writing')
    temporary.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')
    # Windows readers/indexers can briefly deny delete sharing on the previous
    # receipt. Retain atomic publication and the old valid receipt; do not kill
    # an otherwise healthy owned game for a transient metadata-reader lock.
    deadline = time.monotonic() + 2
    while True:
        try:
            temporary.replace(path)
            return
        except PermissionError as error:
            if os.name != 'nt' or error.winerror not in (5,32,33) or time.monotonic() >= deadline:
                raise
            time.sleep(.05)


def launch(executable, arguments, cwd, output, timeout):
    u, k = bindings()
    executable, cwd, output = Path(executable).resolve(strict=True), Path(cwd).resolve(strict=True), Path(output).resolve()
    if not executable.is_file() or not cwd.is_dir() or output.exists():
        raise ValueError('Require an existing executable/directory and a fresh output directory')
    if not 5 <= timeout <= 3600 or not all(isinstance(a, str) and '\x00' not in a for a in arguments):
        raise ValueError('Invalid deadline or arguments')
    output.mkdir(parents=True)
    receipt_path = output / 'DESKTOP-RECEIPT.json'
    before = input_desktop(u)
    name = 'OptimizedMW_Benchmark_' + uuid.uuid4().hex
    desktop = checked(u.CreateDesktopW(name, None, None, 0, 0x01ff, None), 'create private desktop')
    job = process = thread = None
    handles = []
    state = {'schema': 1, 'desktop': name, 'input_desktop_before': before,
             'desktop_switched': False, 'launch_scope': 'private_desktop_windowed_rendering',
             'executable': str(executable), 'arguments': arguments, 'working_directory': str(cwd),
             'deadline_seconds': timeout, 'state': 'preflight', 'timed_out': False}
    with executable.open('rb') as stream:
        state['executable_sha256'] = hashlib.file_digest(stream, 'sha256').hexdigest()
    started = time.monotonic()
    try:
        if desktop_name(u, desktop) != name or input_desktop(u) != before:
            raise RuntimeError('Private desktop isolation preflight failed')
        job = checked(k.CreateJobObjectW(None, None), 'create owned process job')
        limits = ExtendedJobLimits()
        limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        checked(k.SetInformationJobObject(job, 9, C.byref(limits), C.sizeof(limits)), 'job lifetime limit')
        security = SecurityAttributes(C.sizeof(SecurityAttributes), None, True)
        for path, access, disposition in [(str(output/'stdout.log'), 0x40000000, 1),
                                          (str(output/'stderr.log'), 0x40000000, 1), ('NUL', 0x80000000, 3)]:
            handles.append(checked(k.CreateFileW(path, access, 3, C.byref(security), disposition, 0x80, None), 'owned standard handle'))
        si = StartupInfo()
        si.cb, si.desktop = C.sizeof(si), 'WinSta0\\' + name
        si.flags, si.show = 0x100 | 0x80, 0  # std handles, no startup cursor feedback
        si.stdout, si.stderr, si.stdin = handles
        pi = ProcessInfo()
        command = C.create_unicode_buffer(subprocess.list2cmdline([str(executable), *arguments]))
        environment = dict(os.environ)
        if executable.name.lower() == 'powershell.exe':
            # This worker can inherit PowerShell 7's module path. Windows
            # PowerShell 5 must use its own modules (Get-FileHash, CIM, etc.).
            environment = {key: value for key, value in environment.items() if key.lower() != 'psmodulepath'}
            environment['PSModulePath'] = ';'.join((str(executable.parent/'Modules'),
                str(Path(os.environ.get('ProgramFiles', 'C:\\Program Files'))/'WindowsPowerShell'/'Modules')))
            state['powershell_modules'] = 'native_windows_powershell_only'
        block = C.create_unicode_buffer('\x00'.join(f'{key}={value}' for key, value in
            sorted(environment.items(), key=lambda entry: entry[0].upper())) + '\x00\x00')
        checked(k.CreateProcessW(str(executable), command, None, None, True,
                                 0x4 | 0x08000000 | 0x400, block, str(cwd), C.byref(si), C.byref(pi)), 'private desktop process')
        process, thread = pi.process, pi.thread
        state['pid'], state['thread_id'] = pi.pid, pi.tid
        checked(k.AssignProcessToJobObject(job, process), 'own benchmark process tree')
        if k.ResumeThread(thread) == 0xffffffff:
            raise C.WinError(C.get_last_error(), 'resume owned benchmark')
        # Console/managed startup threads may not yet have Win32 desktop state.
        # Verify the explicit assignment now and real owned windows as they appear.
        child_handle = u.GetThreadDesktop(pi.tid)
        if child_handle:
            child_desktop = desktop_name(u, child_handle)
            if child_desktop != name:
                raise RuntimeError('Child was not assigned to its private desktop')
            state['verified_child_desktop'] = child_desktop
        state['startup_desktop'] = si.desktop
        state['owned_private_windows'] = []
        state['owned_render_window_observed'] = False
        state['input_desktop_owned_visible_windows'] = []
        state['state'] = 'running'
        save(receipt_path, state)
        print(json.dumps({'state': 'running', 'pid': pi.pid, 'desktop': name, 'receipt': str(receipt_path)}), flush=True)
        while True:
            status = k.WaitForSingleObject(process, 1000)
            if status == 0:
                break
            if status != 0x102:
                raise C.WinError(C.get_last_error(), 'benchmark wait')
            windows = owned_windows(u, k, desktop, job)
            active = checked(u.OpenInputDesktop(0, False, 1), 'inspect input desktop isolation')
            try:
                escaped = [window for window in owned_windows(u, k, active, job) if window['visible_on_private_desktop']]
            finally:
                u.CloseDesktop(active)
            if escaped:
                state['input_desktop_owned_visible_windows'] = escaped
                raise RuntimeError('Owned window escaped to the input desktop; stopped private process tree')
            if windows:
                state['owned_private_windows'] = windows
                state['owned_render_window_observed'] = any(w['visible_on_private_desktop'] for w in windows)
                save(receipt_path, state)
            if time.monotonic() - started >= timeout:
                state['timed_out'] = True
                checked(k.TerminateJobObject(job, 124), 'terminate only owned expired benchmark')
                k.WaitForSingleObject(process, 10000)
                break
        code = W.DWORD()
        checked(k.GetExitCodeProcess(process, C.byref(code)), 'benchmark exit')
        state['exit_code'] = code.value
        state['state'] = 'timeout' if state['timed_out'] else 'exited'
    except BaseException as error:
        state['state'], state['error'] = 'failed', str(error)
        if process:
            k.TerminateProcess(process, 125)
        raise
    finally:
        if job:
            k.CloseHandle(job)
        if thread:
            k.CloseHandle(thread)
        if process:
            k.CloseHandle(process)
        for handle in handles:
            k.CloseHandle(handle)
        u.CloseDesktop(desktop)
        state['elapsed_seconds'] = time.monotonic() - started
        state['input_desktop_after'] = input_desktop(u)
        state['input_desktop_unchanged'] = state['input_desktop_after'] == before
        save(receipt_path, state)
    print(json.dumps(state), flush=True)
    return 124 if state['timed_out'] else state['exit_code']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', required=True)
    parser.add_argument('--arguments-file', required=True)
    parser.add_argument('--cwd', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--timeout', type=int, default=420)
    args = parser.parse_args()
    argument_path = Path(args.arguments_file)
    if argument_path.stat().st_size > 65536:
        parser.error('Arguments exceed host budget')
    arguments = json.loads(argument_path.read_text(encoding='utf-8-sig'))
    if not isinstance(arguments, list) or len(arguments) > 256:
        parser.error('Expected at most 256 argument strings')
    return launch(args.executable, arguments, args.cwd, args.output, args.timeout)


if __name__ == '__main__':
    raise SystemExit(main())

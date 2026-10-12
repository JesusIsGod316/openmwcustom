"""Detached, bounded controller for the existing private Phase9 matrix launcher.

No game is started directly here. The matrix owns its private-desktop tree and
the Phase9 launcher owns the profile mutex, mute and restoration receipts.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import sys
import time


def save(path, value):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + '.writing')
    temporary.write_text(json.dumps(value, indent=2)+'\n', encoding='utf-8')
    temporary.replace(path)


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def now():
    return datetime.now(timezone.utc).isoformat()


def detached_flags():
    if os.name != 'nt':
        raise RuntimeError('Windows detached supervisor required')
    kernel = C.WinDLL('kernel32', use_last_error=True)
    kernel.GetCurrentProcess.restype = W.HANDLE
    kernel.IsProcessInJob.argtypes = [W.HANDLE, W.HANDLE, C.POINTER(W.BOOL)]
    member = W.BOOL()
    if not kernel.IsProcessInJob(kernel.GetCurrentProcess(), None, C.byref(member)):
        raise C.WinError(C.get_last_error())
    # If the caller job forbids breakaway, launch fails; there is no visible or
    # caller-bound fallback that could silently lose a running benchmark.
    return 0x8 | 0x200 | (0x1000000 if member else 0)  # detached, new group, breakaway


def spawn(arguments, log):
    with Path(log).open('xb') as stream:
        process = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), *arguments],
            stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT,
            close_fds=True, creationflags=detached_flags())
    return process.pid


def mutex():
    kernel = C.WinDLL('kernel32', use_last_error=True)
    kernel.CreateMutexW.argtypes = [W.LPVOID, W.BOOL, W.LPCWSTR]
    kernel.CreateMutexW.restype = W.HANDLE
    kernel.CloseHandle.argtypes = [W.HANDLE]
    handle = kernel.CreateMutexW(None, True, 'Local\\OptimizedMW_PrivateBenchmarkSupervisor')
    if not handle:
        raise C.WinError(C.get_last_error())
    if C.get_last_error() == 183:
        kernel.CloseHandle(handle)
        raise RuntimeError('Another OptimizedMW supervisor is active; never duplicate it')
    return kernel, handle


def occupied():
    # CIM includes private-desktop games and crash monitors. Never stop these.
    query = r"$p=@(Get-CimInstance Win32_Process|Where-Object {$_.Name -match '^(openmw|openmw-launcher|morrowind|openmw-crashmonitor|cl|link|ninja|cmake|ctest|MSBuild|openmw-tests|components-tests)\.exe$'});ConvertTo-Json -InputObject $p.Count -Compress"
    return int(subprocess.check_output(['powershell.exe','-NoProfile','-NonInteractive','-Command',query],
        text=True, creationflags=0x08000000).strip())


def reservation(checkpoint):
    value = read(checkpoint)
    return value.get('runtime_reservation', {}).get('launch_policy', '').startswith('Released')


class PerformanceInfo(C.Structure):
    _fields_=[('cb',W.DWORD)]+[(name,C.c_size_t) for name in (
        'commit','limit','peak','physical','available','cache','kernel','paged','nonpaged','page_size')]+[
        ('handles',W.DWORD),('processes',W.DWORD),('threads',W.DWORD)]


def memory_floor():
    library=C.WinDLL('psapi',use_last_error=True)
    library.GetPerformanceInfo.argtypes=[C.POINTER(PerformanceInfo),W.DWORD]
    sample=PerformanceInfo();sample.cb=C.sizeof(sample)
    if not library.GetPerformanceInfo(C.byref(sample),sample.cb):
        raise C.WinError(C.get_last_error())
    physical=sample.available*sample.page_size
    spare=(sample.limit-sample.commit)*sample.page_size
    return {'utc':now(),'physical_available_bytes':physical,'commit_spare_bytes':spare,
            'qualified':physical>=2*1024**3 and spare>=4*1024**3}


def work(args):
    output = Path(args.output).resolve()
    if output.exists():
        raise ValueError('Fresh supervisor output required')
    output.mkdir(parents=True)
    state = {'schema':1, 'pid':os.getpid(), 'started_utc':now(), 'state':'starting',
             'plan':str(Path(args.plan).resolve()), 'matrix_output':str(output/'Matrix'),
             'scope':'durable controller; all games use Phase9 private launcher',
             'performance_qualified':False}
    kernel, handle = mutex()
    child = None
    stop = output/'STOP-REQUEST.json'
    checkpoint = Path(args.checkpoint).resolve()
    hold = checkpoint.parent/'OVERNIGHT-HOLD.txt'

    def journal(status, **fields):
        state.update(state=status, updated_utc=now(), **fields)
        save(output/'SUPERVISOR.json', state)
        cp = read(checkpoint)
        cp.update(updated_at=state['updated_utc'], state='SUPERVISOR_'+status.upper(),
            active_controller_pid=os.getpid() if status not in ('completed','failed','deferred') else None,
            active_job=state if status not in ('completed','failed','deferred') else None,
            active_matrix=str(output/'Matrix/MATRIX.json') if child and child.poll() is None else None,
            active_matrix_log=str(output/'MATRIX.log') if child and child.poll() is None else None)
        save(checkpoint, cp)

    try:
        plan = read(args.plan)
        plan['stop_request_file'] = str(stop)
        plan['runtime_guard_checkpoint'] = str(checkpoint)
        save(output/'EFFECTIVE-PLAN.json', plan)
        journal('waiting_idle_slot')
        deadline = time.monotonic()+1800
        while True:
            busy=occupied()
            sample=memory_floor()
            state['memory_preflight']=sample
            if not busy and sample['qualified']:
                break
            if hold.exists() or not reservation(checkpoint) or time.monotonic() >= deadline:
                journal('deferred', reason='occupied_game_build_memory_or_reservation')
                return 2
            time.sleep(10)
        if hold.exists() or not reservation(checkpoint):
            journal('deferred', reason='human_hold_or_reservation')
            return 2
        game = Path(plan['game_directory'])
        with (output/'MATRIX.log').open('xb') as log:
            child = subprocess.Popen([sys.executable,str(game/'benchmark_matrix.py'),
                '--plan',str(output/'EFFECTIVE-PLAN.json'),'--output',str(output/'Matrix')],
                cwd=game,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,
                creationflags=0x08000000)
            journal('running', matrix_pid=child.pid)
            while child.poll() is None:
                if (hold.exists() or not reservation(checkpoint)) and not stop.exists():
                    save(stop, {'requested_utc':now(), 'reason':'human_hold_or_reservation',
                                'scope':'cancel exact owned automatic game only'})
                    journal('stop_requested')
                time.sleep(2)
        matrix = output/'Matrix/MATRIX.json'
        terminal = read(matrix) if matrix.exists() else {}
        if child.returncode or terminal.get('state') != 'completed':
            journal('failed', exit_code=child.returncode, matrix_state=terminal.get('state'),
                    reason=terminal.get('error','No completed matrix receipt'))
            return child.returncode or 1
        journal('completed', exit_code=0, matrix_state='completed')
        return 0
    except BaseException as error:
        journal('failed', reason=str(error))
        raise
    finally:
        kernel.CloseHandle(handle)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--spawn', action='store_true')
    parser.add_argument('--proof', action='store_true')
    parser.add_argument('--plan')
    parser.add_argument('--checkpoint')
    parser.add_argument('--output', required=True)
    args=parser.parse_args()
    output=Path(args.output).resolve()
    if args.spawn:
        if output.exists():
            parser.error('Fresh output required')
        arguments=['--output',str(output)]
        if args.proof:
            arguments+=['--proof']
        else:
            if not args.plan or not args.checkpoint:
                parser.error('Plan and checkpoint required')
            arguments+=['--plan',str(Path(args.plan).resolve()),'--checkpoint',str(Path(args.checkpoint).resolve())]
        print(json.dumps({'pid':spawn(arguments,output.with_suffix('.HOST.log')), 'output':str(output)}))
        return 0
    if args.proof:
        # Harmless lifecycle proof: no game, desktop, audio, build or profile.
        output.mkdir(parents=True, exist_ok=False)
        started=time.monotonic()
        while time.monotonic()-started < 20:
            save(output/'PROOF.json',{'pid':os.getpid(),'utc':now(),'state':'running'})
            time.sleep(1)
        save(output/'PROOF.json',{'pid':os.getpid(),'utc':now(),'state':'completed','elapsed':time.monotonic()-started})
        return 0
    if not args.plan or not args.checkpoint:
        parser.error('Plan and checkpoint required')
    return work(args)


if __name__ == '__main__':
    raise SystemExit(main())

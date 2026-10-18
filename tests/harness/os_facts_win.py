# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Windows OS facts for the M0 harness (standard library only: ctypes over kernel32).

The harness reads process facts from /proc on Linux. On Windows the same facts come from the kernel:
  identity(pid)  {"pid", "comm", "state", "ppid", "starttime"}: a Toolhelp32 snapshot gives the parent and
                 the image name; GetProcessTimes gives the creation time (100 ns units since 1601), which
                 plays the role of /proc starttime (a recycled pid has a different one); a process that has
                 exited but whose object is still held open reports state "Z", as a zombie does on Linux.
  children(pid)  pids whose parent is pid (Toolhelp32 snapshot).
  comm(pid)      image name without ".exe".
  rss_kib(pid)   working set in KiB (K32GetProcessMemoryInfo), the closest equivalent of VmRSS.
  kill(pid)      TerminateProcess with exit code 137 (128 + SIGKILL), as the engine's own kill seam uses.
Facts that have no Windows equivalent (rlimits, seccomp status, the environment of another process) are not
provided here; the harness reports those checks as not run.
"""
import ctypes
import ctypes.wintypes as wt

TH32CS_SNAPPROCESS = 0x00000002
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_TERMINATE = 0x0001
SYNCHRONIZE = 0x00100000
STILL_ACTIVE = 259
KILLED_EXIT_CODE = 137
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                ("szExeFile", wt.WCHAR * 260)]


class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD), ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t), ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t), ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t)]


_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
_k32.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]
_k32.Process32FirstW.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
_k32.Process32NextW.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
_k32.OpenProcess.restype = wt.HANDLE
_k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
_k32.CloseHandle.argtypes = [wt.HANDLE]
_k32.GetExitCodeProcess.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]
_k32.GetProcessTimes.argtypes = [wt.HANDLE] + [ctypes.POINTER(wt.FILETIME)] * 4
_k32.TerminateProcess.argtypes = [wt.HANDLE, wt.UINT]
_k32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
_k32.K32GetProcessMemoryInfo.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESS_MEMORY_COUNTERS), wt.DWORD]


def _snapshot() -> dict:
    """pid -> (ppid, image name) for every process listed by Toolhelp32."""
    out = {}
    snap = _k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap is None or snap == INVALID_HANDLE_VALUE:
        return out
    try:
        pe = PROCESSENTRY32W()
        pe.dwSize = ctypes.sizeof(pe)
        ok = _k32.Process32FirstW(snap, ctypes.byref(pe))
        while ok:
            out[int(pe.th32ProcessID)] = (int(pe.th32ParentProcessID), pe.szExeFile)
            ok = _k32.Process32NextW(snap, ctypes.byref(pe))
    finally:
        _k32.CloseHandle(snap)
    return out


def _open(pid: int, access: int):
    h = _k32.OpenProcess(access, False, pid)
    return h or None


def identity(pid):
    if not isinstance(pid, int) or isinstance(pid, bool) or pid <= 0:
        return None
    h = _open(pid, PROCESS_QUERY_LIMITED_INFORMATION)
    if h is None:
        return None
    try:
        code = wt.DWORD(0)
        times = [wt.FILETIME() for _ in range(4)]
        if not _k32.GetProcessTimes(h, *[ctypes.byref(x) for x in times]):
            return None
        created = (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime
        alive = _k32.GetExitCodeProcess(h, ctypes.byref(code)) and code.value == STILL_ACTIVE
    finally:
        _k32.CloseHandle(h)
    ppid, exe = _snapshot().get(pid, (-1, ""))
    name = exe[:-4] if exe.lower().endswith(".exe") else exe
    return {"pid": pid, "comm": name, "state": "R" if alive else "Z", "ppid": ppid, "starttime": created}


def children(pid) -> set:
    return {child for child, (ppid, _) in _snapshot().items() if ppid == pid}


def comm(pid):
    ident = identity(pid)
    return None if ident is None else ident["comm"]


def rss_kib(pid):
    h = _open(pid, PROCESS_QUERY_LIMITED_INFORMATION)
    if h is None:
        return None
    try:
        pmc = PROCESS_MEMORY_COUNTERS()
        pmc.cb = ctypes.sizeof(pmc)
        if not _k32.K32GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb):
            return None
        return int(pmc.WorkingSetSize // 1024)
    finally:
        _k32.CloseHandle(h)


def kill(pid) -> None:
    """Hard kill; raises OSError like os.kill when the process cannot be opened or terminated."""
    h = _open(pid, PROCESS_TERMINATE | SYNCHRONIZE)
    if h is None:
        raise OSError(ctypes.get_last_error(), "OpenProcess failed")
    try:
        if not _k32.TerminateProcess(h, KILLED_EXIT_CODE):
            raise OSError(ctypes.get_last_error(), "TerminateProcess failed")
        _k32.WaitForSingleObject(h, 5000)
    finally:
        _k32.CloseHandle(h)

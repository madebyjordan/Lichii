"""Sampling profiler for Lichii's host threads (LOCAL TEST ONLY, Windows x64).

Briefly suspends the chosen threads of a running lichii.exe ~1000 times a second, records the
instruction pointer and the first return addresses, and reports the hottest functions per thread
using the build's PDB (dbghelp). Run it while the game is in the scene to profile:

    python -I sample_threads.py --seconds 45 --threads GPUBack GPU --out report.txt
"""
import argparse
import collections
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
dbg = ctypes.WinDLL("dbghelp", use_last_error=True)

TH32CS_SNAPPROCESS, TH32CS_SNAPTHREAD = 0x2, 0x4
PROCESS_ALL = 0x1F0FFF
THREAD_ACCESS = 0x0002 | 0x0008 | 0x0010 | 0x0040 | 0x0800  # suspend, get ctx, query, query_limited


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_void_p), ("th32ModuleID", wt.DWORD),
                ("cntThreads", wt.DWORD), ("th32ParentProcessID", wt.DWORD),
                ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                ("szExeFile", ctypes.c_wchar * 260)]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", ctypes.c_long),
                ("tpDeltaPri", ctypes.c_long), ("dwFlags", wt.DWORD)]


# x64 CONTEXT is 1232 bytes and must be 16-byte aligned; Rip at 0xF8, Rsp at 0x98, Rbp at 0xA0.
CONTEXT_SIZE, CONTEXT_FLAGS_OFF, CONTEXT_CONTROL_INTEGER = 1232, 0x30, 0x100001 | 0x100002


def find_pid(name):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    entry = PROCESSENTRY32W(); entry.dwSize = ctypes.sizeof(entry)
    ok = k32.Process32FirstW(snap, ctypes.byref(entry)); pid = None
    while ok:
        if entry.szExeFile.lower() == name:
            pid = entry.th32ProcessID
        ok = k32.Process32NextW(snap, ctypes.byref(entry))
    k32.CloseHandle(snap)
    return pid


def threads_of(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    entry = THREADENTRY32(); entry.dwSize = ctypes.sizeof(entry)
    ok = k32.Thread32First(snap, ctypes.byref(entry)); result = []
    while ok:
        if entry.th32OwnerProcessID == pid:
            result.append(entry.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(entry))
    k32.CloseHandle(snap)
    return result


def thread_name(handle):
    k32.GetThreadDescription.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_wchar_p)]
    ptr = ctypes.c_wchar_p()
    if k32.GetThreadDescription(handle, ctypes.byref(ptr)) < 0 or not ptr.value:
        return ""
    name = ptr.value
    k32.LocalFree(ptr)
    return name


class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", wt.ULONG), ("Size", wt.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", wt.ULONG),
                ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", wt.ULONG),
                ("Scope", wt.ULONG), ("Tag", wt.ULONG), ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG),
                ("Name", ctypes.c_wchar * 512)]


class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_uint64), ("AllocationBase", ctypes.c_uint64),
                ("AllocationProtect", wt.DWORD), ("PartitionId", wt.WORD), ("RegionSize", ctypes.c_uint64),
                ("State", wt.DWORD), ("Protect", wt.DWORD), ("Type", wt.DWORD)]


def make_is_code(hproc):
    k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t]
    k32.VirtualQueryEx.restype = ctypes.c_size_t
    cache = {}

    def is_code(addr):
        page = addr >> 12
        if page not in cache:
            mbi = MEMORY_BASIC_INFORMATION()
            ok = k32.VirtualQueryEx(hproc, addr, ctypes.byref(mbi), ctypes.sizeof(mbi))
            cache[page] = bool(ok) and (mbi.Protect & 0xF0) != 0  # PAGE_EXECUTE*
        return cache[page]
    return is_code


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=45)
    ap.add_argument("--hz", type=float, default=1000)
    ap.add_argument("--threads", nargs="+", default=["GPUBack", "GPU"])
    ap.add_argument("--depth", type=int, default=6, help="return addresses scanned from the stack")
    ap.add_argument("--out", default="sample-report.txt")
    ap.add_argument("--process", default="lichii.exe", help="process to sample (for testing)")
    ap.add_argument("--pid", type=int, help="sample this process id instead (for testing)")
    args = ap.parse_args()

    pid = args.pid or find_pid(args.process.lower())
    if pid is None:
        sys.exit(f"{args.process} is not running")
    if pid == k32.GetCurrentProcessId():
        sys.exit("refusing to sample this process (it would suspend itself)")
    hproc = k32.OpenProcess(PROCESS_ALL, False, pid)
    k32.OpenThread.restype = wt.HANDLE
    targets = {}
    names_seen = collections.Counter()
    for tid in threads_of(pid):
        h = k32.OpenThread(THREAD_ACCESS, False, tid)
        if not h:
            continue
        name = thread_name(h)
        names_seen[name or "<unnamed>"] += 1
        if any(name == want or (want.endswith("*") and name.startswith(want[:-1])) for want in args.threads):
            targets[tid] = (h, name)
        else:
            k32.CloseHandle(h)
    if not targets:
        sys.exit(f"no thread matched {args.threads}; names seen: {dict(names_seen)}")
    print("sampling", {n for _, n in targets.values()}, "for", args.seconds, "s", flush=True)

    dbg.SymSetOptions(0x2 | 0x10 | 0x4)  # undecorated names, line info, deferred loads
    dbg.SymInitializeW(hproc, None, True)  # invade: load the process's modules (PDB next to exe)
    buf = ctypes.create_string_buffer(CONTEXT_SIZE + 16)
    base = (ctypes.addressof(buf) + 15) & ~15
    ctx = (ctypes.c_char * CONTEXT_SIZE).from_address(base)
    k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    stack = ctypes.create_string_buffer(512)
    self_hits = {tid: collections.Counter() for tid in targets}
    incl_stacks = {tid: [] for tid in targets}
    is_code = make_is_code(hproc)
    samples = collections.Counter()
    interval = 1.0 / args.hz
    end = time.perf_counter() + args.seconds
    while time.perf_counter() < end:
        t0 = time.perf_counter()
        for tid, (h, _) in targets.items():
            if k32.SuspendThread(h) == 0xFFFFFFFF:
                continue
            struct.pack_into("<I", ctx, CONTEXT_FLAGS_OFF, CONTEXT_CONTROL_INTEGER)
            ok = k32.GetThreadContext(h, ctypes.c_void_p(base))
            rip = struct.unpack_from("<Q", ctx, 0xF8)[0] if ok else 0
            rsp = struct.unpack_from("<Q", ctx, 0x98)[0] if ok else 0
            got = ctypes.c_size_t()
            have_stack = ok and k32.ReadProcessMemory(hproc, rsp, stack, len(stack), ctypes.byref(got))
            k32.ResumeThread(h)
            if not rip:
                continue
            samples[tid] += 1
            self_hits[tid][rip] += 1
            if have_stack:
                # Stack words that point into executable pages are likely return addresses.
                words = struct.unpack_from(f"<{got.value // 8}Q", stack.raw)
                frames = [rip]
                for w in words:
                    if 0x10000 <= w < 0x800000000000 and is_code(w):
                        frames.append(w)
                        if len(frames) > args.depth:
                            break
                incl_stacks[tid].append(frames)
        rest = interval - (time.perf_counter() - t0)
        if rest > 0:
            time.sleep(rest)

    sym = SYMBOL_INFOW(); sym.SizeOfStruct = 88; sym.MaxNameLen = 512
    cache = {}

    def name_of(addr):
        if addr in cache:
            return cache[addr]
        disp = ctypes.c_uint64()
        if dbg.SymFromAddrW(hproc, ctypes.c_uint64(addr), ctypes.byref(disp), ctypes.byref(sym)):
            text = sym.Name
        else:
            modbase = dbg.SymGetModuleBase64(hproc, ctypes.c_uint64(addr))
            text = f"<module@{modbase:#x}>" if modbase else "<unknown>"
        cache[addr] = text
        return text

    lines = []
    for tid, (h, tname) in targets.items():
        n = max(1, samples[tid])
        by_fn = collections.Counter()
        for addr, c in self_hits[tid].items():
            by_fn[name_of(addr)] += c
        incl = collections.Counter()
        for frames in incl_stacks[tid]:
            for fn in {name_of(a) for a in frames}:
                incl[fn] += 1
        lines.append(f"===== thread {tname} ({tid}): {samples[tid]} samples")
        lines.append("-- self time (where the thread actually is):")
        for fn, c in by_fn.most_common(40):
            lines.append(f"  {100*c/n:5.1f}%  {fn}")
        lines.append("-- on the stack (inclusive, heuristic):")
        for fn, c in incl.most_common(40):
            lines.append(f"  {100*c/n:5.1f}%  {fn}")
    open(args.out, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    print("\n".join(lines[:60]))
    print("written", args.out)


if __name__ == "__main__":
    main()

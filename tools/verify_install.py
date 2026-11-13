#!/usr/bin/env python3
"""End-to-end install check for SWTORFix.dll against a real game binary.

Launches the client suspended, injects the DLL with LoadLibraryA from a remote
thread (exactly what SWTORWrapper does once the game process appears), then
reads the patched bytes back out of the live process and reports per job.

    python tools/verify_install.py --exe "C:\\...\\swtor.exe" --dll SWTORFix.dll

Exit code 0 only when every expected job installed and the bytes read back.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import struct
import sys

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

CREATE_SUSPENDED = 0x00000004
PROCESS_ALL_ACCESS = 0x1F0FFF
MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
PAGE_READWRITE = 0x04
TH32CS_SNAPMODULE = 0x00000008
TH32CS_SNAPMODULE32 = 0x00000010
INFINITE = 0xFFFFFFFF
WAIT_TIMEOUT = 0x102


class STARTUPINFO(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("lpReserved", wt.LPWSTR), ("lpDesktop", wt.LPWSTR),
                ("lpTitle", wt.LPWSTR), ("dwX", wt.DWORD), ("dwY", wt.DWORD),
                ("dwXSize", wt.DWORD), ("dwYSize", wt.DWORD),
                ("dwXCountChars", wt.DWORD), ("dwYCountChars", wt.DWORD),
                ("dwFillAttribute", wt.DWORD), ("dwFlags", wt.DWORD),
                ("wShowWindow", wt.WORD), ("cbReserved2", wt.WORD),
                ("lpReserved2", ctypes.POINTER(ctypes.c_byte)),
                ("hStdInput", wt.HANDLE), ("hStdOutput", wt.HANDLE), ("hStdError", wt.HANDLE)]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", wt.HANDLE), ("hThread", wt.HANDLE),
                ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD)]


class MODULEENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("th32ModuleID", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("GlblcntUsage", wt.DWORD), ("ProccntUsage", wt.DWORD),
                ("modBaseAddr", ctypes.c_void_p), ("modBaseSize", wt.DWORD),
                ("hModule", wt.HMODULE), ("szModule", wt.CHAR * 256),
                ("szExePath", wt.CHAR * 260)]


# ctypes defaults every return value to a 32-bit int, which truncates module
# handles and remote pointers on x64 and makes the remote thread start at a
# garbage address. Every prototype that returns or takes an address is set.
kernel32.CreateProcessW.restype = wt.BOOL
kernel32.CreateProcessW.argtypes = [wt.LPCWSTR, wt.LPWSTR, ctypes.c_void_p, ctypes.c_void_p,
                                    wt.BOOL, wt.DWORD, ctypes.c_void_p, wt.LPCWSTR,
                                    ctypes.POINTER(STARTUPINFO),
                                    ctypes.POINTER(PROCESS_INFORMATION)]
kernel32.VirtualAllocEx.restype = ctypes.c_void_p
kernel32.VirtualAllocEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t,
                                    wt.DWORD, wt.DWORD]
kernel32.WriteProcessMemory.restype = wt.BOOL
kernel32.WriteProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                        ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
kernel32.ReadProcessMemory.restype = wt.BOOL
kernel32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                       ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
kernel32.GetModuleHandleW.restype = wt.HMODULE
kernel32.GetModuleHandleW.argtypes = [wt.LPCWSTR]
kernel32.GetProcAddress.restype = ctypes.c_void_p
kernel32.GetProcAddress.argtypes = [wt.HMODULE, ctypes.c_char_p]
kernel32.CreateRemoteThread.restype = wt.HANDLE
kernel32.CreateRemoteThread.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t,
                                        ctypes.c_void_p, ctypes.c_void_p, wt.DWORD,
                                        ctypes.POINTER(wt.DWORD)]
kernel32.WaitForSingleObject.restype = wt.DWORD
kernel32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
kernel32.GetExitCodeThread.restype = wt.BOOL
kernel32.GetExitCodeThread.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]
kernel32.CreateToolhelp32Snapshot.restype = wt.HANDLE
kernel32.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]
kernel32.Module32First.restype = wt.BOOL
kernel32.Module32First.argtypes = [wt.HANDLE, ctypes.POINTER(MODULEENTRY32)]
kernel32.Module32Next.restype = wt.BOOL
kernel32.Module32Next.argtypes = [wt.HANDLE, ctypes.POINTER(MODULEENTRY32)]
kernel32.TerminateProcess.restype = wt.BOOL
kernel32.TerminateProcess.argtypes = [wt.HANDLE, wt.UINT]
kernel32.CloseHandle.restype = wt.BOOL
kernel32.CloseHandle.argtypes = [wt.HANDLE]


def check(ok, what, detail=""):
    print(f"  [{'PASS' if ok else 'FAIL'}] {what}{(' - ' + detail) if detail else ''}")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True, help="game binary to launch")
    ap.add_argument("--dll", required=True, help="SWTORFix.dll to inject")
    ap.add_argument("--motion-rva", type=lambda s: int(s, 0), default=0x604F20)
    ap.add_argument("--log-rva", type=lambda s: int(s, 0), default=0x530A52)
    ap.add_argument("--flush-seconds", type=float, default=1.0)
    ap.add_argument("--expect-none", action="store_true",
                    help="the DLL should refuse to install on this binary")
    args = ap.parse_args()

    si = STARTUPINFO()
    si.cb = ctypes.sizeof(si)
    pi = PROCESS_INFORMATION()
    if not kernel32.CreateProcessW(args.exe, None, None, None, False,
                                   CREATE_SUSPENDED, None, None,
                                   ctypes.byref(si), ctypes.byref(pi)):
        print(f"CreateProcess failed: {ctypes.get_last_error()}")
        return 2

    results = []
    try:
        h = pi.hProcess
        path = args.dll.encode("mbcs") + b"\0"
        addr = kernel32.VirtualAllocEx(h, None, len(path), MEM_COMMIT | MEM_RESERVE,
                                       PAGE_READWRITE)
        written = ctypes.c_size_t()
        kernel32.WriteProcessMemory(h, ctypes.c_void_p(addr), path, len(path),
                                    ctypes.byref(written))

        k32 = kernel32.GetModuleHandleW("kernel32.dll")
        load_library = kernel32.GetProcAddress(k32, b"LoadLibraryA")
        tid = wt.DWORD()
        th = kernel32.CreateRemoteThread(h, None, 0, ctypes.c_void_p(load_library),
                                         ctypes.c_void_p(addr), 0, ctypes.byref(tid))
        kernel32.WaitForSingleObject(th, 30000)
        code = wt.DWORD()
        kernel32.GetExitCodeThread(th, ctypes.byref(code))
        hmod = code.value
        # A fail-fast inside DllMain surfaces here as 0xC0000409, which is not a handle.
        results.append(check(0 < hmod < 0xFFFFFFFF00000000,
                             "LoadLibraryA in target returned a module handle",
                             f"0x{hmod:X}"))
        if not results[-1]:
            return 1

        # module base of the launched exe
        snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                                 pi.dwProcessId)
        me = MODULEENTRY32()
        me.dwSize = ctypes.sizeof(me)
        base = None
        exe_name = args.exe.replace("/", "\\").rsplit("\\", 1)[-1].lower()
        if kernel32.Module32First(snap, ctypes.byref(me)):
            while True:
                if me.szModule.decode("mbcs").lower() == exe_name:
                    base = me.modBaseAddr
                    break
                if not kernel32.Module32Next(snap, ctypes.byref(me)):
                    break
        kernel32.CloseHandle(snap)
        results.append(check(base is not None, "target module base", hex(base) if base else "?"))
        if base is None:
            return 1

        buf = ctypes.create_string_buffer(16)
        read = ctypes.c_size_t()

        # TeleportSnap: Detours replaces the first 5 bytes with a jmp
        kernel32.ReadProcessMemory(h, ctypes.c_void_p(base + args.motion_rva), buf, 5,
                                   ctypes.byref(read))
        first = buf.raw[:5]
        kernel32.ReadProcessMemory(h, ctypes.c_void_p(base + args.log_rva + 3), buf, 4,
                                   ctypes.byref(read))
        value = struct.unpack("<f", buf.raw[:4])[0]

        if args.expect_none:
            results.append(check(first[0] != 0xE9,
                                 f"TeleportSnap left alone at 0x{args.motion_rva:X}",
                                 f"bytes {first.hex()}"))
            results.append(check(abs(value - args.flush_seconds) > 1e-6,
                                 f"LogFlush left alone at 0x{args.log_rva:X}",
                                 f"{value:.3f} s"))
        else:
            results.append(check(first[0] == 0xE9,
                                 f"TeleportSnap hook at RVA 0x{args.motion_rva:X}",
                                 f"bytes {first.hex()}"))
            results.append(check(abs(value - args.flush_seconds) < 1e-6,
                                 f"LogFlush interval at RVA 0x{args.log_rva:X}",
                                 f"{value:.3f} s"))
    finally:
        kernel32.TerminateProcess(pi.hProcess, 0)
        kernel32.CloseHandle(pi.hThread)
        kernel32.CloseHandle(pi.hProcess)

    ok = all(results)
    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

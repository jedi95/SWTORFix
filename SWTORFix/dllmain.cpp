#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include "Windows.h"
#include "detours.h"

using namespace std;

// ---------------------------------------------------------------------------
// Logging: every step goes to %TEMP%\swtor_teleportsnap.log.
// ---------------------------------------------------------------------------
static void LogLine(const char* fmt, ...)
{
    char path[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, path);
    if (!n) return;
    lstrcatA(path, "swtor_teleportsnap.log");

    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") != 0 || !f) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);

    fputc('\n', f);
    fclose(f);
}

static bool TempFileExists(const char* name)
{
    char path[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, path);
    if (!n) return false;
    lstrcatA(path, name);
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

// ---------------------------------------------------------------------------
// 1. Block display-mode changes (original behaviour).
// ---------------------------------------------------------------------------
LONG(WINAPI* pChange) (LPCTSTR, DEVMODE*, HWND, DWORD, LPVOID) = ChangeDisplaySettingsEx;

LONG WINAPI myChange(LPCTSTR lpszDeviceName, DEVMODE* lpDevMode, HWND hwnd, DWORD dwflags, LPVOID lParam)
{
    return DISP_CHANGE_SUCCESSFUL;
}

// ---------------------------------------------------------------------------
// 2. Teleport snap.
//
//    Mode is chosen by files in %TEMP% (checked once at load):
//      teleportsnap.off        do not install the hook at all
//      teleportsnap.bare       install, hook body is nothing but the passthrough call
//      teleportsnap.verbose    install, log the first 20 calls, never modify dt
//      (none)                  install and apply the patch
//
//    bare separates "does the detour itself work" from "does anything inside the
//    hook break the game". verbose adds the reads and file I/O back on top.
// ---------------------------------------------------------------------------
namespace {

const uintptr_t RVA_MOTION = 0x604F20;

// Full prologue of the target function. The entry point is 0x604F20, NOT 0x604F22:
// 0x604F22 is the second instruction. Hooking there skips the push rbx, which leaves
// the stack 8 bytes out of alignment and crashes on the first call.
const unsigned char EXPECTED[16] = {
    0x40, 0x53,                             // push rbx
    0x48, 0x83, 0xEC, 0x50,                 // sub  rsp,0x50
    0xF7, 0x81, 0xF8, 0x02, 0x00, 0x00,     // test dword [rcx+0x2F8],
    0x00, 0x00, 0x01, 0x00                  //   0x00010000
};

const size_t FLAGS_OFFSET    = 0x2F8;
const size_t BUDGET_OFFSET   = 0x350;
const size_t VELOCITY_OFFSET = 0x408;

const uint32_t MOTION_FLAG = 0x00010000;

// Walking ~0.4 units/s (4 m/s), teleports ~16.6 (166 m/s). 5.0 = 50 m/s.
const float VELOCITY_THRESHOLD_SQ = 25.0f;

enum Mode { MODE_OFF, MODE_BARE, MODE_VERBOSE, MODE_FULL };

typedef void(__fastcall* MotionFn)(void*, float);
MotionFn g_origMotion = nullptr;
Mode g_mode = MODE_OFF;

volatile LONG g_hookCalls = 0;

void __fastcall Hook_Motion(void* self, float dt)
{
    const LONG n = InterlockedIncrement(&g_hookCalls);

    if (g_mode == MODE_VERBOSE && n <= 20) {
        __try {
            const uint32_t flags = *reinterpret_cast<const uint32_t*>(
                                       static_cast<char*>(self) + FLAGS_OFFSET);
            const float* v = reinterpret_cast<const float*>(
                                 static_cast<char*>(self) + VELOCITY_OFFSET);
            LogLine("call %ld self=%p dt=%.6f flags=%08x v=(%.4f,%.4f,%.4f)",
                    n, self, dt, flags, v[0], v[1], v[2]);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            LogLine("call %ld self=%p READ FAULT", n, self);
        }
    }

    if (g_mode == MODE_FULL) {
        __try {
            const uint32_t flags = *reinterpret_cast<const uint32_t*>(
                                       static_cast<char*>(self) + FLAGS_OFFSET);

            // Only touch objects the original function itself will act on.
            if (flags & MOTION_FLAG) {
                const float* v = reinterpret_cast<const float*>(
                                     static_cast<char*>(self) + VELOCITY_OFFSET);
                const float len2 = v[0]*v[0] + v[1]*v[1] + v[2]*v[2];
                if (len2 > VELOCITY_THRESHOLD_SQ) {
                    const float budget = *reinterpret_cast<const float*>(
                                             static_cast<char*>(self) + BUDGET_OFFSET);
                    if (dt < budget) dt = budget;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
    }

    g_origMotion(self, dt);
}

void InstallTeleportSnap()
{
    if (TempFileExists("teleportsnap.off")) {
        g_mode = MODE_OFF;
        LogLine("mode=OFF - hook NOT installed");
        return;
    }
    if (TempFileExists("teleportsnap.bare")) {
        g_mode = MODE_BARE;
        LogLine("mode=BARE - passthrough only, no reads and no logging inside the hook");
    } else if (TempFileExists("teleportsnap.verbose")) {
        g_mode = MODE_VERBOSE;
        LogLine("mode=VERBOSE - logs first 20 calls, dt never modified");
    } else {
        g_mode = MODE_FULL;
        LogLine("mode=FULL - patch active");
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) {
        LogLine("no module base");
        return;
    }

    auto* target = reinterpret_cast<unsigned char*>(base + RVA_MOTION);
    LogLine("target = 0x%p (base 0x%p + 0x%x)", target, (void*)base, (unsigned)RVA_MOTION);

    __try {
        char buf[64];
        int p = 0;
        for (size_t i = 0; i < sizeof(EXPECTED); ++i)
            p += sprintf_s(buf + p, sizeof(buf) - p, "%02x ", target[i]);
        LogLine("prologue bytes: %s", buf);

        for (size_t i = 0; i < sizeof(EXPECTED); ++i) {
            if (target[i] != EXPECTED[i]) {
                LogLine("prologue mismatch at byte %u - NOT installed", (unsigned)i);
                return;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        LogLine("target unreadable - NOT installed");
        return;
    }

    g_origMotion = reinterpret_cast<MotionFn>(target);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    LONG e = DetourAttach(&(PVOID&)g_origMotion, Hook_Motion);
    LONG c = DetourTransactionCommit();

    LogLine("DetourAttach=%ld DetourCommit=%ld trampoline=0x%p", e, c, (void*)g_origMotion);

    if (e == NO_ERROR && c == NO_ERROR)
        LogLine("installed");
    else
        LogLine("detour FAILED - not installed");
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
        case DLL_PROCESS_ATTACH:
        {
            DisableThreadLibraryCalls(hModule);
            LogLine("---- DLL_PROCESS_ATTACH ----");

            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&)pChange, myChange);
            DetourTransactionCommit();
            LogLine("resolution hook done");

            InstallTeleportSnap();
            LogLine("DllMain complete");
            break;
        }
        case DLL_THREAD_ATTACH:
        {
            break;
        }
        case DLL_THREAD_DETACH:
        {
            break;
        }
        case DLL_PROCESS_DETACH:
        {
            LogLine("DLL_PROCESS_DETACH after %ld hook calls", g_hookCalls);
            break;
        }
    }
    return TRUE;
}

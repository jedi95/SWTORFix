// SWTORFix - client-side fixes for the SWTOR client, injected by SWTORWrapper.
//
// Jobs are configurable in SWTORFix.ini, placed beside the DLL:
//
//     [jobs]
//     BlockDisplayChanges=1
//     TeleportSnap=1
//
// A missing file, missing section or missing key leaves that job enabled.
//
// Nothing is written to disk. Failures are reported through OutputDebugString
// only, so DebugView can show them without the shipped build touching the
// filesystem.

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "detours.h"

namespace {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
struct Config
{
    bool blockDisplayChanges;
    bool teleportSnap;
};

Config g_config = { true, true };

void LoadConfig(HMODULE self)
{
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(self, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return;                                   // keep defaults

    char* lastSlash = strrchr(path, '\\');
    if (!lastSlash)
        return;

    if (strcpy_s(lastSlash + 1, MAX_PATH - static_cast<size_t>(lastSlash + 1 - path),
                 "SWTORFix.ini") != 0)
        return;

    // GetPrivateProfileIntA returns the supplied default when the file, the
    // section or the key is absent, which is exactly the behaviour we want.
    g_config.blockDisplayChanges =
        GetPrivateProfileIntA("jobs", "BlockDisplayChanges", 1, path) != 0;
    g_config.teleportSnap =
        GetPrivateProfileIntA("jobs", "TeleportSnap", 1, path) != 0;
}

void Report(const char* what, LONG attached, LONG committed)
{
    char buf[160];
    sprintf_s(buf, sizeof(buf), "[SWTORFix] %s failed (attach=%ld commit=%ld)\n",
              what, attached, committed);
    OutputDebugStringA(buf);
}

// ---------------------------------------------------------------------------
// Job: BlockDisplayChanges
//
// The client calls ChangeDisplaySettingsEx while probing display modes. This
// hook reports success without changing anything. Originally a workaround for
// a driver bug that made startup take a long time.
// ---------------------------------------------------------------------------
LONG(WINAPI* pChangeDisplaySettingsEx)(LPCTSTR, DEVMODE*, HWND, DWORD, LPVOID) =
    ChangeDisplaySettingsEx;

LONG WINAPI Hook_ChangeDisplaySettingsEx(LPCTSTR lpszDeviceName, DEVMODE* lpDevMode,
                                         HWND hwnd, DWORD dwflags, LPVOID lParam)
{
    return DISP_CHANGE_SUCCESSFUL;
}

void InstallBlockDisplayChanges()
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    LONG attached = DetourAttach(&(PVOID&)pChangeDisplaySettingsEx, Hook_ChangeDisplaySettingsEx);
    LONG committed = DetourTransactionCommit();
    if (attached != NO_ERROR || committed != NO_ERROR)
        Report("BlockDisplayChanges", attached, committed);
}

// ---------------------------------------------------------------------------
// Job: TeleportSnap
//
// FUN_140604f20(actor, dt) integrates actor motion:
//
//     dt  = clamp(dt, 0, [actor+0x350])         remaining time budget
//     pos = base + dt * [actor+0x408..0x410]    velocity, memory units per second
//
// A teleport is a high velocity (~16.6 units/s) with a budget of about 0.05 s,
// normally consumed over roughly five frames. If that sequence is cut short the
// actor stops partway along the path - the desync players see. For teleport-sized
// velocities this hook consumes the whole budget in one frame, so the motion
// lands on its endpoint instead of being abandoned mid-way.
//
// The entry point is 0x604F20. 0x604F22 is the second instruction; hooking there
// skips the push rbx and leaves the stack misaligned, which faults on the first
// aligned SSE spill.
// ---------------------------------------------------------------------------
const uintptr_t RVA_MOTION = 0x604F20;

const unsigned char MOTION_PROLOGUE[16] = {
    0x40, 0x53,                             // push rbx
    0x48, 0x83, 0xEC, 0x50,                 // sub  rsp,0x50
    0xF7, 0x81, 0xF8, 0x02, 0x00, 0x00,     // test dword [rcx+0x2F8],
    0x00, 0x00, 0x01, 0x00                  //   0x00010000
};

const size_t FLAGS_OFFSET    = 0x2F8;
const size_t BUDGET_OFFSET   = 0x350;
const size_t VELOCITY_OFFSET = 0x408;

const uint32_t MOTION_FLAG = 0x00010000;      // the gate the original function uses

// Walking measures ~0.4 units/s (4 m/s) and teleports ~16.6 (166 m/s), so 5.0
// units/s (50 m/s) separates them with margin at both ends and stays clear of
// charge-type abilities.
const float VELOCITY_THRESHOLD_SQ = 25.0f;

typedef void(__fastcall* MotionFn)(void*, float);
MotionFn g_originalMotion = nullptr;

void __fastcall Hook_Motion(void* self, float dt)
{
    __try
    {
        const uint32_t flags = *reinterpret_cast<const uint32_t*>(
                                   static_cast<char*>(self) + FLAGS_OFFSET);

        // Only touch objects the original function will act on.
        if (flags & MOTION_FLAG)
        {
            const float* velocity = reinterpret_cast<const float*>(
                                        static_cast<char*>(self) + VELOCITY_OFFSET);
            const float lengthSq = velocity[0] * velocity[0]
                                 + velocity[1] * velocity[1]
                                 + velocity[2] * velocity[2];

            if (lengthSq > VELOCITY_THRESHOLD_SQ)
            {
                const float budget = *reinterpret_cast<const float*>(
                                         static_cast<char*>(self) + BUDGET_OFFSET);
                if (dt < budget)
                    dt = budget;              // consume the whole budget this frame
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // A malformed object must never take the client down; fall through and
        // let the original function decide what to do with it.
    }

    g_originalMotion(self, dt);
}

void InstallTeleportSnap()
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base)
    {
        OutputDebugStringA("[SWTORFix] TeleportSnap: no module base\n");
        return;
    }

    auto* target = reinterpret_cast<unsigned char*>(base + RVA_MOTION);

    __try
    {
        for (size_t i = 0; i < sizeof(MOTION_PROLOGUE); ++i)
        {
            if (target[i] != MOTION_PROLOGUE[i])
            {
                OutputDebugStringA("[SWTORFix] TeleportSnap: prologue mismatch, not installed\n");
                return;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        OutputDebugStringA("[SWTORFix] TeleportSnap: target unreadable, not installed\n");
        return;
    }

    g_originalMotion = reinterpret_cast<MotionFn>(target);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    LONG attached = DetourAttach(&(PVOID&)g_originalMotion, Hook_Motion);
    LONG committed = DetourTransactionCommit();

    if (attached != NO_ERROR || committed != NO_ERROR)
        Report("TeleportSnap", attached, committed);
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        LoadConfig(hModule);

        if (g_config.blockDisplayChanges)
            InstallBlockDisplayChanges();

        if (g_config.teleportSnap)
            InstallTeleportSnap();
    }

    return TRUE;
}

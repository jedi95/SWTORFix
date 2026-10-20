// SWTORFix - client-side fixes for the SWTOR client, injected by SWTORWrapper.
//
// Jobs are configurable in SWTORFix.ini, placed beside the DLL:
//
//     [jobs]
//     BlockDisplayChanges=1
//     TeleportSnap=1
//     LogFlush=1
//
//     [log]
//     FlushMillis=1000
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
    bool logFlush;
    float logFlushSeconds;
    float motionThresholdSq;      // squared velocity threshold, memory units/s
    bool logMotionFires;          // diagnostics: report every snap decision
};

Config g_config = { true, true, true, 1.0f, 100.0f, false };

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
    g_config.logFlush =
        GetPrivateProfileIntA("jobs", "LogFlush", 1, path) != 0;

    // Clamp to a sane range: below 100 ms the file is being flushed constantly,
    // above 10 s the job does nothing useful.
    int flushMillis = GetPrivateProfileIntA("log", "FlushMillis", 1000, path);
    if (flushMillis < 100) flushMillis = 100;
    if (flushMillis > 10000) flushMillis = 10000;
    g_config.logFlushSeconds = flushMillis / 1000.0f;

    // Teleport velocity threshold, in memory units per second (1 unit = 10 m; the
    // client stores positions at one tenth of the combat log's scale).
    // Teleports measure ~16.6 units/s. Measured Force Charge leaps top out at
    // 5.8 units/s (111 samples, 14.8-57.9 m/s over 0.49-0.83 s), so the default
    // of 10 sits clear above every leap and well under a teleport. Lower it if
    // teleports stop being caught; raise it if another fast motion is caught.
    int minVelocity = GetPrivateProfileIntA("teleport", "MinVelocity", 10, path);
    if (minVelocity < 1) minVelocity = 1;
    if (minVelocity > 50) minVelocity = 50;
    g_config.motionThresholdSq = static_cast<float>(minVelocity) * static_cast<float>(minVelocity);

    g_config.logMotionFires = GetPrivateProfileIntA("debug", "LogFires", 0, path) != 0;
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
// Job: LogFlush
//
// The combat log is written to a FILE* held in a small global (DAT_141bae060):
//
//     +0x00  FILE*  file
//     +0x08  float  flush_timer        seconds remaining
//
// The main loop decrements the timer by the frame delta and, when it expires,
// flushes and resets it:
//
//     140530a2d  movss   xmm0, [rbx+8]            ; timer
//     140530a32  subss   xmm0, [frame delta]
//     140530a3a  movss   [rbx+8], xmm0
//     140530a3f  comiss  xmm6, xmm0
//     140530a42  jb      skip                     ; still counting down
//     140530a4c  call    [fflush]
//     140530a52  mov     dword ptr [rbx+8], 3.0f  ; <-- the reset constant
//
// The reset constant *is* the interval, and it is a plain float immediate:
//
//     c7 43 08 00 00 40 40        mov dword ptr [rbx+8], 3.0f
//
// Changing only the four immediate bytes changes how often the log is flushed.
// The instruction length and encoding are untouched, and the CRT still flushes
// on its own when the 4096-byte buffer fills - this only reduces the worst-case
// latency for anything parsing the log live (StarParse and similar).
//
// This job never writes blind: it verifies the full instruction at the known
// RVA, falls back to a scan for a unique match, refuses on absent or ambiguous
// matches, and reads the value back afterwards.
// ---------------------------------------------------------------------------
const uintptr_t RVA_LOG_FLUSH = 0x530A52;

const unsigned char LOG_FLUSH_INSN[7] = {
    0xC7, 0x43, 0x08, 0x00, 0x00, 0x40, 0x40    // mov dword ptr [rbx+8], 3.0f
};

const size_t LOG_FLUSH_IMM = 3;                 // float immediate inside the instruction

struct TextRange { uintptr_t begin; size_t size; };

bool GetTextRange(uintptr_t base, TextRange& out)
{
    auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            out.begin = base + section->VirtualAddress;
            out.size  = section->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

void InstallLogFlush()
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) {
        OutputDebugStringA("[SWTORFix] LogFlush: no module base\n");
        return;
    }

    unsigned char* target = nullptr;

    // 1. Known location.
    auto* candidate = reinterpret_cast<unsigned char*>(base + RVA_LOG_FLUSH);
    bool knownGood = true;
    __try {
        for (size_t i = 0; i < sizeof(LOG_FLUSH_INSN); ++i) {
            if (candidate[i] != LOG_FLUSH_INSN[i]) { knownGood = false; break; }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { knownGood = false; }

    if (knownGood) {
        target = candidate;
    } else {
        // 2. Fall back to a pattern scan, accepting only a single unique hit.
        TextRange text{};
        if (!GetTextRange(base, text)) {
            OutputDebugStringA("[SWTORFix] LogFlush: no executable section, not installed\n");
            return;
        }

        int hits = 0;
        unsigned char* found = nullptr;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(text.begin);
        for (size_t i = 0; i + sizeof(LOG_FLUSH_INSN) <= text.size; ++i) {
            if (p[i] != LOG_FLUSH_INSN[0]) continue;
            bool match = true;
            for (size_t k = 1; k < sizeof(LOG_FLUSH_INSN); ++k) {
                if (p[i + k] != LOG_FLUSH_INSN[k]) { match = false; break; }
            }
            if (!match) continue;
            if (++hits > 1) break;
            found = const_cast<unsigned char*>(p + i);
        }

        if (hits == 0) {
            OutputDebugStringA("[SWTORFix] LogFlush: pattern not found, not installed\n");
            return;
        }
        if (hits > 1) {
            OutputDebugStringA("[SWTORFix] LogFlush: pattern ambiguous, not installed\n");
            return;
        }
        target = found;
    }

    float* immediate = reinterpret_cast<float*>(target + LOG_FLUSH_IMM);
    const float wanted = g_config.logFlushSeconds;

    __try {
        if (*immediate == wanted)
            return;                              // already at the requested interval
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    // 3. Patch the immediate, restore protection, flush the icache, verify.
    DWORD oldProtect = 0;
    if (!VirtualProtect(immediate, sizeof(float), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        OutputDebugStringA("[SWTORFix] LogFlush: VirtualProtect failed\n");
        return;
    }

    *immediate = wanted;
    VirtualProtect(immediate, sizeof(float), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(LOG_FLUSH_INSN));

    if (*immediate != wanted) {
        OutputDebugStringA("[SWTORFix] LogFlush: read-back mismatch\n");
    }
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

const size_t MOTION_FLAGS_OFFSET = 0x420;     // per-actor motion mode bits

typedef void(__fastcall* MotionFn)(void*, float);
MotionFn g_originalMotion = nullptr;

void LogMotionFire(void* self, uint32_t flags2F8, uint32_t flags420,
                   const float* v, float lengthSq, float budget)
{
    char buf[280];
    sprintf_s(buf, sizeof(buf),
              "[SWTORFix] snap self=%p f2F8=%08x f420=%08x v=(%.3f,%.3f,%.3f) "
              "|v|^2=%.3f budget=%.4f\n",
              self, flags2F8, flags420, v[0], v[1], v[2], lengthSq, budget);
    OutputDebugStringA(buf);
}

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

            if (lengthSq > g_config.motionThresholdSq)
            {
                const float budget = *reinterpret_cast<const float*>(
                                         static_cast<char*>(self) + BUDGET_OFFSET);
                if (dt < budget)
                {
                    if (g_config.logMotionFires)
                    {
                        const uint32_t motion = *reinterpret_cast<const uint32_t*>(
                                                    static_cast<char*>(self) + MOTION_FLAGS_OFFSET);
                        LogMotionFire(self, flags, motion, velocity, lengthSq, budget);
                    }
                    dt = budget;              // consume the whole budget this frame
                }
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

        if (g_config.logFlush)
            InstallLogFlush();

        if (g_config.teleportSnap)
            InstallTeleportSnap();
    }

    return TRUE;
}

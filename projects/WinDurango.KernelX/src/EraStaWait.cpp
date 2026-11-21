#include "EraStaWait.h"
#include "Hooks.h"
#include <objbase.h>
#include <ctime>

// On the console a title can block its CoreApplication view thread in task::wait() (a
// std::condition_variable wait). The console's app C++ runtime refuses such a wait on a Windows
// Runtime STA ("Illegal to wait on a task in a Windows Runtime STA"), so that thread does not
// behave as an STA there. On PC the real CoreApplication makes it an application STA (ASTA) and
// desktop msvcp140 lets the wait block. If the work being waited on needs a COM call into that
// thread, for example a callback that was registered from it, the title deadlocks.
//
// Minecraft: Xbox One Edition 1.61 freezes this way while loading a new world: the view thread
// waits on a Microsoft.Xbox.GameChat operation, and GameChat's worker is waiting on a delegate
// call marshaled back to the view thread.
//
// On an ASTA this turns one long wait into short timed waits and lets incoming COM calls run in
// between. Returning without a notify is a spurious wakeup, which callers of _Cnd_wait already
// handle by rechecking their predicate.

namespace
{
using CndT = void *;
using MtxT = void *;

// Layout of both xtime and _timespec64, the absolute UTC time _Cnd_timedwait takes.
struct EraXtime
{
    __time64_t sec;
    long nsec;
};

using CndWaitFn = int(__cdecl *)(CndT, MtxT);
using CndTimedWaitFn = int(__cdecl *)(CndT, MtxT, const EraXtime *);
using MtxFn = int(__cdecl *)(MtxT);

constexpr int ThrdSuccess = 0;  // _Thrd_success
constexpr int ThrdTimedout = 2; // _Thrd_timedout
constexpr long SliceNs = 10'000'000;
constexpr long NsPerSecond = 1'000'000'000;

CndWaitFn TrueCndWait = nullptr;
CndTimedWaitFn TrueCndTimedWait = nullptr;
MtxFn TrueMtxLock = nullptr;
MtxFn TrueMtxUnlock = nullptr;

bool IsApplicationSta()
{
    APTTYPE type;
    APTTYPEQUALIFIER qualifier;
    if (FAILED(CoGetApartmentType(&type, &qualifier)))
        return false;

    return type == APTTYPE_STA && qualifier == APTTYPEQUALIFIER_APPLICATION_STA;
}

void ServiceIncomingCalls()
{
    // Never signaled: the wait only exists so COM can dispatch calls while it lasts.
    thread_local HANDLE idle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (idle == nullptr)
    {
        Sleep(1);
        return;
    }

    DWORD index;
    CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS, 1, 1, &idle, &index);
}

int __cdecl EraCndWait(CndT cnd, MtxT mtx)
{
    if (!IsApplicationSta())
        return TrueCndWait(cnd, mtx);

    timespec now{};
    if (timespec_get(&now, TIME_UTC) != TIME_UTC)
        return TrueCndWait(cnd, mtx);

    EraXtime target{now.tv_sec, now.tv_nsec + SliceNs};
    if (target.nsec >= NsPerSecond)
    {
        target.sec += 1;
        target.nsec -= NsPerSecond;
    }

    int result = TrueCndTimedWait(cnd, mtx, &target);
    if (result != ThrdTimedout)
        return result;

    TrueMtxUnlock(mtx);
    ServiceIncomingCalls();
    TrueMtxLock(mtx);
    return ThrdSuccess;
}
} // namespace

void EraInstallStaWaitPatch(HMODULE titleModule)
{
    HMODULE msvcp = GetModuleHandleW(L"msvcp140.dll");
    if (msvcp == nullptr)
        return;

    TrueCndWait = reinterpret_cast<CndWaitFn>(GetProcAddress(msvcp, "_Cnd_wait"));
    TrueCndTimedWait = reinterpret_cast<CndTimedWaitFn>(GetProcAddress(msvcp, "_Cnd_timedwait"));
    TrueMtxLock = reinterpret_cast<MtxFn>(GetProcAddress(msvcp, "_Mtx_lock"));
    TrueMtxUnlock = reinterpret_cast<MtxFn>(GetProcAddress(msvcp, "_Mtx_unlock"));
    if (!TrueCndWait || !TrueCndTimedWait || !TrueMtxLock || !TrueMtxUnlock)
        return;

    XWinePatchImport(titleModule, msvcp, "_Cnd_wait", reinterpret_cast<PVOID>(&EraCndWait));
}

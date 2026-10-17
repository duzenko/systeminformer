/*
 * CPU frequency via APERF/MPERF through the PawnIO signed driver.
 *
 * Windows 10's ntoskrnl truncates the hybrid performance scale to one byte
 * (PpmRegisterPerfStates), so the SystemProcessorPerformanceDistribution path
 * reports E-core frequencies far too low. Reading the APERF/MPERF counters
 * directly gives the true active frequency and sidesteps the truncated value.
 */

#include <phapp.h>
#include <cpufreqmsr.h>
#include "resource.h"

#define MSR_PLATFORM_INFO 0xCE
#define MSR_MPERF 0xE7
#define MSR_APERF 0xE8

typedef HRESULT (STDAPICALLTYPE *PAWNIO_OPEN)(PHANDLE Handle);
typedef HRESULT (STDAPICALLTYPE *PAWNIO_LOAD)(HANDLE Handle, const UCHAR* Blob, SIZE_T Size);
typedef HRESULT (STDAPICALLTYPE *PAWNIO_EXECUTE)(HANDLE Handle, PCSTR Name, const ULONG64* In, SIZE_T InSize, PULONG64 Out, SIZE_T OutSize, PSIZE_T ReturnSize);
typedef HRESULT (STDAPICALLTYPE *PAWNIO_CLOSE)(HANDLE Handle);

static HMODULE PawnIoModule = NULL;
static HANDLE PawnIoHandle = NULL;
static PAWNIO_OPEN PawnIoOpen = NULL;
static PAWNIO_LOAD PawnIoLoad = NULL;
static PAWNIO_EXECUTE PawnIoExecute = NULL;
static PAWNIO_CLOSE PawnIoClose = NULL;

static ULONG MsrNominalMhz = 0;
static ULONG MsrProcessorCount = 0;
static PULONG64 MsrPrevAperf = NULL;
static PULONG64 MsrPrevMperf = NULL;
static BOOLEAN MsrPrimed = FALSE;

static BOOLEAN PhpMsrRead(
    _In_ ULONG Index,
    _Out_ PULONG64 Value
    )
{
    ULONG64 in = Index;
    ULONG64 out = 0;
    SIZE_T returnSize = 0;

    if (PawnIoExecute(PawnIoHandle, "ioctl_read_msr", &in, 1, &out, 1, &returnSize) != S_OK)
        return FALSE;

    *Value = out;
    return TRUE;
}

static HMODULE PhpLoadPawnIoLibrary(
    VOID
    )
{
    HMODULE module;
    PPH_STRING path;

    if (module = LoadLibraryEx(L"PawnIOLib.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
        return module;

    // Fall back to the installed location.
    path = PhExpandEnvironmentStrings(&(PH_STRINGREF)PH_STRINGREF_INIT(L"%ProgramFiles%\\PawnIO\\PawnIOLib.dll"));

    if (path)
    {
        module = LoadLibrary(PhGetString(path));
        PhDereferenceObject(path);
    }

    return module;
}

BOOLEAN PhInitializeCpuFrequencyMsr(
    _In_ ULONG NumberOfProcessors
    )
{
    HRSRC resourceInfo;
    HGLOBAL resourceData;
    PVOID blob;
    ULONG blobSize;
    ULONG64 platformInfo;

    if (PawnIoHandle)
        return TRUE;
    if (NumberOfProcessors == 0 || NumberOfProcessors > MAXIMUM_PROC_PER_GROUP)
        return FALSE;

    if (!(PawnIoModule = PhpLoadPawnIoLibrary()))
        return FALSE;

    PawnIoOpen = (PAWNIO_OPEN)GetProcAddress(PawnIoModule, "pawnio_open");
    PawnIoLoad = (PAWNIO_LOAD)GetProcAddress(PawnIoModule, "pawnio_load");
    PawnIoExecute = (PAWNIO_EXECUTE)GetProcAddress(PawnIoModule, "pawnio_execute");
    PawnIoClose = (PAWNIO_CLOSE)GetProcAddress(PawnIoModule, "pawnio_close");

    if (!PawnIoOpen || !PawnIoLoad || !PawnIoExecute || !PawnIoClose)
        goto FailExit;

    if (!(resourceInfo = FindResource((HINSTANCE)PhInstanceHandle, MAKEINTRESOURCE(IDR_INTELMSR), RT_RCDATA)))
        goto FailExit;
    if (!(blobSize = SizeofResource((HINSTANCE)PhInstanceHandle, resourceInfo)))
        goto FailExit;
    if (!(resourceData = LoadResource((HINSTANCE)PhInstanceHandle, resourceInfo)))
        goto FailExit;
    if (!(blob = LockResource(resourceData)))
        goto FailExit;

    // Opening the PawnIO device requires elevation; a non-elevated client fails here and
    // falls back to the kernel-reported frequency.
    if (PawnIoOpen(&PawnIoHandle) != S_OK)
        goto FailExit;
    if (PawnIoLoad(PawnIoHandle, blob, blobSize) != S_OK)
        goto FailExit;

    // Nominal (TSC/MPERF reference) frequency = MSR_PLATFORM_INFO[15:8] * 100 MHz.
    if (!PhpMsrRead(MSR_PLATFORM_INFO, &platformInfo))
        goto FailExit;

    MsrNominalMhz = (ULONG)((platformInfo >> 8) & 0xff) * 100;

    if (MsrNominalMhz == 0)
        goto FailExit;

    MsrProcessorCount = NumberOfProcessors;
    MsrPrevAperf = PhAllocateZero(sizeof(ULONG64) * NumberOfProcessors);
    MsrPrevMperf = PhAllocateZero(sizeof(ULONG64) * NumberOfProcessors);
    MsrPrimed = FALSE;

    return TRUE;

FailExit:
    PhCleanupCpuFrequencyMsr();
    return FALSE;
}

VOID PhCleanupCpuFrequencyMsr(
    VOID
    )
{
    if (PawnIoHandle)
    {
        PawnIoClose(PawnIoHandle);
        PawnIoHandle = NULL;
    }
    if (PawnIoModule)
    {
        FreeLibrary(PawnIoModule);
        PawnIoModule = NULL;
    }
    if (MsrPrevAperf)
    {
        PhFree(MsrPrevAperf);
        MsrPrevAperf = NULL;
    }
    if (MsrPrevMperf)
    {
        PhFree(MsrPrevMperf);
        MsrPrevMperf = NULL;
    }

    PawnIoOpen = NULL;
    PawnIoLoad = NULL;
    PawnIoExecute = NULL;
    PawnIoClose = NULL;
    MsrNominalMhz = 0;
    MsrProcessorCount = 0;
    MsrPrimed = FALSE;
}

BOOLEAN PhUpdateCpuFrequencyMsr(
    _Out_writes_(Count) PFLOAT PerCoreMhz,
    _In_ ULONG Count,
    _Out_ PFLOAT OverallMhz
    )
{
    ULONG_PTR originalAffinity = 0;
    BOOLEAN haveOriginal = FALSE;
    DOUBLE sum = 0;
    ULONG counted = 0;

    *OverallMhz = 0;

    if (!PawnIoHandle || Count == 0 || Count != MsrProcessorCount)
        return FALSE;

    for (ULONG i = 0; i < Count; i++)
    {
        ULONG_PTR previousAffinity;
        ULONG64 aperf;
        ULONG64 mperf;

        previousAffinity = SetThreadAffinityMask(GetCurrentThread(), (ULONG_PTR)1 << i);

        if (previousAffinity == 0)
        {
            PerCoreMhz[i] = (FLOAT)MsrNominalMhz;
            continue;
        }

        if (!haveOriginal)
        {
            originalAffinity = previousAffinity;
            haveOriginal = TRUE;
        }

        if (!PhpMsrRead(MSR_APERF, &aperf) || !PhpMsrRead(MSR_MPERF, &mperf))
        {
            PerCoreMhz[i] = (FLOAT)MsrNominalMhz;
            continue;
        }

        if (MsrPrimed)
        {
            ULONG64 deltaAperf = aperf - MsrPrevAperf[i];
            ULONG64 deltaMperf = mperf - MsrPrevMperf[i];
            FLOAT mhz;

            if (deltaMperf != 0)
                mhz = (FLOAT)((DOUBLE)MsrNominalMhz * (DOUBLE)deltaAperf / (DOUBLE)deltaMperf);
            else
                mhz = (FLOAT)MsrNominalMhz; // core idle for the whole interval

            PerCoreMhz[i] = mhz;
            sum += mhz;
            counted++;
        }
        else
        {
            PerCoreMhz[i] = (FLOAT)MsrNominalMhz;
        }

        MsrPrevAperf[i] = aperf;
        MsrPrevMperf[i] = mperf;
    }

    if (haveOriginal)
        SetThreadAffinityMask(GetCurrentThread(), originalAffinity);

    if (!MsrPrimed)
    {
        MsrPrimed = TRUE;
        return FALSE; // first call only primes the counters; no deltas yet
    }

    *OverallMhz = counted ? (FLOAT)(sum / counted) : (FLOAT)MsrNominalMhz;
    return TRUE;
}

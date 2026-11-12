#ifndef PH_CPUFREQMSR_H
#define PH_CPUFREQMSR_H

// Reads true per-core frequency from APERF/MPERF via the PawnIO signed driver,
// bypassing the Windows 10 kernel truncation of the hybrid performance scale.

BOOLEAN PhInitializeCpuFrequencyMsr(
    _In_ ULONG NumberOfProcessors
    );

VOID PhCleanupCpuFrequencyMsr(
    VOID
    );

// Fills PerCoreMhz[0..Count) with active frequency (nominal * dAPERF/dMPERF) and
// OverallMhz with their mean. Returns FALSE if unavailable or on the priming call.
BOOLEAN PhUpdateCpuFrequencyMsr(
    _Out_writes_(Count) PFLOAT PerCoreMhz,
    _In_ ULONG Count,
    _Out_ PFLOAT OverallMhz
    );

#endif

#include "runtime/common/runtime.hpp"
#include "runtime/durango/runtime.hpp"
#include "runtime/modules/kernel.hpp"
#include "runtime/modules/memory.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <windows.h>

#include <psapi.h> // after windows.h

namespace xv::runtime::durango {
namespace {
// xaudio2_9.dll: the console's creates the engine through CreateXAudio2Object, the host's
// through XAudio2Create. The engine and its voices are the same interfaces. The processor
// argument names console cores, so the host's default is used instead.
HRESULT __cdecl create_xaudio2(void **engine, UINT32, UINT32, void *) {
    using Create = HRESULT(__cdecl *)(void **, UINT32, UINT32);
    const auto library = LoadLibraryExW(L"xaudio2_9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto create =
        library ? reinterpret_cast<Create>(GetProcAddress(library, "XAudio2Create")) : nullptr;
    const auto result = create ? create(engine, 0, 1) : E_NOTIMPL;
    std::fprintf(stderr, "[Runtime] XAudio2 engine on the host: 0x%08lx\n",
                 static_cast<unsigned long>(result));
    std::fflush(stderr);
    return result;
}
FARPROC WINAPI get_proc_address(HMODULE module, const char *name) {
    const bool named = reinterpret_cast<std::uintptr_t>(name) >= 0x10000;
    if (named && !std::strcmp(name, "CreateXAudio2Object"))
        return reinterpret_cast<FARPROC>(&create_xaudio2);
    return modules::get_proc_address(module, name);
}
// CONSOLE_TYPE: unknown, Xbox One, One S, One X, One X devkit; the one of the settings, the
// One X without a choice, which is what titles with an Xbox One X mode look for
int WINAPI get_console_type() {
    switch (model_for(Target::durango, console_model())) {
    case ConsoleModel::xbox_one:
        return 1;
    case ConsoleModel::xbox_one_s:
        return 2;
    default:
        return 3;
    }
}
BOOL WINAPI set_thread_name(HANDLE thread, const wchar_t *name) {
    return SUCCEEDED(SetThreadDescription(thread, name));
}
BOOL WINAPI get_thread_name(HANDLE thread, wchar_t *name, SIZE_T capacity, SIZE_T *length) {
    if (!length) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    wchar_t *description = nullptr;
    if (const auto result = GetThreadDescription(thread, &description); FAILED(result)) {
        *length = 0;
        SetLastError(HRESULT_CODE(result));
        return FALSE;
    }
    *length = wcslen(description);
    const bool fits = name && *length < capacity;
    if (fits)
        std::memcpy(name, description, (*length + 1) * sizeof(wchar_t));
    LocalFree(description);
    SetLastError(fits ? ERROR_SUCCESS : ERROR_INSUFFICIENT_BUFFER);
    return fits;
}
// SYSTEMOSVERSIONINFO: the host's version
struct SystemVersion {
    BYTE major, minor;
    WORD build, revision;
};
void WINAPI get_system_os_version(SystemVersion *version) {
    using Get = LONG(WINAPI *)(OSVERSIONINFOW *);
    OSVERSIONINFOW host{sizeof host};
    if (const auto get =
            reinterpret_cast<Get>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
        get(&host);
    DWORD revision = 0, size = sizeof revision;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR",
                     RRF_RT_REG_DWORD, nullptr, &revision, &size) != ERROR_SUCCESS)
        revision = 0;
    *version = {static_cast<BYTE>(host.dwMajorVersion), static_cast<BYTE>(host.dwMinorVersion),
                static_cast<WORD>(host.dwBuildNumber), static_cast<WORD>(revision)};
}
// PROCESSOR_SCHEDULING_STATISTICS, in 100 ns units
struct SchedulingStatistics {
    ULONGLONG running, idle, global;
};
void WINAPI query_processor_scheduling_statistics(SchedulingStatistics *statistics) {
    LARGE_INTEGER frequency, counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    FILETIME idle, kernel, user;
    const auto value = [](FILETIME time) {
        return ULARGE_INTEGER{{time.dwLowDateTime, time.dwHighDateTime}}.QuadPart;
    };
    *statistics = {0, 0,
                   static_cast<ULONGLONG>(counter.QuadPart / (frequency.QuadPart / 10000000))};
    if (GetSystemTimes(&idle, &kernel, &user))
        *statistics = {value(kernel) - value(idle) + value(user), value(idle), statistics->global};
}
// TITLEMEMORYSTATUS: the memory of a title, from what the host has
struct TitleMemory {
    DWORD length, reserved;
    ULONGLONG total, available, legacy_used, legacy_peak, legacy_available, title_used,
        title_available, legacy_page_tables, title_page_tables;
};
BOOL WINAPI title_memory_status(TitleMemory *status) {
    if (!status || status->length < sizeof *status) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    MEMORYSTATUSEX host{sizeof host};
    PROCESS_MEMORY_COUNTERS_EX process{};
    process.cb = sizeof process;
    if (!GlobalMemoryStatusEx(&host) ||
        !K32GetProcessMemoryInfo(GetCurrentProcess(),
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&process),
                                 sizeof process))
        return FALSE;
    // What a title gets on the console it is told it runs on, or less if the host has less in
    // all. How much of the host's is free at the moment does not count: a console's memory
    // is the title's alone, and titles lay their heaps out by what is left of it.
    const ULONGLONG budget = std::min<ULONGLONG>(title_memory(console_model()), host.ullTotalPhys);
    const ULONGLONG used = std::min<ULONGLONG>(process.PrivateUsage, budget);
    const ULONGLONG available = budget - used;
    *status = {sizeof *status, 0, budget, available, 0, 0, 0, used, available, 0, 0};
    return TRUE;
}
} // namespace

void install_kernelx(Registry &registry) {
    registry.add("kernelx.dll", "GetConsoleType", reinterpret_cast<void *>(&get_console_type));
    registry.add("kernelx.dll", "SetThreadName", reinterpret_cast<void *>(&set_thread_name));
    registry.add("kernelx.dll", "GetThreadName", reinterpret_cast<void *>(&get_thread_name));
    registry.add("kernelx.dll", "GetSystemOSVersion",
                 reinterpret_cast<void *>(&get_system_os_version));
    registry.add("kernelx.dll", "QueryProcessorSchedulingStatistics",
                 reinterpret_cast<void *>(&query_processor_scheduling_statistics));
    for (const auto *name : {"TitleMemoryStatus", "JobTitleMemoryStatus"})
        registry.add("kernelx.dll", name, reinterpret_cast<void *>(&title_memory_status));
    // the Win32 functions both kinds of title get the console's answer for
    for (const auto &[name, function] : modules::kernel_functions())
        if (std::string_view(name) != "GetProcAddress")
            registry.add("kernelx.dll", name, function);
    registry.add("kernelx.dll", "GetProcAddress", reinterpret_cast<void *>(&get_proc_address));
    // in kernelbase on the desktop, not kernel32
    if (const auto base = LoadLibraryExW(L"kernelbase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
        for (const auto *name :
             {"EventRegister", "EventUnregister", "EventWrite", "EventWriteEx", "EventWriteString",
              "EventWriteTransfer", "EventEnabled", "EventProviderEnabled", "EventSetInformation",
              "EventActivityIdControl", "TraceEvent", "WaitOnAddress", "WakeByAddressSingle",
              "WakeByAddressAll"})
            if (const auto function = GetProcAddress(base, name))
                registry.add("kernelx.dll", name, reinterpret_cast<void *>(function));
    // classic ETW, in advapi32 on the desktop
    if (const auto advapi = LoadLibraryExW(L"advapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
        for (const auto *name :
             {"RegisterTraceGuidsW", "RegisterTraceGuidsA", "UnregisterTraceGuids",
              "GetTraceLoggerHandle", "GetTraceEnableLevel", "GetTraceEnableFlags", "TraceMessage",
              "TraceMessageVa", "TraceEventInstance"})
            if (const auto function = GetProcAddress(advapi, name))
                registry.add("kernelx.dll", name, reinterpret_cast<void *>(function));
    for (const auto &[name, function] : modules::memory_functions())
        registry.add("kernelx.dll", name, function);
    for (const auto *library : {"d3d11_x.dll", "d3d12_x.dll"}) {
        registry.add(library, "D3DAllocateGraphicsMemory",
                     reinterpret_cast<void *>(&modules::allocate_graphics_memory));
        registry.add(library, "D3DFreeGraphicsMemory",
                     reinterpret_cast<void *>(&modules::free_graphics_memory));
    }
}
} // namespace xv::runtime::durango

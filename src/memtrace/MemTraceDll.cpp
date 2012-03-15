/* Copyright 2012 the SumatraPDF project authors (see AUTHORS file).
License: Simplified BSD (see COPYING.BSD) */

/*
memtrace.dll enables tracing memory allocations in arbitrary programs.
It hooks RtlFreeHeap etc. APIs in the process and sends collected information
(allocation address/size, address of freed memory, callstacks if possible etc.)
to an external collection and visualization process via named pipe.

The dll can either be injected into arbitrary processes or an app can load it
by itself (easier to integrate than injecting dll).

If the collection process doesn't run when memtrace.dll is initialized, we do
nothing.
*/

#include <stddef.h> // for offsetof
#include "BaseUtil.h"
#include "MemTraceDll.h"

#include "nsWindowsDllInterceptor.h"
#include "StrUtil.h"
#include "Vec.h"

#define NOLOG 0  // always log
#include "DebugLog.h"

static HANDLE gModule;
static HANDLE gPipe;

struct AllocData {
    int64     size;
    uint64    addr;
};

struct FreeData {
    uint64    addr;
};

struct SerializeInfo {
    enum Type { Int64, UInt64, Sentinel };
    Type    type;
    int     offset;

    bool IsSentinel() const { return Sentinel == type; };
};

#define SERIALIZEINFO_SENTINEL { SerializeInfo::Sentinel, 0 }

SerializeInfo allocDataSerInfo[] = {
    { SerializeInfo::Int64, offsetof(AllocData, size) },
    { SerializeInfo::UInt64, offsetof(AllocData, addr) },
    SERIALIZEINFO_SENTINEL
};

// data is a pointer to a struct being serialized and serInfo describes
// the struct. res is a result as a stream of bytes
void SerializeStruct(char *data, SerializeInfo *serInfo, Vec<byte>& res)
{
    res.Reset();
    // reserve space for the size of the packet, which we only know
    // after serializeing the data. We're making an assumption here
    // that serialized data will be smaller than 65k
    // note: we can easily relax that by using uint32 for len
    uint16 *resLen = (uint16*)res.AppendBlanks(2);
    uint16 len = 0;
    while (!serInfo->IsSentinel()) {
        ++serInfo;
    }
    *resLen = len;
}

WindowsDllInterceptor gNtdllIntercept;

//http://msdn.microsoft.com/en-us/library/windows/hardware/ff552108(v=vs.85).aspx
PVOID (WINAPI *gRtlAllocateHeapOrig)(PVOID heapHandle, ULONG flags, SIZE_T size);
// http://msdn.microsoft.com/en-us/library/windows/hardware/ff552276(v=vs.85).aspx
BOOLEAN (WINAPI *gRtlFreeHeapOrig)(PVOID heapHandle, ULONG flags, PVOID heapBase);

#define PIPE_NAME "\\\\.\\pipe\\MemTraceCollectorPipe"

// note: must be careful to not allocate memory in this function to avoid
// infinite recursion
PVOID WINAPI RtlAllocateHeapHook(PVOID heapHandle, ULONG flags, SIZE_T size)
{
    PVOID res = gRtlAllocateHeapOrig(heapHandle, flags, size);
    return res;
}

BOOLEAN WINAPI RtlFreeHeapHook(PVOID heapHandle, ULONG flags, PVOID heapBase)
{
    BOOLEAN res = gRtlFreeHeapOrig(heapHandle, flags, heapBase);
    return res;
}

static bool WriteToPipe(const char *s)
{
    DWORD size;
    if (!gPipe)
        return false;
    DWORD sLen = str::Len(s);
    BOOL ok = WriteFile(gPipe, s, (DWORD)sLen, &size, NULL);
    if (!ok || (size != sLen))
        return false;
    return true;
}

static bool TryOpenPipe()
{
    gPipe = CreateFileA(PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING,
        SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION | FILE_FLAG_OVERLAPPED, NULL);
    if (INVALID_HANDLE_VALUE == gPipe) {
        gPipe = NULL;
        return false;
    }
    WriteToPipe("hello, sailor");
    return true;
}

static void ClosePipe()
{
    if (gPipe && (INVALID_HANDLE_VALUE != gPipe))
        CloseHandle(gPipe);
    gPipe = NULL;
}

static void InstallHooks()
{
    gNtdllIntercept.Init("ntdll.dll");
    bool ok = gNtdllIntercept.AddHook("RtlAllocateHeap", reinterpret_cast<intptr_t>(RtlAllocateHeapHook), (void**) &gRtlAllocateHeapOrig);
    if (ok)
        lf("Hooked RtlAllocateHeap");
    else
        lf("failed to hook RtlAllocateHeap");

    ok = gNtdllIntercept.AddHook("RtlFreeHeap", reinterpret_cast<intptr_t>(RtlFreeHeapHook), (void**) &gRtlFreeHeapOrig);
    if (ok)
        lf("Hooked RtlFreeHeap");
    else
        lf("failed to hook RtlFreeHeap");
}

static BOOL ProcessAttach()
{
    lf("ProcessAttach()");
    if (!TryOpenPipe()) {
        lf("couldn't open pipe");
        return FALSE;
    } else {
        lf("opened pipe");
    }
    InstallHooks();
    return TRUE;
}

static BOOL ProcessDetach()
{
    lf("ProcessDetach()");
    ClosePipe();
    return TRUE;
}

static BOOL ThreadAttach()
{
    return TRUE;
}

static BOOL ThreadDetach()
{
    return TRUE;
}

BOOL APIENTRY DllMain(HANDLE hModule, DWORD dwReason, LPVOID lpReserved)
{
    gModule = hModule;
    if (DLL_PROCESS_ATTACH == dwReason)
        return ProcessAttach();
    if (DLL_PROCESS_DETACH == dwReason)
        return ProcessDetach();
    if (DLL_THREAD_ATTACH == dwReason)
        return ThreadAttach();
    if (DLL_THREAD_DETACH == dwReason)
        return ThreadDetach();

    return TRUE;
}

// Diagnostic-only linker wrapper for the preserved mouse-smooth-review objects.
// Production SimSession::open now registers once directly. This wrapper allows a
// single-change live trial without bringing unfinished feature work into the sim.
#include <MSFS/MSFS_WindowsTypes.h>
#include <SimConnect.h>

namespace {
HANDLE registeredHandle = 0;
DispatchProc registeredProc = nullptr;
void* registeredContext = nullptr;
}

extern "C" {
HRESULT __real_SimConnect_CallDispatch(HANDLE, DispatchProc, void*);
HRESULT __real_SimConnect_Close(HANDLE);

HRESULT __wrap_SimConnect_CallDispatch(HANDLE handle, DispatchProc proc, void* context) {
    if (handle && handle == registeredHandle && proc == registeredProc && context == registeredContext) return S_OK;
    const HRESULT result = __real_SimConnect_CallDispatch(handle, proc, context);
    if (SUCCEEDED(result)) {
        registeredHandle = handle;
        registeredProc = proc;
        registeredContext = context;
    }
    return result;
}

HRESULT __wrap_SimConnect_Close(HANDLE handle) {
    const HRESULT result = __real_SimConnect_Close(handle);
    if (SUCCEEDED(result) && handle == registeredHandle) {
        registeredHandle = 0;
        registeredProc = nullptr;
        registeredContext = nullptr;
    }
    return result;
}
}

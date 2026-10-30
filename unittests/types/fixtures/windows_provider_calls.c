// Original import-only fixture, compiled against the pinned Microsoft SDK.
#include <windows.h>

// The provider headers require Windows' base declarations first.
#include <evntprov.h>
#include <pathcch.h>
#include <winstring.h>

__declspec(dllexport) ULONG UseEventRegistration(const GUID *Id,
                                                 PENABLECALLBACK Callback,
                                                 void *Context,
                                                 REGHANDLE *Handle) {
  return EventRegister(Id, Callback, Context, Handle);
}

__declspec(dllexport) HRESULT UsePath(wchar_t *Output, size_t Capacity,
                                      const wchar_t *Base, const wchar_t *More,
                                      ULONG Flags) {
  return PathCchCombineEx(Output, Capacity, Base, More, Flags);
}

__declspec(dllexport) HRESULT UseRuntimeString(const wchar_t *Text,
                                               UINT32 Length, HSTRING *String) {
  return WindowsCreateString(Text, Length, String);
}

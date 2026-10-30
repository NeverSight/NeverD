// Original import-only fixture, compiled against the pinned Microsoft SDK.
#include <windows.h>

// The provider headers require Windows' base declarations first.
#include <clfsw32.h>
#include <d3dkmthk.h>
#include <evntprov.h>
#include <pathcch.h>
#include <tbs.h>
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

__declspec(dllexport) NTSTATUS UseGraphicsDevice(D3DKMT_CREATEDEVICE *Device) {
  return D3DKMTCreateDevice(Device);
}

__declspec(dllexport) HANDLE UseLogFile(const wchar_t *Name, ACCESS_MASK Access,
                                        DWORD Share,
                                        LPSECURITY_ATTRIBUTES Security,
                                        ULONG Creation, ULONG Flags) {
  return CreateLogFile(Name, Access, Share, Security, Creation, Flags);
}

__declspec(dllexport) TBS_RESULT UseTpmContext(PCTBS_CONTEXT_PARAMS Parameters,
                                               PTBS_HCONTEXT Context) {
  return Tbsi_Context_Create(Parameters, Context);
}

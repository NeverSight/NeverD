// Original import-only fixture, compiled against the pinned Microsoft SDK.
#include <windows.h>

// The provider headers require Windows' base declarations first.
#include <clfsw32.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <d3dkmthk.h>
#include <dxgi1_6.h>
#include <evntprov.h>
#include <pathcch.h>
#include <tbs.h>
#include <winstring.h>

// The provider authenticates the SDK's exact C++ type before this compatible
// C redeclaration: the SDK's own () spelling supplies no C parameter list.
HRESULT WINAPI DXGIDeclareAdapterRemovalSupport(void);

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

__declspec(dllexport) HRESULT
UseD3D11Device(IDXGIAdapter *Adapter, D3D_DRIVER_TYPE Type, HMODULE Software,
               UINT Flags, const D3D_FEATURE_LEVEL *Levels, UINT Count,
               UINT Version, ID3D11Device **Device, D3D_FEATURE_LEVEL *Selected,
               ID3D11DeviceContext **Context) {
  return D3D11CreateDevice(Adapter, Type, Software, Flags, Levels, Count,
                           Version, Device, Selected, Context);
}

__declspec(dllexport) HRESULT
UseD3D11SwapChain(IDXGIAdapter *Adapter, D3D_DRIVER_TYPE Type, HMODULE Software,
                  UINT Flags, const D3D_FEATURE_LEVEL *Levels, UINT Count,
                  UINT Version, const DXGI_SWAP_CHAIN_DESC *Description,
                  IDXGISwapChain **SwapChain, ID3D11Device **Device,
                  D3D_FEATURE_LEVEL *Selected, ID3D11DeviceContext **Context) {
  return D3D11CreateDeviceAndSwapChain(Adapter, Type, Software, Flags, Levels,
                                       Count, Version, Description, SwapChain,
                                       Device, Selected, Context);
}

__declspec(dllexport) HRESULT UseD3D12Device(IUnknown *Adapter,
                                             D3D_FEATURE_LEVEL Level,
                                             REFIID Interface, void **Device) {
  return D3D12CreateDevice(Adapter, Level, Interface, Device);
}

__declspec(dllexport) HRESULT
UseD3D12RootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *Description,
                      ID3DBlob **Blob, ID3DBlob **Errors) {
  return D3D12SerializeVersionedRootSignature(Description, Blob, Errors);
}

__declspec(dllexport) HRESULT UseDXGIFactory(UINT Flags, REFIID Interface,
                                             void **Factory) {
  return CreateDXGIFactory2(Flags, Interface, Factory);
}

__declspec(dllexport) HRESULT
UseShaderCompile(const void *Data, SIZE_T Size, const char *Name,
                 const D3D_SHADER_MACRO *Defines, ID3DInclude *Include,
                 const char *Entry, const char *Target, UINT Flags1,
                 UINT Flags2, ID3DBlob **Code, ID3DBlob **Errors) {
  return D3DCompile(Data, Size, Name, Defines, Include, Entry, Target, Flags1,
                    Flags2, Code, Errors);
}

__declspec(dllexport) HRESULT
UseShaderCompile2(const void *Data, SIZE_T Size, const char *Name,
                  const D3D_SHADER_MACRO *Defines, ID3DInclude *Include,
                  const char *Entry, const char *Target, UINT Flags1,
                  UINT Flags2, UINT SecondaryFlags, const void *SecondaryData,
                  SIZE_T SecondarySize, ID3DBlob **Code, ID3DBlob **Errors) {
  return D3DCompile2(Data, Size, Name, Defines, Include, Entry, Target, Flags1,
                     Flags2, SecondaryFlags, SecondaryData, SecondarySize, Code,
                     Errors);
}

__declspec(dllexport) HRESULT UseShaderBlob(SIZE_T Size, ID3DBlob **Blob) {
  return D3DCreateBlob(Size, Blob);
}

__declspec(dllexport) HRESULT UseAdapterRemovalSupport(void) {
  return DXGIDeclareAdapterRemovalSupport();
}

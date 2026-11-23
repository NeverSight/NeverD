//===- windows_sdk_ordinal_calls.c - Microsoft SDK ordinal callers -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Calls from original Windows headers that link through module-local ordinals.
///
//===----------------------------------------------------------------------===//

// clang-format off
#include <windows.h>
#include <compressapi.h>
#include <mapi.h>
#include <prntvpt.h>
#include <xinput.h>
// clang-format on

__declspec(dllexport) BOOL __cdecl
UseCloseCompressor(COMPRESSOR_HANDLE Handle) {
  return CloseCompressor(Handle);
}

__declspec(dllexport) BOOL __cdecl
UseCloseDecompressor(DECOMPRESSOR_HANDLE Handle) {
  return CloseDecompressor(Handle);
}

__declspec(dllexport) HRESULT __cdecl
UsePrintTicketHandle(HPTPROVIDER Provider) {
  return PTCloseProvider(Provider);
}

__declspec(dllexport) DWORD __cdecl UseControllerState(DWORD Index,
                                                       XINPUT_STATE *State) {
  return XInputGetState(Index, State);
}

__declspec(dllexport) ULONG __cdecl UseFreeMessageBuffer(void *Buffer) {
  return MAPIFreeBuffer(Buffer);
}

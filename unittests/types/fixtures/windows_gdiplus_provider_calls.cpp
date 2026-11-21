//===- windows_gdiplus_provider_calls.cpp - Original GDI+ callers --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Import-only callers compiled against the pinned Microsoft SDK.
///
//===----------------------------------------------------------------------===//
#include <windows.h>

#include <objidl.h>
#include <gdiplus.h>

using namespace Gdiplus;
using namespace Gdiplus::DllExports;

extern "C" {
__declspec(dllexport) GpStatus __cdecl UseGraphicsPath(GpFillMode Mode,
                                                       GpPath **Path) {
  return GdipCreatePath(Mode, Path);
}

__declspec(dllexport) GpStatus __cdecl UseImageEncoderSizes(UINT *Count,
                                                           UINT *Size) {
  return GdipGetImageEncodersSize(Count, Size);
}

__declspec(dllexport) GpStatus __cdecl
UseMetafileHeader(HMETAFILE Handle, const WmfPlaceableFileHeader *Placeable,
                  MetafileHeader *Header) {
  return GdipGetMetafileHeaderFromWmf(Handle, Placeable, Header);
}
}

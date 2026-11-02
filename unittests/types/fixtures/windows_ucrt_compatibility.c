// Original public call fixtures for the reviewed compatibility declarations.
// ABI evidence: Wine 840a939ee526137639665ff5f587224d6a36403f,
// dlls/msvcrt/math.c (_except1), dlls/msvcrt/cpp.c (type-info cleanup).
// No Wine implementation is included or executed by these fixtures.
#include <fpieee.h>
#include <windows.h>

double __cdecl _except1(DWORD, _FP_OPERATION_CODE, double, double, DWORD,
                        void *);
void __cdecl __std_type_info_destroy_list(SLIST_HEADER *);

__declspec(dllexport) double UseCRTException(DWORD Flags, double Value,
                                             double Result, void *Context) {
  return _except1(Flags, _FpCodeAdd, Value, Result, 0, Context);
}

__declspec(dllexport) void UseCRTTypeCleanup(SLIST_HEADER *Head) {
  __std_type_info_destroy_list(Head);
}

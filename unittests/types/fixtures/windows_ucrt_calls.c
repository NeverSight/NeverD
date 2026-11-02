// Original public fixture: compile with Microsoft's UCRT headers and link
// ucrt.lib or ucrt.osmode_permissive.lib for each Windows target.
#include <complex.h>
#include <io.h>
#include <math.h>
#include <process.h>
#include <wchar.h>

// A linker marker for floating-point use; these import-only DLLs have no CRT
// startup object, and the marker supplies no function implementation.
int _fltused;

__declspec(dllexport) double UseCRTMath(double X, double Y) {
  return _hypot(X, Y);
}

__declspec(dllexport) float UseCRTMathFloat(float X, float Y) {
  return _hypotf(X, Y);
}

__declspec(dllexport) _Dcomplex UseCRTComplex(_Dcomplex X, _Dcomplex Y) {
  return cpow(X, Y);
}

__declspec(dllexport) double UseCRTComplexMagnitude(_Dcomplex X) {
  return cabs(X);
}

__declspec(dllexport) int UseCRTUnicode(const wchar_t *Left,
                                        const wchar_t *Right) {
  return _wcsicmp(Left, Right);
}

__declspec(dllexport) int UseCRTVarargs(const wchar_t *Path, int Mode) {
  return _wopen(Path, 0x100, Mode);
}

__declspec(dllexport) uintptr_t UseCRTThread(unsigned(__stdcall *Start)(void *),
                                             void *Argument,
                                             unsigned *ThreadId) {
  return _beginthreadex(0, 0, Start, Argument, 0, ThreadId);
}

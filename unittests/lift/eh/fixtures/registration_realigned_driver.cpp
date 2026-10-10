// Runtime oracle for the separately generated realigned catch frame.
extern "C" __declspec(dllimport) __declspec(noreturn) void __stdcall
ExitProcess(unsigned);
extern "C" unsigned long __readfsdword(unsigned long);
#pragma intrinsic(__readfsdword)

extern "C" __declspec(dllexport) __declspec(noinline) void callback_throw() {
  throw 7;
}
extern "C" int callback_parent();
extern "C" __declspec(dllexport) __declspec(noinline) void
    __attribute__((thiscall))
    callback_increment(int *Object) noexcept {
  *Object += 5;
}

template <unsigned Padding> __declspec(noinline) int call_with_padding() {
  volatile unsigned char Space[Padding];
  for (unsigned I = 0; I != Padding; ++I)
    Space[I] = static_cast<unsigned char>(I);
  const auto Chain = __readfsdword(0);
  const int Value = callback_parent();
  for (unsigned I = 0; I != Padding; ++I)
    if (Space[I] != I)
      return -1;
  return __readfsdword(0) == Chain ? Value : -1;
}

#ifndef EXPECTED_RESULT
#define EXPECTED_RESULT 7
#endif

extern "C" __declspec(noreturn) void mainCRTStartup() {
  const int Results[] = {call_with_padding<4>(), call_with_padding<20>(),
                         call_with_padding<36>(), call_with_padding<52>()};
  for (int Value : Results)
    if (Value != EXPECTED_RESULT)
      ExitProcess(1);
  ExitProcess(0);
}

// Runtime oracle for source reconstruction of aligned value/reference catches.
extern "C" __declspec(dllimport) __declspec(noreturn) void __stdcall
ExitProcess(unsigned);
extern "C" __declspec(dllimport) void *__stdcall GetStdHandle(unsigned);
extern "C" __declspec(dllimport) int __stdcall
WriteFile(void *, const void *, unsigned, unsigned *, void *);
extern "C" unsigned long __readfsdword(unsigned long);
extern "C" void *_ReturnAddress();
#pragma intrinsic(__readfsdword)
#pragma intrinsic(_ReturnAddress)

extern "C" {
__declspec(dllexport) volatile unsigned callback_caller = 0;
__declspec(dllexport) volatile unsigned callback_caught = 0;
__declspec(dllexport) __declspec(noinline) void callback_throw() {
  callback_caller = reinterpret_cast<unsigned>(_ReturnAddress());
  throw 7;
}
int callback_parent();
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
#ifndef REFERENCE_CATCH
#define REFERENCE_CATCH 0
#endif

extern "C" __declspec(noreturn) void mainCRTStartup() {
  bool Passed = true;
  unsigned Value = 0, Caught = 0, Iterations = 0;
  const auto Chain = __readfsdword(0);
  for (unsigned I = 0; I != 4; ++I) {
    callback_caught = 0;
    switch (I) {
    case 0:
      Value = call_with_padding<4>();
      break;
    case 1:
      Value = call_with_padding<20>();
      break;
    case 2:
      Value = call_with_padding<36>();
      break;
    case 3:
      Value = call_with_padding<52>();
      break;
    }
    Caught = callback_caught;
    Passed &= Value == EXPECTED_RESULT && Caught == (REFERENCE_CATCH ? 18 : 7);
    ++Iterations;
  }
  const unsigned Restored = __readfsdword(0) == Chain;
  char Message[] = "CALLBACK 00000000 00000000 00000000 00000000 00000000\n";
  const unsigned Values[] = {Value, Caught, Restored, Iterations,
                             callback_caller};
  for (unsigned I = 0; I != 5; ++I)
    for (unsigned J = 0; J != 8; ++J)
      Message[9 + I * 9 + J] =
          "0123456789ABCDEF"[(Values[I] >> ((7 - J) * 4)) & 15];
  unsigned Written = 0;
  WriteFile(GetStdHandle(unsigned(-11)), Message, sizeof(Message) - 1, &Written,
            nullptr);
  ExitProcess(Passed && Restored && Written == sizeof(Message) - 1 ? 0 : 1);
}

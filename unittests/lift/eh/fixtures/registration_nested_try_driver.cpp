// Runtime oracle for nested value, reference and catch-all reconstruction.
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
__declspec(dllexport) volatile unsigned callback_choice = 0;
__declspec(dllexport) __declspec(noinline) void callback_throw_int() {
  callback_caller = reinterpret_cast<unsigned>(_ReturnAddress());
  throw 7;
}
__declspec(dllexport) __declspec(noinline) void callback_throw_unsigned() {
  callback_caller = reinterpret_cast<unsigned>(_ReturnAddress());
  throw 7u;
}
__declspec(dllexport) __declspec(noinline) void callback_throw_float() {
  callback_caller = reinterpret_cast<unsigned>(_ReturnAddress());
  throw 7.0f;
}
__declspec(dllexport) int callback_parent();
}

extern "C" __declspec(dllexport) __declspec(noinline) int callback_parent() {
  try {
    try {
      if (callback_choice == 0)
        callback_throw_int();
      if (callback_choice == 1)
        callback_throw_unsigned();
      callback_throw_float();
    } catch (unsigned &Value) {
      Value += 11;
      callback_caught = Value;
      return Value + 10;
    }
  } catch (int Value) {
    callback_caught = Value;
    return Value + 10;
  } catch (...) {
    callback_caught = 39;
    return 39;
  }
  return -1;
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

#ifndef EXPECTED_FIRST
#define EXPECTED_FIRST 17
#endif
extern "C" __declspec(noreturn) void mainCRTStartup() {
  bool Passed = true;
  unsigned Values[3] = {}, Caught[3] = {}, Callers[3] = {}, Iterations = 0;
  const unsigned Expected[] = {EXPECTED_FIRST, 28, 39};
  const unsigned ExpectedCaught[] = {7, 18, 39};
  const auto Chain = __readfsdword(0);
  for (unsigned Round = 0; Round != 4; ++Round)
    for (unsigned Choice = 0; Choice != 3; ++Choice) {
      callback_choice = Choice;
      callback_caught = 0;
      switch (Round) {
      case 0:
        Values[Choice] = call_with_padding<4>();
        break;
      case 1:
        Values[Choice] = call_with_padding<20>();
        break;
      case 2:
        Values[Choice] = call_with_padding<36>();
        break;
      case 3:
        Values[Choice] = call_with_padding<52>();
        break;
      }
      Caught[Choice] = callback_caught;
      Callers[Choice] = callback_caller;
      Passed &= Values[Choice] == Expected[Choice] &&
                Caught[Choice] == ExpectedCaught[Choice];
      ++Iterations;
    }
  const unsigned Restored = __readfsdword(0) == Chain;
  char Message[] = "MULTICATCH 00000000 00000000 00000000 00000000 00000000 "
                   "00000000 00000000 00000000 00000000 00000000 00000000\n";
  const unsigned Results[] = {Values[0],  Values[1],  Values[2], Caught[0],
                              Caught[1],  Caught[2],  Restored,  Iterations,
                              Callers[0], Callers[1], Callers[2]};
  for (unsigned I = 0; I != 11; ++I)
    for (unsigned J = 0; J != 8; ++J)
      Message[11 + I * 9 + J] =
          "0123456789ABCDEF"[(Results[I] >> ((7 - J) * 4)) & 15];
  unsigned Written = 0;
  WriteFile(GetStdHandle(unsigned(-11)), Message, sizeof(Message) - 1, &Written,
            nullptr);
  ExitProcess(Passed && Restored && Written == sizeof(Message) - 1 ? 0 : 1);
}

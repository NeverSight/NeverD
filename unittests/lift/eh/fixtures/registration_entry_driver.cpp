// Runtime oracle for cdecl, stdcall, thiscall and fastcall C++ parent entries.
extern "C" __declspec(dllimport) __declspec(noreturn) void __stdcall
ExitProcess(unsigned);
extern "C" __declspec(dllimport) void *__stdcall GetStdHandle(unsigned);
extern "C" __declspec(dllimport) int __stdcall
WriteFile(void *, const void *, unsigned, unsigned *, void *);
extern "C" unsigned long __readfsdword(unsigned long);
extern "C" void *_ReturnAddress();
#pragma intrinsic(__readfsdword)
#pragma intrinsic(_ReturnAddress)

#ifndef ENTRY_ABI
#define ENTRY_ABI __cdecl
#endif
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
__declspec(dllexport) __declspec(noinline) unsigned ENTRY_ABI
callback_parent(unsigned A, unsigned B, unsigned C, unsigned D) {
  try {
    try {
      if (callback_choice == 0)
        callback_throw_int();
      if (callback_choice == 1)
        callback_throw_unsigned();
      if (callback_choice == 2)
        callback_throw_float();
      callback_caught = 49;
      return 49 + A + 3 * B + 5 * C + 7 * D;
    } catch (unsigned &Value) {
      Value += 11;
      callback_caught = Value;
      return Value + 10 + A + 3 * B + 5 * C + 7 * D;
    }
  } catch (int Value) {
    callback_caught = Value;
    return Value + 10 + A + 3 * B + 5 * C + 7 * D;
  } catch (...) {
    callback_caught = 39;
    return 39 + A + 3 * B + 5 * C + 7 * D;
  }
}
}

template <unsigned Padding>
__declspec(noinline) unsigned call_with_padding(unsigned Seed,
                                                unsigned &Balanced) {
  volatile unsigned char Space[Padding];
  for (unsigned I = 0; I != Padding; ++I)
    Space[I] = static_cast<unsigned char>(I);
  const auto Chain = __readfsdword(0);
  unsigned Before = 0, After = 0;
  __asm { mov Before, esp }
  const unsigned Value =
      callback_parent(Seed + 2, Seed + 4, Seed + 6, Seed + 8);
  __asm { mov After, esp }
  Balanced += Before == After;
  for (unsigned I = 0; I != Padding; ++I)
    if (Space[I] != I)
      return ~0u;
  return __readfsdword(0) == Chain && Before == After ? Value : ~0u;
}

extern "C" __declspec(noreturn) void mainCRTStartup() {
  bool Passed = true;
  unsigned Values[4] = {}, Caught[4] = {}, Callers[3] = {};
  unsigned Iterations = 0, Balanced = 0;
  const unsigned Expected[] = {17, 28, 39, 49};
  const unsigned ExpectedCaught[] = {7, 18, 39, 49};
  const auto Chain = __readfsdword(0);
  for (unsigned Round = 0; Round != 4; ++Round)
    for (unsigned Choice = 0; Choice != 4; ++Choice) {
      const unsigned Seed = Round * 4 + Choice;
      callback_choice = Choice;
      callback_caught = 0;
      switch (Round) {
      case 0:
        Values[Choice] = call_with_padding<4>(Seed, Balanced);
        break;
      case 1:
        Values[Choice] = call_with_padding<20>(Seed, Balanced);
        break;
      case 2:
        Values[Choice] = call_with_padding<36>(Seed, Balanced);
        break;
      case 3:
        Values[Choice] = call_with_padding<52>(Seed, Balanced);
        break;
      }
      Caught[Choice] = callback_caught;
      if (Choice < 3)
        Callers[Choice] = callback_caller;
#ifndef EXPECTED_BIAS
#define EXPECTED_BIAS 0
#endif
      Passed &= Values[Choice] ==
                    Expected[Choice] + 100 + 16 * Seed + EXPECTED_BIAS &&
                Caught[Choice] == ExpectedCaught[Choice];
      ++Iterations;
    }
  const unsigned Restored = __readfsdword(0) == Chain;
  char Message[] = "ENTRY 00000000 00000000 00000000 00000000 00000000 "
                   "00000000 00000000 00000000 00000000 00000000 00000000 "
                   "00000000 00000000 00000000\n";
  const unsigned Results[] = {Values[0],  Values[1],  Values[2], Values[3],
                              Caught[0],  Caught[1],  Caught[2], Caught[3],
                              Restored,   Iterations, Balanced,  Callers[0],
                              Callers[1], Callers[2]};
  for (unsigned I = 0; I != 14; ++I)
    for (unsigned J = 0; J != 8; ++J)
      Message[6 + I * 9 + J] =
          "0123456789ABCDEF"[(Results[I] >> ((7 - J) * 4)) & 15];
  unsigned Written = 0;
  WriteFile(GetStdHandle(unsigned(-11)), Message, sizeof(Message) - 1, &Written,
            nullptr);
  ExitProcess(Passed && Restored && Balanced == 16 && Iterations == 16 ? 0 : 1);
}

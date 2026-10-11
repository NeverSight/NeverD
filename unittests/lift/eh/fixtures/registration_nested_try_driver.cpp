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
#ifdef CATCH_CLEANUP
__declspec(dllexport) volatile unsigned callback_cleanup = 0;
#endif
#ifdef DIRECT_TYPED_THROW
__declspec(dllexport) __declspec(noinline) int callback_mark() {
  callback_caller = reinterpret_cast<unsigned>(_ReturnAddress());
  return 7;
}
#endif
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
#if defined(RETHROW_SEARCH) && !defined(INLINE_RETHROW_SEARCH)
__declspec(dllexport) __declspec(noinline) void callback_rethrow() {
  callback_caller = reinterpret_cast<unsigned>(_ReturnAddress());
  throw;
}
#endif
__declspec(dllexport) int callback_parent();
}

#ifdef CATCH_CLEANUP
struct CleanupProbe {
  unsigned Value;
  __declspec(noinline) ~CleanupProbe() {
    callback_cleanup = callback_cleanup * 10 + Value;
  }
};
#endif

extern "C" __declspec(dllexport) __declspec(noinline) int callback_parent() {
  try {
    try {
#ifdef DIRECT_TYPED_THROW
      if (callback_choice == 0) {
        callback_mark();
        throw 7;
      }
      if (callback_choice == 1) {
        callback_mark();
        throw 7u;
      }
      callback_mark();
      throw 7.0f;
#else
      if (callback_choice == 0)
        callback_throw_int();
      if (callback_choice == 1)
        callback_throw_unsigned();
      callback_throw_float();
#endif
    } catch (unsigned &Value) {
#ifdef RETHROW_SEARCH
      if (Value == 7) {
        Value += 11;
#ifdef INLINE_RETHROW_SEARCH
        throw;
#else
        callback_rethrow();
#endif
      }
#elif defined(SECONDARY_SEARCH)
      if (Value == 7) {
#ifdef DIRECT_TYPED_THROW
        callback_mark();
        throw 7;
#else
        callback_throw_int();
#endif
      }
#endif
#ifdef CATCH_TRY
      try {
#ifdef CATCH_CLEANUP
        CleanupProbe First{3};
        CleanupProbe Second{5};
#endif
        callback_throw_int();
      } catch (int Inner) {
        Value += Inner + 4;
      }
#else
      Value += 11;
#endif
      callback_caught = Value;
      return Value + 10;
    }
#ifdef RETHROW_SEARCH
  } catch (unsigned &Value) {
#else
  } catch (int Value) {
#endif
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
#ifdef RETHROW_SEARCH
#define EXPECTED_FIRST 39
#else
#define EXPECTED_FIRST 17
#endif
#endif
extern "C" __declspec(noreturn) void mainCRTStartup() {
  bool Passed = true;
  unsigned Values[3] = {}, Caught[3] = {}, Callers[3] = {}, Iterations = 0;
#ifdef CATCH_CLEANUP
  unsigned Cleanups[3] = {};
#endif
#ifdef RETHROW_SEARCH
  const unsigned Expected[] = {EXPECTED_FIRST, 28, 39};
  const unsigned ExpectedCaught[] = {39, 18, 39};
#elif defined(SECONDARY_SEARCH)
  const unsigned Expected[] = {EXPECTED_FIRST, 17, 39};
  const unsigned ExpectedCaught[] = {7, 7, 39};
#else
  const unsigned Expected[] = {EXPECTED_FIRST, 28, 39};
  const unsigned ExpectedCaught[] = {7, 18, 39};
#endif
  const auto Chain = __readfsdword(0);
  for (unsigned Round = 0; Round != 4; ++Round)
    for (unsigned Choice = 0; Choice != 3; ++Choice) {
      callback_choice = Choice;
      callback_caught = 0;
#ifdef CATCH_CLEANUP
      callback_cleanup = 0;
#endif
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
#ifdef CATCH_CLEANUP
#ifndef EXPECTED_CLEANUP
#define EXPECTED_CLEANUP 53
#endif
      Cleanups[Choice] = callback_cleanup;
      Passed &= callback_cleanup == (Choice == 1 ? EXPECTED_CLEANUP : 0);
#endif
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
#ifdef CATCH_CLEANUP
  char CleanupMessage[] = "CLEANUP 00000000 00000000 00000000\n";
  for (unsigned I = 0; I != 3; ++I)
    for (unsigned J = 0; J != 8; ++J)
      CleanupMessage[8 + I * 9 + J] =
          "0123456789ABCDEF"[(Cleanups[I] >> ((7 - J) * 4)) & 15];
  unsigned CleanupWritten = 0;
  WriteFile(GetStdHandle(unsigned(-11)), CleanupMessage,
            sizeof(CleanupMessage) - 1, &CleanupWritten, nullptr);
  Passed &= CleanupWritten == sizeof(CleanupMessage) - 1;
#endif
  ExitProcess(Passed && Restored && Written == sizeof(Message) - 1 ? 0 : 1);
}

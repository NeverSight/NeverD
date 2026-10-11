// PE32 trivial object copies, references and rethrows with real CRT dispatch.
extern "C" __declspec(dllimport) __declspec(noreturn) void __stdcall
ExitProcess(unsigned);
extern "C" __declspec(dllimport) void *__stdcall GetStdHandle(unsigned);
extern "C" __declspec(dllimport) int __stdcall
WriteFile(void *, const void *, unsigned, unsigned *, void *);
extern "C" unsigned long __readfsdword(unsigned long);
extern "C" void *_ReturnAddress();
#pragma intrinsic(__readfsdword)
#pragma intrinsic(_ReturnAddress)

#include "registration_object_types.h"

extern "C" {
__declspec(dllexport) volatile unsigned callback_caller = 0;
__declspec(dllexport) volatile unsigned callback_caught = 0;
__declspec(dllexport) volatile unsigned callback_choice = 0;
__declspec(dllexport) __declspec(noinline) void callback_throw_value() {
  callback_caller = reinterpret_cast<unsigned>(_ReturnAddress());
  throw ValueObject{7, 13};
}
__declspec(dllexport) __declspec(noinline) void callback_throw_reference() {
  callback_caller = reinterpret_cast<unsigned>(_ReturnAddress());
  throw ReferenceObject{7, 11, 13};
}
__declspec(dllexport) __declspec(noinline) void callback_throw_float() {
  callback_caller = reinterpret_cast<unsigned>(_ReturnAddress());
  throw 7.0f;
}
#ifdef DIRECT_TYPED_THROW
__declspec(dllexport) __declspec(noinline) int callback_mark() {
  callback_caller = reinterpret_cast<unsigned>(_ReturnAddress());
  return 7;
}
#elif defined(RETHROW_SEARCH)
__declspec(dllexport) __declspec(noinline) void callback_rethrow() {
  callback_caller = reinterpret_cast<unsigned>(_ReturnAddress());
  throw;
}
#endif

__declspec(dllexport) __declspec(noinline) int callback_parent() {
  try {
    try {
#ifdef DIRECT_TYPED_THROW
      if (callback_choice == 0) {
        callback_mark();
        throw ValueObject{7, 13};
      }
      if (callback_choice == 1) {
        callback_mark();
        throw ReferenceObject{7, 11, 13};
      }
      callback_mark();
      throw 7.0f;
#else
      if (callback_choice == 0)
        callback_throw_value();
      if (callback_choice == 1)
        callback_throw_reference();
      callback_throw_float();
#endif
    } catch (ReferenceObject &Object) {
      if (Object.Tail != 11 || Object.Tag != 13)
        return -1;
      Object.Head += 11;
      Object.Tail += 3;
#ifdef RETHROW_SEARCH
      if (Object.Head == 18) {
#ifdef DIRECT_TYPED_THROW
        throw;
#else
        callback_rethrow();
#endif
      }
#endif
      callback_caught = Object.Head;
      return Object.Head + Object.Tail - 4;
    }
#ifdef RETHROW_SEARCH
  } catch (ReferenceObject Object) {
    if (Object.Tail != 14 || Object.Tag != 13)
      return -2;
    Object.Tag += 4;
    callback_caught = Object.Head;
    return Object.Head + Object.Tail + Object.Tag - 21;
#else
  } catch (ValueObject Object) {
    if (Object.Tail != 13)
      return -3;
    Object.Tail += 4;
    callback_caught = Object.Head;
    return Object.Head + Object.Tail - 7;
#endif
  } catch (...) {
    callback_caught = 39;
    return 39;
  }
  return -1;
}
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
  const unsigned Expected[] = {EXPECTED_FIRST, 28, 39};
#ifdef RETHROW_SEARCH
  const unsigned ExpectedCaught[] = {39, 18, 39};
#else
  const unsigned ExpectedCaught[] = {7, 18, 39};
#endif
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

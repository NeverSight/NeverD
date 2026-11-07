//===- registration_mixed_observer.cpp - Mixed PE32 runtime checks -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Observe both reconstructed parents using the native x86 MSVC runtime.
//===----------------------------------------------------------------------===//

extern "C" {
__declspec(dllimport) __declspec(noreturn) void __stdcall ExitProcess(unsigned);
extern volatile unsigned cleanup_only_trace;
extern volatile unsigned cleanup_only_enabled;
int cleanup_only_parent();
int mixed_seh();
}

static unsigned registrationChain() {
  unsigned Chain;
  __asm__ volatile("movl %%fs:0, %0" : "=r"(Chain));
  return Chain;
}

extern "C" __declspec(noreturn) void mainCRTStartup() {
  const unsigned Chain = registrationChain();
  volatile unsigned StackGuard[2] = {0x12345678, 0xabcdef01};
  if (mixed_seh() != 31 || registrationChain() != Chain ||
      StackGuard[0] != 0x12345678 || StackGuard[1] != 0xabcdef01)
    ExitProcess(4);
  const unsigned Before = cleanup_only_trace;
  cleanup_only_enabled = 0;
  if (cleanup_only_parent() != 0 || cleanup_only_trace != Before)
    ExitProcess(3);
  cleanup_only_enabled = 1;
  unsigned Result = 2;
  try {
    cleanup_only_parent();
  } catch (int Value) {
    Result = Value == 7 && cleanup_only_trace == 21 ? 0 : 1;
  }
  // The CRT owns an extra registration while its catch callback is active.
  // Observe restoration only after leaving that callback.
  if (registrationChain() != Chain || StackGuard[0] != 0x12345678 ||
      StackGuard[1] != 0xabcdef01)
    ExitProcess(5);
  ExitProcess(Result);
}

// Native support module for actual C SEH and cross-image unwind execution.
typedef unsigned int U32;
typedef unsigned long long U64;
__declspec(dllimport) void ExRaiseStatus(U32);
#define EXPORT __declspec(dllexport)
EXPORT volatile U64 ModuleSEHCount;
static volatile U64 *volatile Count = &ModuleSEHCount;
EXPORT __declspec(noinline) void ModuleSEHThrow(U64 Value) {
  volatile U64 Local[4] = {Value, 2, 3, 4};
  __asm__ volatile("movabsq $0x1122334455667788, %%rbx\n\t"
                   "movabsq $0x8877665544332211, %%r12" ::
                       : "rbx", "r12");
  ExRaiseStatus(0xc000009a);
  Local[0] = 0xbad;
}
EXPORT U64 ModuleSEHCatch(U64 Value) {
  volatile U64 Local = Value + 3;
  ++*Count;
  __try {
    ModuleSEHThrow(Value);
    Local = 0xbad;
  } __except (1) {
    return Local + 0x40;
  }
  return 0;
}
EXPORT U64 ModuleSEHCall(U64 Target, U64 Value) {
  volatile U64 Local = Value + 5;
  ++*Count;
  __try {
    ((void (*)(U64))Target)(Value);
    Local = 0xbad;
  } __except (1) {
    return Local + 0x80;
  }
  return 0;
}

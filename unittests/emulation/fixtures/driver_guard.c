//===- driver_guard.c - Original AMD64 CFG metadata and calls -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original guest fallback code and the documented PE32+ guard layout. Build
/// with -Xclang -cfguard and link with /guard:cf for enforcement; omit the link
/// flag to exercise an instrumented library's dormant compatibility slots.
/// https://learn.microsoft.com/windows/win32/secbp/pe-metadata
///
//===----------------------------------------------------------------------===//

typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;

// Declaring these assembly labels as data deliberately excludes the guard
// fallbacks themselves from GFIDS, as required by the public PE contract.
extern const U8 NeverDGuardCheck[];
extern const U8 NeverDGuardDispatch[];
__asm__(".text\n.p2align 4\n.globl NeverDGuardCheck\n"
        "NeverDGuardCheck:\nretq\n.p2align 4\n"
        ".globl NeverDGuardDispatch\nNeverDGuardDispatch:\njmpq *%rax\n");
const void *const __guard_check_icall_fptr = NeverDGuardCheck;
const void *const __guard_dispatch_icall_fptr = NeverDGuardDispatch;
__attribute__((used)) static const void *const XfgCheck = NeverDGuardCheck;
__attribute__((used)) static const void *const XfgDispatch =
    NeverDGuardDispatch;
__attribute__((used)) static const void *const XfgTableDispatch =
    NeverDGuardDispatch;
__attribute__((used)) static const U64 CastGuard = 0;
__attribute__((used)) static const U64 UmaPointers = 0;
extern const U8 __guard_fids_table[];
extern const U8 __guard_fids_count[];
extern const U8 __guard_flags[];
extern const U8 __guard_iat_table[];
extern const U8 __guard_iat_count[];

struct LOAD_CONFIG_GUARD {
  U32 Size;
  U8 BeforeGuard[108];
  const void *Check;
  const void *Dispatch;
  const void *Functions;
  U64 FunctionCount;
  U32 Flags;
  U8 CodeIntegrity[12];
  const void *AddressTakenIat;
  U64 AddressTakenIatCount;
  U8 BeforeXfg[104];
  const void *XfgCheck;
  const void *XfgDispatch;
  const void *XfgTableDispatch;
  const void *CastGuard;
  const void *GuardMemcpy;
  const void *UmaPointers;
};
_Static_assert(__builtin_offsetof(struct LOAD_CONFIG_GUARD, Check) == 0x70,
               "PE32+ check slot field");
_Static_assert(__builtin_offsetof(struct LOAD_CONFIG_GUARD, XfgCheck) == 0x118,
               "PE32+ XFG slot field");
_Static_assert(sizeof(struct LOAD_CONFIG_GUARD) == 0x148,
               "PE32+ current guard layout");
// Assembly preserves absolute linker-defined integer symbols without C's
// pointer-to-32-bit constant-expression restriction.
__asm__(".section .rdata,\"dr\"\n.p2align 3\n.globl _load_config_used\n"
        "_load_config_used:\n.long 0x148\n.zero 108\n"
        ".quad __guard_check_icall_fptr\n.quad __guard_dispatch_icall_fptr\n"
        ".quad __guard_fids_table\n.quad __guard_fids_count\n"
        ".long __guard_flags\n.zero 12\n"
        ".quad __guard_iat_table\n.quad __guard_iat_count\n.zero 104\n"
        ".quad XfgCheck\n.quad XfgDispatch\n.quad XfgTableDispatch\n"
        ".quad CastGuard\n.quad 0\n.quad UmaPointers\n");
int _fltused = 0;

__declspec(noinline) static U64 Target(double A, U64 B, U64 C, U64 D, U64 E,
                                       double F) {
  return (U64)A + B * 2 + C * 3 + D * 4 + E * 5 + (U64)F;
}
static U64 (*volatile TargetPointer)(double, U64, U64, U64, U64,
                                     double) = Target;

int DriverEntry(void *Driver, void *Registry) {
  if (!Driver || !Registry)
    return (int)0xc000000dU;
  U64 (*Selected)(double, U64, U64, U64, U64, double) = TargetPointer;
#ifdef NEVERD_GUARD_INVALID_TARGET
  Selected = (void *)((U64)Selected + 1);
#endif
  // Exercise the separately callable check ABI as well as the compiler's
  // dispatch sequence. Neither helper may consume the target call's stack.
  __asm__ volatile("callq *__guard_check_icall_fptr(%%rip)"
                   :
                   : "c"(Selected)
                   : "memory");
#ifdef NEVERD_GUARD_INVALID_DISPATCH
  Selected = (void *)((U64)Selected + 1);
#endif
  return Selected(11.0, 13, 17, 19, 23, 29.0) == 308 ? 0 : (int)0xc000000dU;
}

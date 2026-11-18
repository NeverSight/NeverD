//===- test_patch_coff_guard.c - x64 Control Flow Guard call fixture ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Compiled with -Xclang -cfguard, so every indirect call and indirect tail
/// jump goes through the load configuration's dispatch slot with the target
/// in RAX.  Linked without /guard:cf the image is CF_INSTRUMENTED only, as an
/// MSVC image whose objects were compiled with /guard:cf and linked without
/// it is; linked with it the loader enforces the guard.  The slots keep this
/// file's own fallbacks, a check that returns and a dispatcher that jumps to
/// RAX, laid out as the public PE contract requires.
/// https://learn.microsoft.com/windows/win32/secbp/pe-metadata
///
//===----------------------------------------------------------------------===//

typedef unsigned char U8;

// Declared as data so the fallbacks themselves stay out of GFIDS.
extern const U8 NeverDGuardCheck[];
extern const U8 NeverDGuardDispatch[];
__asm__(".text\n.p2align 4\n.globl NeverDGuardCheck\n"
        "NeverDGuardCheck:\nretq\n.p2align 4\n"
        ".globl NeverDGuardDispatch\nNeverDGuardDispatch:\njmpq *%rax\n");
const void *const __guard_check_icall_fptr = NeverDGuardCheck;
const void *const __guard_dispatch_icall_fptr = NeverDGuardDispatch;
extern const U8 __guard_fids_table[];
extern const U8 __guard_fids_count[];
extern const U8 __guard_flags[];
extern const U8 __guard_iat_table[];
extern const U8 __guard_iat_count[];
// The PE32+ load configuration up to its guard fields; assembly keeps the
// linker's absolute integer symbols, which C cannot narrow to 32 bits.
__asm__(".section .rdata,\"dr\"\n.p2align 3\n.globl _load_config_used\n"
        "_load_config_used:\n.long 0x148\n.zero 108\n"
        ".quad __guard_check_icall_fptr\n.quad __guard_dispatch_icall_fptr\n"
        ".quad __guard_fids_table\n.quad __guard_fids_count\n"
        ".long __guard_flags\n.zero 12\n"
        ".quad __guard_iat_table\n.quad __guard_iat_count\n.zero 104\n"
        ".quad 0\n.quad 0\n.quad 0\n.quad 0\n.quad 0\n.quad 0\n");

__declspec(noinline) static int Twice(int X) { return X * 2; }
int (*volatile Target)(int) = Twice;

/// `mov rax, [Target]; add ecx, 3; call [__guard_dispatch_icall_fptr]`.
__declspec(noinline) int CallThrough(int X) { return Target(X + 3) + 1; }

/// `mov rax, [Target]; dec ecx; jmp [__guard_dispatch_icall_fptr]`.
__declspec(noinline) int TailThrough(int X) { return Target(X - 1); }

int mainCRTStartup(void) { return CallThrough(20) + TailThrough(5) != 55; }

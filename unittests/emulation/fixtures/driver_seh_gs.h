//===- driver_seh_gs.h - Original GS exception frames ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original assembly fixes the cookie slot and alignment so the negative test
/// changes only the cookie. Both forms first execute the WDK runtime's cookie
/// checker normally, then raise through its linked SEH personality.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DRIVER_SEH_GS_H
#define NEVERD_DRIVER_SEH_GS_H

#include <ntifs.h>
#include <ntimage.h>

#define SEH_GS_FRAME_REGISTER 5
#define SEH_GS_FRAME_OFFSET 64
#define SEH_GS_STACK_SIZE 256
#define SEH_GS_COOKIE_OFFSET 48
#define SEH_GS_ARGUMENT_OFFSET 80
#define SEH_GS_ALIGNMENT 64
#define SEH_GS_STRING_IMPL(Value) #Value
#define SEH_GS_STRING(Value) SEH_GS_STRING_IMPL(Value)

// The WDK exposes the runtime-function entry through ntimage.h but leaves
// DISPATCHER_CONTEXT opaque. These checkers read only this ABI prefix.
typedef struct {
  ULONG64 ControlPc;
  ULONG64 ImageBase;
  PIMAGE_RUNTIME_FUNCTION_ENTRY FunctionEntry;
  ULONG64 EstablisherFrame;
  ULONG64 TargetIp;
  PCONTEXT ContextRecord;
  PVOID LanguageHandler;
  PVOID HandlerData;
} SEH_GS_DISPATCHER_PREFIX;

C_ASSERT(FIELD_OFFSET(SEH_GS_DISPATCHER_PREFIX, ImageBase) == 8);
C_ASSERT(FIELD_OFFSET(SEH_GS_DISPATCHER_PREFIX, FunctionEntry) == 16);
C_ASSERT(FIELD_OFFSET(SEH_GS_DISPATCHER_PREFIX, HandlerData) == 56);

VOID __GSHandlerCheckCommon(PVOID Frame, SEH_GS_DISPATCHER_PREFIX *Context,
                            PVOID CookieData);
EXCEPTION_DISPOSITION __GSHandlerCheck(PEXCEPTION_RECORD Record, PVOID Frame,
                                       PCONTEXT Registers,
                                       SEH_GS_DISPATCHER_PREFIX *Context);

// Link this fixture with /debug:dwarf to retain exact COFF runtime identities.
// Cookie data alone cannot identify an arbitrary stripped personality.

__attribute__((used)) __declspec(noinline) static VOID
SehGSVerifyRuntime(PVOID Frame, PVOID CookieData, BOOLEAN Standalone) {
  // Only the frame-register byte is used by this helper. Its two pointer
  // inputs remain separate, as they are in an actual dispatcher context.
  const UCHAR Unwind[] = {1, 0, 0, SEH_GS_FRAME_OFFSET | SEH_GS_FRAME_REGISTER};
  IMAGE_RUNTIME_FUNCTION_ENTRY Function = {0};
  SEH_GS_DISPATCHER_PREFIX Context = {0};
  Context.ImageBase = (ULONG64)Unwind;
  Context.FunctionEntry = &Function;
  Context.HandlerData = CookieData;
  if (Standalone) {
    if (__GSHandlerCheck(NULL, Frame, NULL, &Context) !=
        ExceptionContinueSearch)
      __debugbreak();
  } else {
    __GSHandlerCheckCommon(Frame, &Context, CookieData);
  }
}

NTSTATUS SehGSFrame(BOOLEAN Corrupt);
NTSTATUS SehGSAlignedFrame(BOOLEAN Corrupt);
NTSTATUS SehGSStandaloneFrame(BOOLEAN Corrupt);
NTSTATUS SehGSStandaloneAlignedFrame(BOOLEAN Corrupt);

#define SEH_GS_FUNCTION(Name, Align, CookieFlags, Personality, Standalone)     \
  ".text\n.p2align 4\n.globl " #Name "\n" #Name ":\n"                          \
  ".seh_proc " #Name "\n"                                                      \
  "pushq %rbp\n.seh_pushreg %rbp\n"                                            \
  "subq $SehGSStackSize, %rsp\n.seh_stackalloc SehGSStackSize\n"               \
  "leaq SehGSFrameOffset(%rsp), %rbp\n"                                        \
  ".seh_setframe %rbp, SehGSFrameOffset\n.seh_endprologue\n"                   \
  "movb %cl, SehGSArgumentOffset(%rbp)\n"                                      \
  ".if " #Align "\nandq $-SehGSAlignment, %rsp\n.endif\n"                      \
  "movq __security_cookie(%rip), %rax\nxorq %rbp, %rax\n"                      \
  "movq %rax, SehGSCookieOffset(%rsp)\n"                                       \
  "leaq -SehGSFrameOffset(%rbp), %rcx\n"                                       \
  "leaq .L" #Name "CookieData(%rip), %rdx\n"                                   \
  "movl $" #Standalone ", %r8d\n"                                              \
  "callq SehGSVerifyRuntime\n"                                                 \
  "cmpb $0, SehGSArgumentOffset(%rbp)\nje .L" #Name "Raise\n"                  \
  "xorq $1, SehGSCookieOffset(%rsp)\n"                                         \
  ".L" #Name "Raise:\ncallq *__imp_ExRaiseAccessViolation(%rip)\n"             \
  ".L" #Name "AfterRaise:\nud2\n"                                              \
  ".L" #Name "Handler:\n"                                                      \
  "cmpl $0xc0000005, %eax\njne .L" #Name "Failure\n"                           \
  "leaq -SehGSFrameOffset(%rbp), %rax\n"                                       \
  ".if " #Align "\nandq $-SehGSAlignment, %rax\n.endif\n"                      \
  "movq SehGSCookieOffset(%rax), %rcx\nxorq %rbp, %rcx\n"                      \
  "callq __security_check_cookie\nxorl %eax, %eax\n"                           \
  "jmp .L" #Name "Return\n"                                                    \
  ".L" #Name "Failure:\nmovl $0xc0000001, %eax\n"                              \
  ".L" #Name "Return:\n"                                                       \
  "leaq SehGSStackSize-SehGSFrameOffset(%rbp), %rsp\npopq %rbp\nretq\n"        \
  ".seh_handler " #Personality ", @except, @unwind\n"                          \
  ".seh_handlerdata\n.if !" #Standalone "\n.long 1\n"                          \
  ".rva .L" #Name "Raise, .L" #Name "AfterRaise\n"                             \
  ".long 1\n.rva .L" #Name "Handler\n"                                         \
  ".endif\n"                                                                   \
  ".L" #Name "CookieData:\n"                                                   \
  ".long SehGSCookieOffset | " #CookieFlags "\n"                               \
  ".if " #Align "\n.long 0, SehGSAlignment\n.endif\n"                          \
  ".text\n.seh_endproc\n"

#define SEH_GS_CONSTANT(Name, Value)                                           \
  __asm__(".set " #Name ", " SEH_GS_STRING(Value));

SEH_GS_CONSTANT(SehGSFrameOffset, SEH_GS_FRAME_OFFSET)
SEH_GS_CONSTANT(SehGSStackSize, SEH_GS_STACK_SIZE)
SEH_GS_CONSTANT(SehGSCookieOffset, SEH_GS_COOKIE_OFFSET)
SEH_GS_CONSTANT(SehGSArgumentOffset, SEH_GS_ARGUMENT_OFFSET)
SEH_GS_CONSTANT(SehGSAlignment, SEH_GS_ALIGNMENT)

__asm__(SEH_GS_FUNCTION(SehGSFrame, 0, 3, __GSHandlerCheck_SEH, 0));
__asm__(SEH_GS_FUNCTION(SehGSAlignedFrame, 1, 7, __GSHandlerCheck_SEH, 0));
__asm__(SEH_GS_FUNCTION(SehGSStandaloneFrame, 0, 0, __GSHandlerCheck, 1));
__asm__(SEH_GS_FUNCTION(SehGSStandaloneAlignedFrame, 1, 4, __GSHandlerCheck,
                        1));

#undef SEH_GS_CONSTANT

#undef SEH_GS_FUNCTION
#undef SEH_GS_STRING
#undef SEH_GS_STRING_IMPL
#undef SEH_GS_ALIGNMENT
#undef SEH_GS_ARGUMENT_OFFSET
#undef SEH_GS_COOKIE_OFFSET
#undef SEH_GS_STACK_SIZE
#undef SEH_GS_FRAME_OFFSET
#undef SEH_GS_FRAME_REGISTER

#endif

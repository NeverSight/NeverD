//===- driver_wdm_seh.c - Genuine WDK API-raised C SEH --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original driver exercising constant C exception handlers. Compile against
/// genuine WDK headers. Explicit original GS frames exercise the linked WDK
/// security runtime independently of the C compiler's stack-protector choice.
//===----------------------------------------------------------------------===//

#include "driver_seh_gs.h"
#include "driver_seh_test.h"

C_ASSERT(sizeof(NTSTATUS) == 4);
C_ASSERT(sizeof(ULONG_PTR) == 8);
C_ASSERT(EXCEPTION_EXECUTE_HANDLER == 1);
C_ASSERT(STATUS_ACCESS_VIOLATION == (NTSTATUS)0xc0000005);
C_ASSERT(STATUS_DATATYPE_MISALIGNMENT == (NTSTATUS)0x80000002);

C_ASSERT(sizeof(CONTEXT) == 1232);
C_ASSERT(FIELD_OFFSET(CONTEXT, ContextFlags) == 48);
C_ASSERT(FIELD_OFFSET(CONTEXT, SegCs) == 56);
C_ASSERT(FIELD_OFFSET(CONTEXT, SegSs) == 66);
C_ASSERT(FIELD_OFFSET(CONTEXT, EFlags) == 68);
C_ASSERT(FIELD_OFFSET(CONTEXT, Rax) == 120);
C_ASSERT(FIELD_OFFSET(CONTEXT, Rip) == 248);
C_ASSERT(sizeof(EXCEPTION_RECORD) == 152);
C_ASSERT(FIELD_OFFSET(EXCEPTION_RECORD, ExceptionFlags) == 4);
C_ASSERT(FIELD_OFFSET(EXCEPTION_RECORD, ExceptionAddress) == 16);
C_ASSERT(FIELD_OFFSET(EXCEPTION_RECORD, NumberParameters) == 24);
C_ASSERT(FIELD_OFFSET(EXCEPTION_RECORD, ExceptionInformation) == 32);
C_ASSERT((CONTEXT_CONTROL | CONTEXT_INTEGER) == 0x100003);
C_ASSERT(SehRecoverControlCode ==
         CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_NEITHER, FILE_ANY_ACCESS));

static volatile ULONG Stage;
static CHAR Mode = 'S';

typedef NTSTATUS (*PSEH_TEST)(VOID);

static NTSTATUS Check(BOOLEAN Condition, ULONG Line) {
  if (Condition)
    return STATUS_SUCCESS;
  DbgPrint("WDM SEH: failure line=%lu stage=%lu\n", Line, Stage);
  return STATUS_UNSUCCESSFUL;
}

#define REQUIRE(Expression)                                                    \
  do {                                                                         \
    NTSTATUS CheckStatus = Check((BOOLEAN)(Expression), __LINE__);             \
    if (!NT_SUCCESS(CheckStatus))                                              \
      return CheckStatus;                                                      \
  } while (0)

static NTSTATUS GSCookiePaths(VOID) {
  const BOOLEAN Corrupt =
      Mode == SehGSCorruptCookie || Mode == SehGSAlignedCorruptCookie;
  const BOOLEAN Aligned =
      Mode == SehGSAlignedCookie || Mode == SehGSAlignedCorruptCookie;
  NTSTATUS Status = Aligned ? SehGSAlignedFrame(Corrupt) : SehGSFrame(Corrupt);
  REQUIRE(NT_SUCCESS(Status));
  DbgPrint("WDM SEH: GS cookie checked aligned=%u\n", Aligned);
  return STATUS_SUCCESS;
}

__declspec(noinline) static NTSTATUS DirectRaise(VOID) {
  volatile ULONG Local = 0x12345678;
  ULONG Code = 0;
  __try {
    Stage = 1;
    if (Mode == 'A')
      ExRaiseAccessViolation();
    else if (Mode == 'D')
      ExRaiseDatatypeMisalignment();
    else
      ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    Stage = 0xbad;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Code = (ULONG)GetExceptionCode();
    REQUIRE(Stage == 1 && Local == 0x12345678);
    Local ^= 0x00ff00ff;
    Stage = 2;
    DbgPrint("WDM SEH: direct caught code=%08lx local=%08lx\n", Code, Local);
  }
  REQUIRE(Code == (ULONG)(Mode == 'A'   ? STATUS_ACCESS_VIOLATION
                          : Mode == 'D' ? STATUS_DATATYPE_MISALIGNMENT
                                        : STATUS_INSUFFICIENT_RESOURCES));
  REQUIRE(Stage == 2 && Local == (0x12345678 ^ 0x00ff00ff));
  return STATUS_SUCCESS;
}

__declspec(noinline) static VOID RaiseHelper(VOID) {
  volatile ULONG Local[4] = {1, 2, 3, 4};
  // These original instructions force genuine PUSH_NONVOL unwind records and
  // make register restoration observable at the enclosing handler.
  __asm__ volatile("movabsq $0x1122334455667788, %%rbx\n\t"
                   "movabsq $0x8877665544332211, %%r12"
                   :
                   :
                   : "rbx", "r12");
  Stage = Local[2] + 7;
  ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
}

__declspec(noinline) static NTSTATUS AcrossHelper(VOID) {
  ULONG Code = 0;
  ULONG64 SavedB = 0, Saved12 = 0;
  volatile ULONG Local = 0x87654321;
  __asm__ volatile("movabsq $0x13579bdf2468ace0, %%rbx\n\t"
                   "movabsq $0xfedcba9876543210, %%r12"
                   :
                   :
                   : "rbx", "r12");
  __try {
    RaiseHelper();
    Stage = 0xbad;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Code = (ULONG)GetExceptionCode();
    __asm__ volatile("movq %%rbx, %0\n\t"
                     "movq %%r12, %1"
                     : "=&r"(SavedB), "=&r"(Saved12)
                     :
                     : "rbx", "r12");
    REQUIRE(Code == (ULONG)STATUS_INSUFFICIENT_RESOURCES);
    REQUIRE(Stage == 10 && Local == 0x87654321);
    REQUIRE(SavedB == 0x13579bdf2468ace0ULL);
    REQUIRE(Saved12 == 0xfedcba9876543210ULL);
    Stage = 11;
    DbgPrint("WDM SEH: helper caught code=%08lx nonvolatile restored\n", Code);
  }
  REQUIRE(Stage == 11);
  return STATUS_SUCCESS;
}

__declspec(noinline) static VOID RaiseWithSavedXmm(VOID) {
  __asm__ volatile("pxor %%xmm6, %%xmm6\n\t"
                   "pcmpeqd %%xmm15, %%xmm15"
                   :
                   :
                   : "xmm6", "xmm15");
  ExRaiseAccessViolation();
}

__declspec(noinline) static NTSTATUS AcrossXmmHelper(VOID) {
  const ULONG64 Expected[2] = {0x123456789abcdef0ULL, 0xfedcba9876543210ULL};
  ULONG64 Saved6[2] = {0}, Saved15[2] = {0};
  __asm__ volatile("movdqu %0, %%xmm6\n\t"
                   "movdqu %0, %%xmm15"
                   :
                   : "m"(Expected)
                   : "xmm6", "xmm15");
  __try {
    RaiseWithSavedXmm();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    __asm__ volatile("movdqu %%xmm6, %0\n\t"
                     "movdqu %%xmm15, %1"
                     : "=m"(Saved6), "=m"(Saved15));
    REQUIRE(GetExceptionCode() == STATUS_ACCESS_VIOLATION);
    REQUIRE(Saved6[0] == Expected[0] && Saved6[1] == Expected[1]);
    REQUIRE(Saved15[0] == Expected[0] && Saved15[1] == Expected[1]);
    Stage = 90;
  }
  REQUIRE(Stage == 90);
  DbgPrint("WDM SEH: full nonvolatile XMM restored\n");
  return STATUS_SUCCESS;
}

// Two noncontiguous runtime-function entries describe one stack frame. The
// secondary record saves R12 in the primary allocation, then chains to it.
extern VOID SehChainedRaise(VOID);
extern VOID SehPrologueFault(PVOID Address);
__asm__(".text\n"
        ".p2align 4\n"
        ".globl SehChainedRaise\n"
        "SehChainedRaise:\n"
        "pushq %rbx\n"
        ".LSehPrimaryPushEnd:\n"
        "subq $48, %rsp\n"
        ".LSehPrimaryAllocEnd:\n"
        "jmp .LSehSecondary\n"
        ".LSehPrimaryEnd:\n"
        ".p2align 4\n"
        ".LSehSecondary:\n"
        "movq %r12, 32(%rsp)\n"
        ".LSehSecondarySaveEnd:\n"
        "movabsq $0x1122334455667788, %rbx\n"
        "movabsq $0x8877665544332211, %r12\n"
        "callq *__imp_ExRaiseAccessViolation(%rip)\n"
        "ud2\n"
        ".LSehSecondaryEnd:\n"
        ".section .xdata,\"dr\"\n"
        ".p2align 2\n"
        ".set SehUnwindVersion, 1\n"
        ".set SehChainFlag, 4\n"
        ".set SehPushNonvolatile, 0\n"
        ".set SehAllocateSmall, 2\n"
        ".set SehSaveNonvolatile, 4\n"
        ".set SehRbx, 3\n"
        ".set SehR12, 12\n"
        ".LSehPrimaryUnwind:\n"
        ".byte SehUnwindVersion, .LSehPrimaryAllocEnd-SehChainedRaise, 2, 0\n"
        ".byte .LSehPrimaryAllocEnd-SehChainedRaise\n"
        ".byte (((48-8)/8)<<4)|SehAllocateSmall\n"
        ".byte .LSehPrimaryPushEnd-SehChainedRaise, "
        "(SehRbx<<4)|SehPushNonvolatile\n"
        ".LSehSecondaryUnwind:\n"
        ".byte (SehChainFlag<<3)|SehUnwindVersion\n"
        ".byte .LSehSecondarySaveEnd-.LSehSecondary, 2, 0\n"
        ".byte .LSehSecondarySaveEnd-.LSehSecondary, "
        "(SehR12<<4)|SehSaveNonvolatile\n"
        ".short 32/8\n"
        ".rva SehChainedRaise, .LSehPrimaryEnd, .LSehPrimaryUnwind\n"
        ".section .pdata,\"dr\"\n"
        ".p2align 2\n"
        ".rva SehChainedRaise, .LSehPrimaryEnd, .LSehPrimaryUnwind\n"
        ".rva .LSehSecondary, .LSehSecondaryEnd, .LSehSecondaryUnwind\n"
        ".text\n"
        ".p2align 4\n"
        ".globl SehPrologueFault\n"
        "SehPrologueFault:\n"
        ".seh_proc SehPrologueFault\n"
        "pushq %rbx\n"
        ".seh_pushreg %rbx\n"
        "movabsq $0x1122334455667788, %rbx\n"
        "movl (%rcx), %eax\n"
        "subq $32, %rsp\n"
        ".seh_stackalloc 32\n"
        ".seh_endprologue\n"
        "addq $32, %rsp\n"
        "popq %rbx\n"
        "retq\n"
        ".seh_endproc\n");

__declspec(noinline) static NTSTATUS AcrossChainedHelper(VOID) {
  ULONG64 SavedB = 0, Saved12 = 0;
  __asm__ volatile("movabsq $0x13579bdf2468ace0, %%rbx\n\t"
                   "movabsq $0xfedcba9876543210, %%r12"
                   :
                   :
                   : "rbx", "r12");
  __try {
    SehChainedRaise();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    __asm__ volatile("movq %%rbx, %0\n\t"
                     "movq %%r12, %1"
                     : "=&r"(SavedB), "=&r"(Saved12)
                     :
                     : "rbx", "r12");
    REQUIRE(GetExceptionCode() == STATUS_ACCESS_VIOLATION);
    REQUIRE(SavedB == 0x13579bdf2468ace0ULL);
    REQUIRE(Saved12 == 0xfedcba9876543210ULL);
    Stage = 91;
  }
  REQUIRE(Stage == 91);
  DbgPrint("WDM SEH: chained nonvolatile restored\n");
  return STATUS_SUCCESS;
}

__declspec(noinline) static NTSTATUS AcrossPrologueFault(PVOID Address) {
  ULONG64 SavedB = 0;
  __asm__ volatile("movabsq $0x13579bdf2468ace0, %%rbx" : : : "rbx");
  __try {
    SehPrologueFault(Address);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    __asm__ volatile("movq %%rbx, %0" : "=r"(SavedB) : : "rbx");
    REQUIRE(GetExceptionCode() == STATUS_ACCESS_VIOLATION);
    REQUIRE(SavedB == 0x13579bdf2468ace0ULL);
    Stage = 92;
  }
  REQUIRE(Stage == 92);
  DbgPrint("WDM SEH: partial prologue restored\n");
  return STATUS_SUCCESS;
}

__declspec(noinline) static NTSTATUS NestedConstant(VOID) {
  ULONG InnerCode = 0;
  volatile ULONG Local = 0xabcdef01;
  __try {
    __try {
      Stage = 20;
      ExRaiseAccessViolation();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      InnerCode = (ULONG)GetExceptionCode();
      REQUIRE(InnerCode == (ULONG)STATUS_ACCESS_VIOLATION);
      REQUIRE(Stage == 20 && Local == 0xabcdef01);
      Stage = 21;
      DbgPrint("WDM SEH: nested inner code=%08lx\n", InnerCode);
    }
    REQUIRE(Stage == 21);
    Stage = 22;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    DbgPrint("WDM SEH: failure unexpected outer code=%08lx\n",
             (ULONG)GetExceptionCode());
    return STATUS_UNSUCCESSFUL;
  }
  REQUIRE(Stage == 22 && Local == 0xabcdef01);
  return STATUS_SUCCESS;
}

__declspec(noinline) static NTSTATUS RaiseFromHandler(VOID) {
  ULONG OuterCode = 0;
  volatile ULONG Local = 0xa5a55a5a;
  __try {
    __try {
      Stage = 30;
      ExRaiseAccessViolation();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      ULONG InnerCode = (ULONG)GetExceptionCode();
      REQUIRE(InnerCode == (ULONG)STATUS_ACCESS_VIOLATION && Stage == 30);
      Stage = 31;
      Local ^= 0xff;
      DbgPrint("WDM SEH: reraising inner code=%08lx\n", InnerCode);
      ExRaiseDatatypeMisalignment();
    }
    Stage = 0xbad;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    OuterCode = (ULONG)GetExceptionCode();
    REQUIRE(OuterCode == (ULONG)STATUS_DATATYPE_MISALIGNMENT);
    REQUIRE(Stage == 31 && Local == (0xa5a55a5a ^ 0xff));
    Stage = 32;
    DbgPrint("WDM SEH: reraising outer code=%08lx\n", OuterCode);
  }
  REQUIRE(Stage == 32);
  return STATUS_SUCCESS;
}

__declspec(noinline) static VOID RaiseFromHelperHandler(VOID) {
  __try {
    Stage = 40;
    ExRaiseAccessViolation();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ULONG Code = (ULONG)GetExceptionCode();
    if (Code != (ULONG)STATUS_ACCESS_VIOLATION || Stage != 40) {
      DbgPrint("WDM SEH: failure helper inner code=%08lx\n", Code);
      ExRaiseStatus(STATUS_UNSUCCESSFUL);
    }
    Stage = 41;
    DbgPrint("WDM SEH: helper reraising inner code=%08lx\n", Code);
    ExRaiseDatatypeMisalignment();
  }
}

__declspec(noinline) static NTSTATUS AcrossHelperHandler(VOID) {
  __try {
    RaiseFromHelperHandler();
    Stage = 0xbad;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ULONG Code = (ULONG)GetExceptionCode();
    REQUIRE(Code == (ULONG)STATUS_DATATYPE_MISALIGNMENT && Stage == 41);
    Stage = 42;
    DbgPrint("WDM SEH: helper reraising outer code=%08lx\n", Code);
  }
  REQUIRE(Stage == 42);
  return STATUS_SUCCESS;
}

static PEXCEPTION_POINTERS FirstPointers;

// Deliberately use every home slot permitted by the Win64 call ABI.
__attribute__((naked, noinline)) static VOID WriteHomeSlots(VOID) {
  __asm__ volatile("movq $1, 8(%rsp)\n\t"
                   "movq $2, 16(%rsp)\n\t"
                   "movq $3, 24(%rsp)\n\t"
                   "movq $4, 32(%rsp)\n\tretq");
}

__declspec(noinline) static LONG LinkedFilter(PEXCEPTION_POINTERS Pointers,
                                              PEXCEPTION_POINTERS Outer) {
  if (Pointers == Outer || !Pointers || !Outer ||
      Pointers->ExceptionRecord->ExceptionCode !=
          STATUS_DATATYPE_MISALIGNMENT ||
      Pointers->ExceptionRecord->ExceptionRecord != Outer->ExceptionRecord ||
      Outer->ExceptionRecord->ExceptionCode != STATUS_ACCESS_VIOLATION ||
      !Pointers->ContextRecord->Rip || !Pointers->ContextRecord->Rsp)
    return EXCEPTION_CONTINUE_SEARCH;
  DbgPrint("WDM SEH: nested exception record linked\n");
  return EXCEPTION_EXECUTE_HANDLER;
}

__declspec(noinline) static VOID LocalNestedRaise(PEXCEPTION_POINTERS Outer) {
  volatile ULONG Local = 0x51a;
  __try {
    ExRaiseDatatypeMisalignment();
  } __except (LinkedFilter(GetExceptionInformation(), Outer)) {
    if (GetExceptionCode() != STATUS_DATATYPE_MISALIGNMENT || Local != 0x51a ||
        Outer->ExceptionRecord->ExceptionCode != STATUS_ACCESS_VIOLATION) {
      DbgPrint("WDM SEH: failure local nested handler\n");
      return;
    }
    DbgPrint("WDM SEH: local nested handler\n");
  }
}

__declspec(noinline) static LONG DynamicFilter(PEXCEPTION_POINTERS Pointers,
                                               volatile ULONG *Local,
                                               LONG Decision) {
  WriteHomeSlots();
  if ((Mode == SehNestedFilter || Mode == SehNestedSearch ||
       Mode == SehRepeatedFilter || Mode == SehNestedFinally) &&
      Pointers &&
      Pointers->ExceptionRecord->ExceptionCode != STATUS_ACCESS_VIOLATION) {
    PEXCEPTION_RECORD Record = Pointers->ExceptionRecord;
    const BOOLEAN Repeated = Mode == SehRepeatedFilter && Stage == 52;
    const NTSTATUS ExpectedCode =
        Repeated ? STATUS_INVALID_PARAMETER : STATUS_DATATYPE_MISALIGNMENT;
    const NTSTATUS PreviousCode =
        Repeated ? STATUS_DATATYPE_MISALIGNMENT : STATUS_ACCESS_VIOLATION;
    if (Record->ExceptionCode != ExpectedCode || !Record->ExceptionRecord ||
        Record->ExceptionRecord->ExceptionCode != PreviousCode || !Local ||
        *Local != 71 ||
        ((Record->ExceptionFlags & EXCEPTION_NESTED_CALL) != 0) !=
            (Mode != SehNestedFinally)) {
      DbgPrint("WDM SEH: failure nested record contract\n");
      return EXCEPTION_CONTINUE_SEARCH;
    }
    ++Stage;
    if (Mode == SehRepeatedFilter && !Repeated)
      ExRaiseStatus(STATUS_INVALID_PARAMETER);
    DbgPrint("WDM SEH: nested filter decision=%ld stage=%lu\n", Decision,
             Stage);
    return Decision;
  }
  if (!Pointers || !Pointers->ExceptionRecord || !Pointers->ContextRecord ||
      Pointers->ExceptionRecord->ExceptionCode != STATUS_ACCESS_VIOLATION ||
      Pointers->ExceptionRecord->ExceptionRecord ||
      Pointers->ExceptionRecord->ExceptionFlags ||
      !Pointers->ExceptionRecord->ExceptionAddress ||
      Pointers->ContextRecord->ContextFlags !=
          (CONTEXT_CONTROL | CONTEXT_INTEGER) ||
      !Pointers->ContextRecord->Rip || !Pointers->ContextRecord->Rsp ||
      !Local || *Local != 71 || (FirstPointers && FirstPointers != Pointers)) {
    DbgPrint("WDM SEH: failure dynamic record contract\n");
    return EXCEPTION_CONTINUE_SEARCH;
  }
  FirstPointers = Pointers;
  ++Stage;
  DbgPrint("WDM SEH: filter decision=%ld stage=%lu\n", Decision, Stage);
  if (Mode == SehNestedFilter || Mode == SehNestedSearch ||
      Mode == SehRepeatedFilter)
    ExRaiseDatatypeMisalignment();
  if (Mode == SehLocalFilter)
    LocalNestedRaise(Pointers);
  return Decision;
}

__declspec(noinline) static NTSTATUS FilteredRaise(VOID) {
  volatile ULONG Local = 71;
  Stage = 50;
  FirstPointers = NULL;
  __try {
    __try {
      ExRaiseAccessViolation();
    } __except (
        DynamicFilter(GetExceptionInformation(), &Local,
                      (Mode == SehSearchFilters || Mode == SehNestedSearch)
                          ? EXCEPTION_CONTINUE_SEARCH
                      : Mode == SehContinueApi ? EXCEPTION_CONTINUE_EXECUTION
                                               : EXCEPTION_EXECUTE_HANDLER)) {
      const ULONG ExpectedStage = Mode == SehRepeatedFilter ? 53
                                  : Mode == SehNestedFilter ? 52
                                                            : 51;
      REQUIRE(Stage == ExpectedStage && Local == 71);
      if (Mode == SehNestedFilter || Mode == SehRepeatedFilter)
        REQUIRE(GetExceptionCode() == (Mode == SehNestedFilter
                                           ? STATUS_DATATYPE_MISALIGNMENT
                                           : STATUS_INVALID_PARAMETER));
      Local = 72;
      DbgPrint("WDM SEH: dynamic inner handled\n");
    }
  } __except (DynamicFilter(GetExceptionInformation(), &Local,
                            EXCEPTION_EXECUTE_HANDLER)) {
    REQUIRE((Mode == SehSearchFilters || Mode == SehNestedSearch) &&
            Stage == (ULONG)(Mode == SehNestedSearch ? 53 : 52) && Local == 71);
    if (Mode == SehNestedSearch)
      REQUIRE((NTSTATUS)GetExceptionCode() == STATUS_DATATYPE_MISALIGNMENT);
    Local = 72;
    DbgPrint("WDM SEH: dynamic outer handled\n");
  }
  REQUIRE(Local == 72);
  return STATUS_SUCCESS;
}

__declspec(noinline) static VOID FinallyHelper(VOID) {
  volatile ULONG Local = 0xabc;
  __try {
    ExRaiseAccessViolation();
  } __finally {
    if (Mode == SehNestedFinally) {
      DbgPrint("WDM SEH: collided finally entered\n");
      ExRaiseDatatypeMisalignment();
    }
    if (Mode == SehLocalFinally)
      LocalNestedRaise(FirstPointers);
    if (!AbnormalTermination() || Local != 0xabc || Stage != 61) {
      DbgPrint("WDM SEH: failure helper finally\n");
      Stage = 0xbad;
    } else {
      Stage = 62;
      DbgPrint("WDM SEH: helper finally abnormal\n");
    }
  }
}

__declspec(noinline) static NTSTATUS FinallyPaths(VOID) {
  volatile ULONG Local = 71;
  FirstPointers = NULL;
  Stage = Mode == SehNormalFinally ? 61 : 60;
  __try {
    __try {
      if (Mode != SehNormalFinally)
        FinallyHelper();
    } __finally {
      if (Local != 71 || AbnormalTermination() != (Mode != SehNormalFinally) ||
          Stage != (ULONG)(Mode == SehNormalFinally ? 61 : 62)) {
        DbgPrint("WDM SEH: failure parent finally\n");
        Stage = 0xbad;
      } else {
        Local = 72;
        Stage = 63;
        DbgPrint("WDM SEH: parent finally abnormal=%u\n",
                 AbnormalTermination());
      }
    }
  } __except (DynamicFilter(GetExceptionInformation(), &Local,
                            EXCEPTION_EXECUTE_HANDLER)) {
    REQUIRE((Mode == SehExceptionalFinally || Mode == SehLocalFinally ||
             Mode == SehNestedFinally) &&
            Stage == 63 && Local == 72);
    if (Mode == SehNestedFinally)
      REQUIRE((NTSTATUS)GetExceptionCode() == STATUS_DATATYPE_MISALIGNMENT);
    DbgPrint("WDM SEH: finally handler\n");
  }
  REQUIRE(Stage == 63 && Local == 72);
  return STATUS_SUCCESS;
}

static ULONG RecoveredValue = SehRecoveredValue;

static KPROCESSOR_MODE ExpectedMode;
static HANDLE ExpectedCreatingProcess;
static PEPROCESS ExpectedProcess;

static VOID CaptureProcessContext(VOID) {
  ExpectedMode = ExGetPreviousMode();
  ExpectedCreatingProcess = PsGetCurrentProcessId();
  ExpectedProcess = IoGetCurrentProcess();
}

__declspec(noinline) static LONG InspectUserBuffer(PVOID Buffer) {
  PMDL Mdl;
  volatile ULONG *Alias;
  if (ExGetPreviousMode() != ExpectedMode ||
      PsGetCurrentProcessId() != ExpectedCreatingProcess ||
      IoGetCurrentProcess() != ExpectedProcess) {
    DbgPrint("WDM SEH: failure filter process identity\n");
    return EXCEPTION_CONTINUE_SEARCH;
  }
  DbgPrint("WDM SEH: filter process mode=%u creating=%lu\n", ExpectedMode,
           (ULONG)(ULONG_PTR)ExpectedCreatingProcess);
  ProbeForRead(Buffer, sizeof(ULONG), sizeof(ULONG));
  if (Mode != SehWorkerUserLock)
    ProbeForWrite(Buffer, sizeof(ULONG), sizeof(ULONG));
  Mdl = IoAllocateMdl(Buffer, sizeof(ULONG), FALSE, FALSE, NULL);
  if (!Mdl)
    return EXCEPTION_CONTINUE_SEARCH;
  MmProbeAndLockPages(Mdl, UserMode, IoWriteAccess);
  Alias = MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority);
  if (!Alias) {
    MmUnlockPages(Mdl);
    IoFreeMdl(Mdl);
    return EXCEPTION_CONTINUE_SEARCH;
  }
  *(volatile ULONG *)Buffer = SehFilterUserMarker;
  if (*Alias != SehFilterUserMarker) {
    DbgPrint("WDM SEH: failure filter user alias\n");
    MmUnlockPages(Mdl);
    IoFreeMdl(Mdl);
    return EXCEPTION_CONTINUE_SEARCH;
  }
  *Alias ^= 1;
  if (*(volatile ULONG *)Buffer != (SehFilterUserMarker ^ 1)) {
    DbgPrint("WDM SEH: failure filter user write\n");
    MmUnlockPages(Mdl);
    IoFreeMdl(Mdl);
    return EXCEPTION_CONTINUE_SEARCH;
  }
  MmUnlockPages(Mdl);
  IoFreeMdl(Mdl);
  DbgPrint("WDM SEH: filter user memory verified\n");
  return EXCEPTION_EXECUTE_HANDLER;
}

static PIRP WorkerIrp;
static PIO_WORKITEM WorkerItem;
static PVOID WorkerUserBuffer;
static PEPROCESS WorkerProcess;

static PMDL WorkerMdl;
static BOOLEAN WorkerMdlLocked;

__declspec(noinline) static LONG InspectAttachedBuffer(PVOID Buffer) {
  volatile ULONG *Alias;
  if (Mode == SehAttachedForbidden) {
    // A child filter must preserve its parent's bounded attachment contract.
    ExRaiseAccessViolation();
  }
  if (ExGetPreviousMode() != ExpectedMode ||
      PsGetCurrentProcessId() != ExpectedCreatingProcess ||
      IoGetCurrentProcess() != ExpectedProcess)
    return EXCEPTION_CONTINUE_SEARCH;
  ProbeForRead(Buffer, sizeof(ULONG), sizeof(ULONG));
  ProbeForWrite(Buffer, sizeof(ULONG), sizeof(ULONG));
  MmProbeAndLockPages(WorkerMdl, UserMode, IoWriteAccess);
  WorkerMdlLocked = TRUE;
  Alias = MmGetSystemAddressForMdlSafe(WorkerMdl, NormalPagePriority);
  if (!Alias)
    return EXCEPTION_CONTINUE_SEARCH;
  *(volatile ULONG *)Buffer = SehFilterUserMarker;
  if (*Alias != SehFilterUserMarker)
    return EXCEPTION_CONTINUE_SEARCH;
  *Alias ^= 1;
  return *(volatile ULONG *)Buffer == (SehFilterUserMarker ^ 1)
             ? EXCEPTION_EXECUTE_HANDLER
             : EXCEPTION_CONTINUE_SEARCH;
}

static VOID UserWorker(PDEVICE_OBJECT Device, PVOID Context) {
  NTSTATUS Status = STATUS_UNSUCCESSFUL;
  KAPC_STATE ApcState;
  const BOOLEAN Attached =
      Mode == SehAttachedWorker || Mode == SehAttachedForbidden;
  UNREFERENCED_PARAMETER(Device);
  UNREFERENCED_PARAMETER(Context);
  if (Attached) {
    WorkerMdl =
        IoAllocateMdl(WorkerUserBuffer, sizeof(ULONG), FALSE, FALSE, NULL);
    if (!WorkerMdl) {
      WorkerIrp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
      IoCompleteRequest(WorkerIrp, IO_NO_INCREMENT);
      IoFreeWorkItem(WorkerItem);
      return;
    }
    KeStackAttachProcess(WorkerProcess, &ApcState);
  }
  CaptureProcessContext();
  __try {
    if (Attached)
      ProbeForRead((PUCHAR)WorkerUserBuffer + 1, sizeof(ULONG), sizeof(ULONG));
    else
      ExRaiseAccessViolation();
  } __except (Attached ? InspectAttachedBuffer(WorkerUserBuffer)
                       : InspectUserBuffer(WorkerUserBuffer)) {
    if (Attached && ExpectedMode == KernelMode &&
        (ULONG_PTR)ExpectedCreatingProcess == 4 &&
        ExpectedProcess == WorkerProcess) {
      Status = STATUS_SUCCESS;
      WorkerIrp->IoStatus.Information = sizeof(ULONG);
    } else {
      DbgPrint("WDM SEH: failure worker gained user authority\n");
    }
  }
  if (Attached) {
    KeUnstackDetachProcess(&ApcState);
    if (WorkerMdlLocked)
      MmUnlockPages(WorkerMdl);
    IoFreeMdl(WorkerMdl);
    if (NT_SUCCESS(Status)) {
      DbgPrint("WDM SEH: filter process mode=%u creating=%lu\n", ExpectedMode,
               (ULONG)(ULONG_PTR)ExpectedCreatingProcess);
      DbgPrint("WDM SEH: filter user memory verified\n");
      DbgPrint("WDM SEH: attached worker filter handled\n");
    }
  }
  WorkerIrp->IoStatus.Status = Status;
  IoCompleteRequest(WorkerIrp, IO_NO_INCREMENT);
  IoFreeWorkItem(WorkerItem);
}

__declspec(noinline) static LONG RepairRead(PEXCEPTION_POINTERS Pointers,
                                            PVOID FaultAddress, PVOID Buffer) {
  if (Pointers->ExceptionRecord->ExceptionCode != STATUS_ACCESS_VIOLATION ||
      Pointers->ExceptionRecord->NumberParameters != 2 ||
      Pointers->ExceptionRecord->ExceptionInformation[0] != 0 ||
      Pointers->ExceptionRecord->ExceptionInformation[1] !=
          (ULONG_PTR)FaultAddress ||
      Pointers->ContextRecord->Rax != (ULONG_PTR)FaultAddress)
    return EXCEPTION_CONTINUE_SEARCH;
  if (InspectUserBuffer(Buffer) != EXCEPTION_EXECUTE_HANDLER)
    return EXCEPTION_CONTINUE_SEARCH;
  Pointers->ContextRecord->Rax = (ULONG_PTR)&RecoveredValue;
  if (Mode == SehRejectContextMutation)
    Pointers->ContextRecord->SegCs ^= 1;
  DbgPrint("WDM SEH: resume user read\n");
  // Volatile CPU state belongs to the faulted instruction, not this callback.
  __asm__ volatile("pxor %%xmm0, %%xmm0\n\tclc" : : : "xmm0", "cc");
  return EXCEPTION_CONTINUE_EXECUTION;
}

static DRIVER_DISPATCH Dispatch;
static NTSTATUS Dispatch(PDEVICE_OBJECT Device, PIRP Irp) {
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  NTSTATUS Status = STATUS_SUCCESS;
  UNREFERENCED_PARAMETER(Device);
  Irp->IoStatus.Information = 0;
  if (Stack->MajorFunction == IRP_MJ_DEVICE_CONTROL &&
      Mode == SehPrologueUnwind) {
    Status =
        AcrossPrologueFault(Stack->Parameters.DeviceIoControl.Type3InputBuffer);
  } else if (Stack->MajorFunction == IRP_MJ_DEVICE_CONTROL) {
    ULONG Value = 0;
    ULONG64 Vector = 0;
    UCHAR Carry = 0;
    PVOID Source = Stack->Parameters.DeviceIoControl.Type3InputBuffer;
    if (Mode == SehWorkerUserProbe || Mode == SehWorkerUserLock ||
        Mode == SehAttachedWorker || Mode == SehAttachedForbidden) {
      WorkerItem = IoAllocateWorkItem(Device);
      if (!WorkerItem)
        return STATUS_INSUFFICIENT_RESOURCES;
      WorkerIrp = Irp;
      WorkerUserBuffer = Irp->UserBuffer;
      WorkerProcess = IoGetRequestorProcess(Irp);
      IoMarkIrpPending(Irp);
      IoQueueWorkItem(WorkerItem, UserWorker, DelayedWorkQueue, NULL);
      return STATUS_PENDING;
    }
    CaptureProcessContext();
    __try {
      // Fixing RAX in the filter resumes exactly this faulting instruction.
      __asm__ volatile("movabsq $0x1122334455667788, %1\n\t"
                       "movq %1, %%xmm0\n\tstc\n\t"
                       "movl (%%rax), %%eax\n\tsetc %2\n\t"
                       "movq %%xmm0, %1"
                       : "=a"(Value), "=&r"(Vector), "=&q"(Carry)
                       : "a"(Source)
                       : "memory", "cc", "xmm0");
    } __except (
        RepairRead(GetExceptionInformation(), Source, Irp->UserBuffer)) {
      Status = GetExceptionCode();
    }
    if (Value != SehRecoveredValue || Vector != 0x1122334455667788ULL ||
        Carry != 1)
      Status = STATUS_UNSUCCESSFUL;
    if (NT_SUCCESS(Status)) {
      *(ULONG *)Irp->UserBuffer = Value;
      Irp->IoStatus.Information = sizeof(Value);
      DbgPrint("WDM SEH: resumed value=%08lx\n", Value);
    }
  }
  Irp->IoStatus.Status = Status;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return Status;
}

__declspec(noinline) static NTSTATUS UnhandledRaise(VOID) {
  ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
  return STATUS_UNSUCCESSFUL;
}

__declspec(noinline) static NTSTATUS UnsupportedCPUFault(VOID) {
  volatile ULONG Value = 0;
  __try {
    Value = *(volatile ULONG *)(ULONG_PTR)0x11100000;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    DbgPrint("WDM SEH: unsupported CPU handler executed code=%08lx\n",
             (ULONG)GetExceptionCode());
  }
  return (NTSTATUS)Value;
}

static DRIVER_UNLOAD Unload;
static VOID Unload(PDRIVER_OBJECT DriverObject) {
  if (DriverObject->DeviceObject)
    IoDeleteDevice(DriverObject->DeviceObject);
  DbgPrint("WDM SEH: unload mode=%c stage=%lu\n", Mode, Stage);
}

DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject,
                     PUNICODE_STRING RegistryPath) {
  PSEH_TEST Test = DirectRaise;
  NTSTATUS Status;
  if (RegistryPath && RegistryPath->Length >= sizeof(WCHAR)) {
    WCHAR Last = RegistryPath->Buffer[RegistryPath->Length / sizeof(WCHAR) - 1];
    if ((Last >= 'A' && Last <= 'Z') || Last == SehXmmUnwind ||
        Last == SehChainedUnwind || Last == SehPrologueUnwind ||
        Last == SehGSCookie || Last == SehGSAlignedCookie ||
        Last == SehGSCorruptCookie || Last == SehGSAlignedCorruptCookie)
      Mode = (CHAR)Last;
  }
  switch (Mode) {
  case SehGSCookie:
  case SehGSAlignedCookie:
  case SehGSCorruptCookie:
  case SehGSAlignedCorruptCookie:
    Test = GSCookiePaths;
    break;
  case SehChainedUnwind:
    Test = AcrossChainedHelper;
    break;
  case SehXmmUnwind:
    Test = AcrossXmmHelper;
    break;
  case 'H':
    Test = AcrossHelper;
    break;
  case 'N':
    Test = NestedConstant;
    break;
  case 'R':
    Test = RaiseFromHandler;
    break;
  case 'G':
    Test = AcrossHelperHandler;
    break;
  case SehDynamicFilter:
  case SehSearchFilters:
  case SehContinueApi:
  case SehNestedFilter:
  case SehNestedSearch:
  case SehRepeatedFilter:
  case SehLocalFilter:
    Test = FilteredRaise;
    break;
  case SehExceptionalFinally:
  case SehNestedFinally:
  case SehLocalFinally:
  case SehNormalFinally:
    Test = FinallyPaths;
    break;
  case 'U':
    Test = UnhandledRaise;
    break;
  case 'C':
    Test = UnsupportedCPUFault;
    break;
  default:
    break;
  }
  if (Mode == SehPrologueUnwind || Mode == SehRecoverUserRead ||
      Mode == SehRejectContextMutation || Mode == SehWorkerUserProbe ||
      Mode == SehWorkerUserLock || Mode == SehAttachedWorker ||
      Mode == SehAttachedForbidden) {
    UNICODE_STRING Name;
    RtlInitUnicodeString(&Name, L"\\Device\\NeverDSEH");
    PDEVICE_OBJECT Device;
    Status = IoCreateDevice(DriverObject, 0, &Name, FILE_DEVICE_UNKNOWN, 0,
                            FALSE, &Device);
    if (!NT_SUCCESS(Status))
      return Status;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = Dispatch;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = Dispatch;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = Dispatch;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = Dispatch;
    DriverObject->DriverUnload = Unload;
    Device->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
  }
  DbgPrint("WDM SEH: begin mode=%c\n", Mode);
  Status = Test();
  if (!NT_SUCCESS(Status))
    return Status;
  DriverObject->DriverUnload = Unload;
  DbgPrint("WDM SEH: complete mode=%c stage=%lu\n", Mode, Stage);
  return STATUS_SUCCESS;
}

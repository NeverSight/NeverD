//===- ExceptionInfo.h - Normalized exception metadata --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the format-independent representation used to carry table-based
/// unwind and language exception metadata from loaders through NeverD's IR and
/// rewrite pipelines.  Raw file offsets never escape the loader: consumers see
/// checked half-open VA ranges, normalized targets, parse provenance, and an
/// explicit completeness state.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_EXCEPTIONINFO_H
#define NEVERD_LOADER_EXCEPTIONINFO_H

#include "neverd/loader/ExceptionCommon.h"
#include "neverd/loader/LanguageEH.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

/// Native runtime-function representation that produced a normalized record.
enum class ExceptionEncoding : uint8_t {
  Unknown,
  X64UnwindV1,
  X64UnwindV2,
  X64UnwindV3,
  ARM32Packed,
  ARM32PackedFragment,
  ARM32Unpacked,
  ARM64Packed,
  ARM64PackedFragment,
  ARM64Unpacked,
  /// DWARF call frame information: one FDE and its CIE.
  DwarfFDE,
  /// Darwin `__unwind_info` second-level compact entry.
  CompactUnwind,
  /// x86-32 `_except_handler3` scope table reached through the registration
  /// chain the prologue installed.
  X86ScopeTableEH3,
  /// x86-32 `_except_handler4` scope table, which prefixes the entry array
  /// with security-cookie displacements.
  X86ScopeTableEH4,
  /// x86-32 `__CxxFrameHandler` `FuncInfo`, whose maps hold absolute
  /// pointers rather than the image-relative fields the x64 form uses.
  X86CxxFuncInfo,
  /// Delphi's x86-32 registration frame, whose handler is a runtime routine
  /// rather than a table-driven dispatcher.
  DelphiX86Chain,
  /// Go `pclntab` frame metadata.
  GoFuncTable,
  /// ARM EHABI `EXIDX_CANTUNWIND`: the index covers the frame in order to say
  /// that it may not be unwound through.
  ARMEHABICantUnwind,
  /// ARM EHABI index entry whose own word holds the whole descriptor.
  ARMEHABIInline,
  /// ARM EHABI `.ARM.extab` entry using an ARM-defined personality index.
  ARMEHABICompact,
  /// ARM EHABI `.ARM.extab` entry naming its personality routine, which is
  /// the form that carries an Itanium LSDA inline after its unwind opcodes.
  ARMEHABIGeneric,
};

inline const char *getExceptionEncodingName(ExceptionEncoding Encoding) {
  switch (Encoding) {
  case ExceptionEncoding::X64UnwindV1:
    return "x64-unwind-v1";
  case ExceptionEncoding::X64UnwindV2:
    return "x64-unwind-v2";
  case ExceptionEncoding::X64UnwindV3:
    return "x64-unwind-v3";
  case ExceptionEncoding::ARM32Packed:
    return "arm32-packed";
  case ExceptionEncoding::ARM32PackedFragment:
    return "arm32-packed-fragment";
  case ExceptionEncoding::ARM32Unpacked:
    return "arm32-unpacked";
  case ExceptionEncoding::ARM64Packed:
    return "arm64-packed";
  case ExceptionEncoding::ARM64PackedFragment:
    return "arm64-packed-fragment";
  case ExceptionEncoding::ARM64Unpacked:
    return "arm64-unpacked";
  case ExceptionEncoding::DwarfFDE:
    return "dwarf-fde";
  case ExceptionEncoding::CompactUnwind:
    return "compact-unwind";
  case ExceptionEncoding::X86ScopeTableEH3:
    return "x86-scope-table-eh3";
  case ExceptionEncoding::X86ScopeTableEH4:
    return "x86-scope-table-eh4";
  case ExceptionEncoding::X86CxxFuncInfo:
    return "x86-cxx-funcinfo";
  case ExceptionEncoding::DelphiX86Chain:
    return "delphi-x86-chain";
  case ExceptionEncoding::GoFuncTable:
    return "go-func-table";
  case ExceptionEncoding::ARMEHABICantUnwind:
    return "arm-ehabi-cantunwind";
  case ExceptionEncoding::ARMEHABIInline:
    return "arm-ehabi-inline";
  case ExceptionEncoding::ARMEHABICompact:
    return "arm-ehabi-compact";
  case ExceptionEncoding::ARMEHABIGeneric:
    return "arm-ehabi-generic";
  case ExceptionEncoding::Unknown:
    return "unknown";
  }
  return "unknown";
}

/// The exception model a native encoding belongs to.  Keeping this a pure
/// function of the encoding means a consumer cannot see a record whose model
/// and encoding disagree.
inline ExceptionModel getExceptionEncodingModel(ExceptionEncoding Encoding) {
  switch (Encoding) {
  case ExceptionEncoding::X64UnwindV1:
  case ExceptionEncoding::X64UnwindV2:
  case ExceptionEncoding::X64UnwindV3:
  case ExceptionEncoding::ARM32Packed:
  case ExceptionEncoding::ARM32PackedFragment:
  case ExceptionEncoding::ARM32Unpacked:
  case ExceptionEncoding::ARM64Packed:
  case ExceptionEncoding::ARM64PackedFragment:
  case ExceptionEncoding::ARM64Unpacked:
    return ExceptionModel::WindowsTable;
  case ExceptionEncoding::DwarfFDE:
    return ExceptionModel::Itanium;
  case ExceptionEncoding::CompactUnwind:
    return ExceptionModel::CompactUnwind;
  case ExceptionEncoding::X86ScopeTableEH3:
  case ExceptionEncoding::X86ScopeTableEH4:
  case ExceptionEncoding::X86CxxFuncInfo:
  case ExceptionEncoding::DelphiX86Chain:
    return ExceptionModel::WindowsRegistration;
  case ExceptionEncoding::GoFuncTable:
    return ExceptionModel::GoRuntime;
  case ExceptionEncoding::ARMEHABICantUnwind:
  case ExceptionEncoding::ARMEHABIInline:
  case ExceptionEncoding::ARMEHABICompact:
  case ExceptionEncoding::ARMEHABIGeneric:
    return ExceptionModel::ARMEHABI;
  case ExceptionEncoding::Unknown:
    return ExceptionModel::None;
  }
  return ExceptionModel::None;
}

enum class RuntimeFunctionKind : uint8_t {
  Primary,
  Chained,
  Fragment,
};

/// Unwind actions, normalized across targets.
///
/// Every operation is stated in the *saving* direction, as the prologue
/// performs it, even where the native table spells the epilogue instruction
/// instead — ARM32's codes name `pop` and `add sp` where ARM64's name `stp`
/// and `sub sp`.  Both describe the same frame, and a consumer that has to
/// ask which way round a record was written cannot use it.
///
/// New enumerators are appended: the value is serialized into the lifted
/// Windows EH metadata, so reordering would silently reinterpret it.
enum class UnwindOperationKind : uint8_t {
  PushNonVolatile,
  PushTwoRegisters,
  PushConsecutiveRegisters,
  AllocateLarge,
  AllocateHuge,
  AllocateSmall,
  SetFramePointer,
  SaveNonVolatile,
  SaveNonVolatileFar,
  Epilog,
  Spare,
  SaveXMM128,
  SaveXMM128Far,
  PushMachineFrame,
  PushCanonicalFrame,
  Opaque,
  /// `sub sp,sp,#N`.  Covers ARM64's `alloc_s`/`alloc_m`/`alloc_l` and ARM32's
  /// `add sp` codes, which differ only in how wide an immediate they encode.
  AllocateStack,
  /// `alloc_z`: an allocation counted in SVE vector lengths rather than bytes,
  /// so its size is not known until the implementation's width is.
  AllocateVectorLengthStack,
  /// Store one register at a non-negative offset from sp.
  SaveRegister,
  /// Store one register and decrement sp in the same instruction.
  SaveRegisterPreIndexed,
  /// Store a register pair at a non-negative offset from sp.
  SaveRegisterPair,
  /// Store a register pair and decrement sp in the same instruction.
  SaveRegisterPairPreIndexed,
  /// `save_next`: repeat the previous pair save for the next pair up, at the
  /// next slot up.  Kept as its own operation because the pair it names is
  /// only defined relative to the operation before it.
  SaveNextPair,
  /// `add x29,sp,#N`: establish the frame pointer above the stack pointer.
  AddFramePointer,
  /// `mov sp,rX`: the stack pointer was restored from another register, so the
  /// frame's extent is not recoverable from the unwind codes alone.
  SetStackPointerFromRegister,
  /// `ldr lr,[sp],#X`: the return address alone is reloaded and sp advanced.
  LoadReturnAddress,
  /// `pacibsp`: the return address in lr is signed against sp, so a reader of
  /// the saved value must strip the pointer authentication code.
  SignReturnAddress,
  /// One of the `MSFT_OP_*` codes an assembly routine uses to declare that a
  /// trap frame, machine frame, or context record sits on the stack in place
  /// of an ordinary frame.  \ref UnwindOperation::OpInfo carries which.
  CustomStackFrame,
  /// An instruction the unwinder must step over but that changes no state.
  Nop,
  End,
  /// `end_c`: end of the codes for the current chained scope, with the parent
  /// scope's codes continuing after it.
  EndChained,
  /// `add sp,sp,#N`: the frame the prologue leaves is *smaller* than the one
  /// it was entered with.  ARM EHABI is the only target here that can say so,
  /// and it says it often enough that folding it into \ref AllocateStack with
  /// a sign nobody reads would lose the size of the frame.
  DeallocateStack,
};

/// Register file an operation's register operand is numbered in.
enum class UnwindRegisterClass : uint8_t {
  None,
  /// x64 general-purpose, ARM `r0`-`r15`, ARM64 `x0`-`x30`.
  GeneralPurpose,
  /// ARM and ARM64 `d0`-`d31`.
  FloatingPoint,
  /// ARM64 `q0`-`q31`.  Only the Arm64EC entry thunks save these: x64 treats
  /// the full 128-bit register as non-volatile where ARM64 treats only its low
  /// half that way, so a thunk between the two has to preserve the difference.
  Vector,
};

/// One decoded unwind action.  OperandBytes retains the exact native payload
/// when an operation is unknown or cannot yet be represented semantically.
struct UnwindOperation {
  UnwindOperationKind Kind = UnwindOperationKind::Opaque;
  /// Position of the operation in the native array: a slot index on x64,
  /// where the array is an array of 2-byte slots, and a byte offset on ARM,
  /// where it is an array of bytes and an epilogue scope points into it.
  uint32_t CodeOffset = 0;
  uint8_t OpInfo = 0;
  /// Native encoding size, in the same unit as \ref CodeOffset.
  uint8_t SlotCount = 0;
  /// Lowest register the operation acts on, numbered within \ref
  /// RegisterClass.
  uint16_t Register = 0;
  uint64_t StackOffset = 0;
  UnwindRegisterClass RegisterClass = UnwindRegisterClass::None;
  /// Every register the operation acts on, as a bitmask over \ref
  /// RegisterClass's numbering.  Pairs, ranges, and ARM's arbitrary pop masks
  /// are all expanded here, so a consumer never has to re-derive the set from
  /// a base register and a count it would have to know the encoding to read.
  uint32_t RegisterMask = 0;
  /// Size of the machine instruction this operation stands against, for the
  /// targets whose unwind codes map one-to-one onto instructions.  Zero on
  /// x64, where no such mapping exists.
  uint8_t InstructionSize = 0;
  std::vector<uint8_t> OperandBytes;
};

struct UnwindEpilog {
  int64_t StartOffset = 0;
  uint8_t Flags = 0;
  uint32_t FirstOperationOffset = 0;
  uint32_t LastInstructionOffset = 0;
  std::vector<UnwindOperation> Operations;
};

/// The exact personality identity.  Two personalities that share a table
/// schema still remain distinct enumerators whenever their run-time contract
/// differs, because a rewriter must reproduce the contract and not merely the
/// bytes: GS wrappers stay separate from their base language handlers because
/// their cookie payload is part of that contract, and a language runtime's
/// personality stays separate from the C++ one it borrows a schema from
/// because its landing pads obey the language's own rules.
enum class ExceptionPersonality : uint8_t {
  None,
  Unknown,
  // Windows table model.
  CSpecificHandler,
  CxxFrameHandler3,
  CxxFrameHandler4,
  GSHandlerCheckSEH,
  GSHandlerCheckEH,
  GSHandlerCheckEH4,
  // Windows x86-32 registration model.
  ExceptHandler3,
  ExceptHandler4,
  /// x86-32 `__CxxFrameHandler`, reached through the registration chain.
  CxxFrameHandlerX86,
  // Itanium / DWARF model.
  GxxPersonalityV0,
  /// MinGW/Windows Itanium personality forwarding to SEH dispatch.
  GxxPersonalitySEH0,
  /// SJLJ variant used by targets without table-driven unwinding.
  GxxPersonalitySJ0,
  /// `__gcc_personality_v0`: cleanup-only, emitted for C with
  /// `-fexceptions`.
  GccPersonalityV0,
  /// `__gcc_personality_seh0`: the same cleanup-only routine reached through
  /// SEH dispatch, which is what mingw installs for C.
  GccPersonalitySEH0,
  /// `__gcc_personality_sj0`: the SJLJ variant.
  GccPersonalitySJ0,
  /// `__objc_personality_v0`: Apple's non-fragile Objective-C runtime.  Its
  /// type-table slots address an `objc_typeinfo`, whose first two fields are
  /// laid out to match `std::type_info` so that one table can hold both.
  ObjCPersonalityV0,
  /// `__gnu_objc_personality_v0`: GCC libobjc, GNUstep's older ABI, and
  /// ObjFW.  Its type-table slots hold the class *name string* itself rather
  /// than the address of a descriptor, so nothing in such a slot may be
  /// dereferenced the way an Itanium or Apple slot is.
  GnuObjCPersonalityV0,
  /// `__gnu_objc_personality_seh0`/`_sj0`: the same routine reached through
  /// SEH dispatch and through setjmp/longjmp.
  GnuObjCPersonalitySEH0,
  GnuObjCPersonalitySJ0,
  /// `__gnustep_objc_personality_v0`: GNUstep 1.7's Objective-C routine.  It
  /// reads the same name-string slots as `__gnu_objc_personality_v0`.
  GNUstepObjCPersonalityV0,
  /// `__gnustep_objcxx_personality_v0`: what GNUstep installs once C++ types
  /// can appear in the same table, which is every Objective-C++ translation
  /// unit.  Its slots address a `gnustep::libobjc::__objc_class_type_info`,
  /// a real `std::type_info` subclass, so they are read as Itanium slots.
  GNUstepObjCXXPersonalityV0,
  /// Rust's own personality routine.  It uses the Itanium tables but only
  /// ever selects cleanup or its single `catch_unwind` filter.
  RustEhPersonality,
  // Ada.  GNAT names its routine the way GCC names every front end's, so the
  // three spellings differ only in which unwinder reaches them.  All of them
  // read the call-site and action tables as C++ does; what they do not share
  // is the type table, whose slots address an Ada `Exception_Data` rather than
  // a `std::type_info` and so may not be followed as RTTI.
  /// `__gnat_personality_v0`: the DWARF variant.
  GnatPersonalityV0,
  /// `__gnat_personality_sj0`: the SJLJ variant, which is what GNAT installs
  /// on every target configured `--enable-sjlj-exceptions`.
  GnatPersonalitySJ0,
  /// `__gnat_personality_seh0` and the `__gnat_personality_imp` it forwards
  /// to once `_GCC_specific_handler` has turned the SEH state into GCC's.
  GnatPersonalitySEH0,
  // D.  The three compilers agree on the tables and disagree on the name.
  /// `__dmd_personality_v0`: the reference compiler's routine.
  DmdPersonalityV0,
  /// `_d_eh_personality`: LDC's, generalized from DMD's.
  DRuntimeEhPersonality,
  /// `__gdc_personality_v0`: GDC's, which follows GCC's naming for every
  /// front end it hosts, and so has the same three variants.
  GdcPersonalityV0,
  GdcPersonalitySJ0,
  GdcPersonalitySEH0,
  // ARM EHABI.  These three are named by index rather than by address, and
  // they are unwinders rather than handlers: they restore the frame and
  // resume, so a frame that installs one stops nothing.  They stay distinct
  // from each other because the index also decides how many opcode words the
  // entry has and whether scope descriptors follow them.
  /// `__aeabi_unwind_cpp_pr0`: three opcode bytes, no further words.
  AeabiUnwindCppPr0,
  /// `__aeabi_unwind_cpp_pr1`: a word count and scope descriptors after the
  /// opcodes.
  AeabiUnwindCppPr1,
  /// `__aeabi_unwind_cpp_pr2`: as `pr1`, with the wider scope offsets.
  AeabiUnwindCppPr2,
  // Delphi.
  /// Delphi x86-32 `@HandleAnyException` / `@HandleFinally` family.
  DelphiX86Handler,
  /// Delphi x86-64 `__DelphiExceptionHandler`.
  DelphiExceptionHandler,
  // Go.
  /// Go does not install a personality; the runtime unwinds from its own
  /// frame metadata.  The enumerator exists so a Go frame is not reported as
  /// having an unknown personality.
  GoRuntimeDispatch,
  /// `runtime.sehtramp`, the one personality the Go linker does install.  Go
  /// on windows/amd64 emits an exception directory so Windows can unwind
  /// through Go frames during a cgo call, and marks the single landing pad
  /// (`runtime.asmcgocall_landingpad`) with this routine.  It carries no
  /// language data: the handler walks the goroutine stack itself.
  GoSEHTrampoline,
};

inline const char *getExceptionPersonalityName(ExceptionPersonality P) {
  switch (P) {
  case ExceptionPersonality::None:
    return "none";
  case ExceptionPersonality::Unknown:
    return "unknown";
  case ExceptionPersonality::CSpecificHandler:
    return "__C_specific_handler";
  case ExceptionPersonality::CxxFrameHandler3:
    return "__CxxFrameHandler3";
  case ExceptionPersonality::CxxFrameHandler4:
    return "__CxxFrameHandler4";
  case ExceptionPersonality::GSHandlerCheckSEH:
    return "__GSHandlerCheck_SEH";
  case ExceptionPersonality::GSHandlerCheckEH:
    return "__GSHandlerCheck_EH";
  case ExceptionPersonality::GSHandlerCheckEH4:
    return "__GSHandlerCheck_EH4";
  case ExceptionPersonality::ExceptHandler3:
    return "_except_handler3";
  case ExceptionPersonality::ExceptHandler4:
    return "_except_handler4";
  case ExceptionPersonality::CxxFrameHandlerX86:
    return "__CxxFrameHandler";
  case ExceptionPersonality::GxxPersonalityV0:
    return "__gxx_personality_v0";
  case ExceptionPersonality::GxxPersonalitySEH0:
    return "__gxx_personality_seh0";
  case ExceptionPersonality::GxxPersonalitySJ0:
    return "__gxx_personality_sj0";
  case ExceptionPersonality::GccPersonalityV0:
    return "__gcc_personality_v0";
  case ExceptionPersonality::GccPersonalitySEH0:
    return "__gcc_personality_seh0";
  case ExceptionPersonality::GccPersonalitySJ0:
    return "__gcc_personality_sj0";
  case ExceptionPersonality::ObjCPersonalityV0:
    return "__objc_personality_v0";
  case ExceptionPersonality::GnuObjCPersonalityV0:
    return "__gnu_objc_personality_v0";
  case ExceptionPersonality::GnuObjCPersonalitySEH0:
    return "__gnu_objc_personality_seh0";
  case ExceptionPersonality::GnuObjCPersonalitySJ0:
    return "__gnu_objc_personality_sj0";
  case ExceptionPersonality::GNUstepObjCPersonalityV0:
    return "__gnustep_objc_personality_v0";
  case ExceptionPersonality::GNUstepObjCXXPersonalityV0:
    return "__gnustep_objcxx_personality_v0";
  case ExceptionPersonality::RustEhPersonality:
    return "rust_eh_personality";
  case ExceptionPersonality::GnatPersonalityV0:
    return "__gnat_personality_v0";
  case ExceptionPersonality::GnatPersonalitySJ0:
    return "__gnat_personality_sj0";
  case ExceptionPersonality::GnatPersonalitySEH0:
    return "__gnat_personality_seh0";
  case ExceptionPersonality::DmdPersonalityV0:
    return "__dmd_personality_v0";
  case ExceptionPersonality::DRuntimeEhPersonality:
    return "_d_eh_personality";
  case ExceptionPersonality::GdcPersonalityV0:
    return "__gdc_personality_v0";
  case ExceptionPersonality::GdcPersonalitySJ0:
    return "__gdc_personality_sj0";
  case ExceptionPersonality::GdcPersonalitySEH0:
    return "__gdc_personality_seh0";
  case ExceptionPersonality::AeabiUnwindCppPr0:
    return "__aeabi_unwind_cpp_pr0";
  case ExceptionPersonality::AeabiUnwindCppPr1:
    return "__aeabi_unwind_cpp_pr1";
  case ExceptionPersonality::AeabiUnwindCppPr2:
    return "__aeabi_unwind_cpp_pr2";
  case ExceptionPersonality::DelphiX86Handler:
    return "@HandleAnyException";
  case ExceptionPersonality::DelphiExceptionHandler:
    return "__DelphiExceptionHandler";
  case ExceptionPersonality::GoRuntimeDispatch:
    return "go-runtime-dispatch";
  case ExceptionPersonality::GoSEHTrampoline:
    return "runtime.sehtramp";
  }
  return "unknown";
}

inline bool isSEHPersonality(ExceptionPersonality P) {
  return P == ExceptionPersonality::CSpecificHandler ||
         P == ExceptionPersonality::GSHandlerCheckSEH ||
         P == ExceptionPersonality::ExceptHandler3 ||
         P == ExceptionPersonality::ExceptHandler4;
}

inline bool isCxxPersonality(ExceptionPersonality P) {
  return P == ExceptionPersonality::CxxFrameHandler3 ||
         P == ExceptionPersonality::CxxFrameHandler4 ||
         P == ExceptionPersonality::GSHandlerCheckEH ||
         P == ExceptionPersonality::GSHandlerCheckEH4 ||
         P == ExceptionPersonality::CxxFrameHandlerX86;
}

inline bool isGSWrappedPersonality(ExceptionPersonality P) {
  return P == ExceptionPersonality::GSHandlerCheckSEH ||
         P == ExceptionPersonality::GSHandlerCheckEH ||
         P == ExceptionPersonality::GSHandlerCheckEH4;
}

/// True for a personality that dispatches through an Itanium LSDA.
inline bool isItaniumPersonality(ExceptionPersonality P) {
  switch (P) {
  case ExceptionPersonality::GxxPersonalityV0:
  case ExceptionPersonality::GxxPersonalitySEH0:
  case ExceptionPersonality::GxxPersonalitySJ0:
  case ExceptionPersonality::GccPersonalityV0:
  case ExceptionPersonality::GccPersonalitySEH0:
  case ExceptionPersonality::GccPersonalitySJ0:
  case ExceptionPersonality::ObjCPersonalityV0:
  case ExceptionPersonality::GnuObjCPersonalityV0:
  case ExceptionPersonality::GnuObjCPersonalitySEH0:
  case ExceptionPersonality::GnuObjCPersonalitySJ0:
  case ExceptionPersonality::GNUstepObjCPersonalityV0:
  case ExceptionPersonality::GNUstepObjCXXPersonalityV0:
  case ExceptionPersonality::RustEhPersonality:
  case ExceptionPersonality::GnatPersonalityV0:
  case ExceptionPersonality::GnatPersonalitySJ0:
  case ExceptionPersonality::GnatPersonalitySEH0:
  case ExceptionPersonality::DmdPersonalityV0:
  case ExceptionPersonality::DRuntimeEhPersonality:
  case ExceptionPersonality::GdcPersonalityV0:
  case ExceptionPersonality::GdcPersonalitySJ0:
  case ExceptionPersonality::GdcPersonalitySEH0:
    return true;
  default:
    return false;
  }
}

/// True for a personality whose LSDA uses the setjmp/longjmp call-site form.
///
/// The two forms are told apart by the personality and by nothing else: the
/// header they share declares an encoding for the call-site table but not
/// which of the two things that table's first two columns are.  Under the SJLJ
/// form they are a call-site index and an action, not an address and a length,
/// so a reader that guesses wrong does not fail -- it produces guarded ranges
/// and landing pads at addresses the program never named.
inline bool isSJLJPersonality(ExceptionPersonality P) {
  return P == ExceptionPersonality::GxxPersonalitySJ0 ||
         P == ExceptionPersonality::GccPersonalitySJ0 ||
         P == ExceptionPersonality::GnuObjCPersonalitySJ0 ||
         P == ExceptionPersonality::GnatPersonalitySJ0 ||
         P == ExceptionPersonality::GdcPersonalitySJ0;
}

/// Which Objective-C runtime \p P belongs to, or nullopt when it is not an
/// Objective-C personality at all.  This is what decides how a type-table slot
/// is read; see \ref ObjCRuntimeKind for why the answer matters.
inline std::optional<ObjCRuntimeKind>
getObjCRuntimeForPersonality(ExceptionPersonality P) {
  switch (P) {
  case ExceptionPersonality::ObjCPersonalityV0:
    return ObjCRuntimeKind::AppleNonFragile;
  case ExceptionPersonality::GnuObjCPersonalityV0:
  case ExceptionPersonality::GnuObjCPersonalitySEH0:
  case ExceptionPersonality::GnuObjCPersonalitySJ0:
  case ExceptionPersonality::GNUstepObjCPersonalityV0:
    return ObjCRuntimeKind::GNU;
  case ExceptionPersonality::GNUstepObjCXXPersonalityV0:
    return ObjCRuntimeKind::GNUstepObjCXX;
  default:
    return std::nullopt;
  }
}

/// True for a personality whose landing pads only run cleanup.  A frame with
/// such a personality can never stop an in-flight exception, which is what
/// lets the structurer represent it as scope-exit code instead of a handler.
inline bool isCleanupOnlyPersonality(ExceptionPersonality P) {
  return P == ExceptionPersonality::GccPersonalityV0 ||
         P == ExceptionPersonality::GccPersonalitySEH0 ||
         P == ExceptionPersonality::GccPersonalitySJ0;
}

enum class SEHScopeKind : uint8_t {
  Filter,
  CatchAll,
  Finally,
};

struct SEHScopeRecord {
  ExceptionAddressRange GuardedRange;
  SEHScopeKind Kind = SEHScopeKind::Filter;
  va_t FilterOrFinallyVA = 0;
  va_t HandlerVA = 0;
  va_t ContinuationVA = 0;
  ExceptionParseStatus ParseStatus = ExceptionParseStatus::Complete;
};

struct SEHExceptionInfo {
  std::vector<SEHScopeRecord> Scopes;
};

struct CxxUnwindAction {
  int32_t ToState = -1;
  va_t ActionVA = 0;
  enum class ActionKind : uint8_t {
    None,
    Direct,
    DestructorWithObject,
    DestructorWithObjectPointer,
  } Kind = ActionKind::Direct;
  int32_t ObjectOffset = 0;
};

inline const char *
getCxxUnwindActionKindName(CxxUnwindAction::ActionKind Kind) {
  switch (Kind) {
  case CxxUnwindAction::ActionKind::None:
    return "none";
  case CxxUnwindAction::ActionKind::Direct:
    return "direct";
  case CxxUnwindAction::ActionKind::DestructorWithObject:
    return "destructor-object";
  case CxxUnwindAction::ActionKind::DestructorWithObjectPointer:
    return "destructor-object-pointer";
  }
  return "unknown";
}

struct CxxIPState {
  va_t IP = 0;
  int32_t State = -1;
};

struct CxxCatchHandler {
  uint32_t Adjectives = 0;
  va_t TypeDescriptorVA = 0;
  int32_t CatchObjectOffset = 0;
  va_t HandlerVA = 0;
  int32_t ParentFrameOffset = 0;
  std::vector<va_t> ContinuationVAs;
};

struct CxxTryBlock {
  int32_t TryLow = -1;
  int32_t TryHigh = -1;
  int32_t CatchHigh = -1;
  std::vector<CxxCatchHandler> Handlers;
};

/// One type named by a dynamic exception specification (`void f() throw(A)`).
/// MSVC spells the list with the same `HandlerType` record a catch clause
/// uses, so an entry carries the same adjectives and type descriptor; the
/// handler and catch-object fields are meaningless here and are not kept.
struct CxxExceptionSpecType {
  uint32_t Adjectives = 0;
  va_t TypeDescriptorVA = 0;
};

/// Which `FuncInfo` fields the record's magic declares.  MSVC only ever
/// appends, so a newer magic is a superset of an older one; what the version
/// decides is where the record *ends*, and therefore which trailing words are
/// part of it rather than whatever data follows in the section.
enum class CxxFuncInfoVersion : uint8_t {
  /// `EH_MAGIC_NUMBER1` (0x19930520): no exception-specification list and no
  /// `EHFlags`.
  Original,
  /// `EH_MAGIC_NUMBER2` (0x19930521): adds `pESTypeList`.
  WithExceptionSpecs,
  /// `EH_MAGIC_NUMBER3` (0x19930522): adds `EHFlags`.
  WithEHFlags,
};

struct CxxExceptionInfo {
  enum class Encoding : uint8_t {
    FH3,
    FH4,
  } NativeEncoding = Encoding::FH3;
  /// The 29-bit `magicNumber` field, with the three `bbtFlags` bits that share
  /// its word already split out into \ref BBTFlags.
  uint32_t Magic = 0;
  CxxFuncInfoVersion Version = CxxFuncInfoVersion::WithEHFlags;
  uint32_t Flags = 0;
  uint32_t MaxState = 0;
  int32_t UnwindHelpOffset = 0;
  va_t ESTypeListVA = 0;
  /// Decoded `ESTypeList`.  An empty vector with a nonzero \ref ESTypeListVA
  /// is `throw()`, which is a different contract from having no list at all.
  std::vector<CxxExceptionSpecType> ExceptionSpecTypes;
  uint32_t BBTFlags = 0;
  uint32_t FrameOffset = 0;
  bool IsCatchFunclet = false;
  bool IsSeparated = false;
  bool IsSynchronous = false;
  bool IsNoExcept = false;
  /// `FI_DYNSTKALIGN_FLAG`: the frame is dynamically aligned, so the unwinder
  /// reaches locals through an established frame pointer rather than from the
  /// stack pointer the unwind codes describe.
  bool HasDynamicStackAlignment = false;
  std::vector<CxxUnwindAction> UnwindMap;
  std::vector<CxxTryBlock> TryBlocks;
  std::vector<CxxIPState> IPMap;

  /// True when the function declares a dynamic exception specification, which
  /// only a record whose magic reaches `EH_MAGIC_NUMBER2` can do.
  bool hasExceptionSpecification() const { return ESTypeListVA != 0; }

  /// Validate normalized state relationships without consulting native table
  /// layout.  Each unwind transition must move to a strictly older state (or
  /// -1), and IP map entries must be strictly ordered and name valid states.
  bool hasValidStateGraph() const {
    if (UnwindMap.size() != MaxState)
      return false;
    for (size_t I = 0; I < UnwindMap.size(); ++I) {
      int32_t To = UnwindMap[I].ToState;
      if (To < -1 || To >= static_cast<int32_t>(I))
        return false;
    }
    for (size_t I = 0; I < IPMap.size(); ++I) {
      if (I != 0 && IPMap[I - 1].IP >= IPMap[I].IP)
        return false;
      if (IPMap[I].State < -1 ||
          IPMap[I].State >= static_cast<int32_t>(MaxState))
        return false;
    }
    for (const CxxTryBlock &Try : TryBlocks) {
      if (Try.TryLow < 0 || Try.TryHigh < Try.TryLow ||
          Try.CatchHigh <= Try.TryHigh ||
          Try.CatchHigh >= static_cast<int32_t>(MaxState))
        return false;
    }
    return true;
  }
};

/// GS wrapper data is intentionally opaque until its target-specific payload
/// has been validated.  Keeping the bytes and their status prevents a caller
/// from accidentally regenerating only the wrapped base personality.
/// `__GSHandlerData`: where in the frame the stack cookie lives, plus how the
/// CRT recomputes it.  The record's shape follows the target's pointer width,
/// because the flags occupy the low bits the cookie's frame offset cannot use.
struct GSCookieInfo {
  ExceptionParseStatus ParseStatus = ExceptionParseStatus::Partial;
  int32_t CookieOffset = 0;
  /// Only encoded on a 64-bit target, which has a third spare bit to spend on
  /// them.  A 32-bit record conveys the same thing by which wrapper is
  /// installed, so these stay false there rather than reading as "absent".
  bool HasExceptionHandler = false;
  bool HasUnwindHandler = false;
  /// The frame was dynamically aligned, so the cookie's slot is found relative
  /// to the realigned base rather than to the establisher frame.
  bool HasAlignment = false;
  /// Meaningful only alongside \ref HasAlignment on a 64-bit target: the 32-bit
  /// CRT derives the same adjustment arithmetically and stores nothing.
  int32_t AlignmentBaseOffset = 0;
  uint32_t Alignment = 0;
  std::vector<uint8_t> Payload;
};

enum class ExceptionalEdgeKind : uint8_t {
  SEHFilter,
  SEHHandler,
  SEHFinally,
  CxxCleanup,
  CxxCatch,
  /// Itanium landing pad that only runs destructors and resumes unwinding.
  ItaniumCleanupPad,
  /// Itanium landing pad that may stop the exception for a typed catch.
  ItaniumCatchPad,
  /// Itanium landing pad reached because an exception specification was
  /// violated, which calls `std::unexpected`/`std::terminate`.
  ItaniumSpecPad,
  /// Go `deferreturn` re-entry: the runtime resumes the frame here after
  /// running its deferred calls during a panic.
  GoDeferReturn,
  /// Go `recover` continuation.
  GoRecover,
  /// Delphi `try..finally` cleanup body.
  DelphiFinally,
  /// Delphi `try..except` body that catches anything, which is also what a
  /// `safecall` wrapper's automatic handler reaches.
  DelphiExcept,
  /// One `except on <class> do` arm.
  DelphiOnException,
  Unknown,
};

inline const char *getExceptionalEdgeKindName(ExceptionalEdgeKind Kind) {
  switch (Kind) {
  case ExceptionalEdgeKind::SEHFilter:
    return "seh-filter";
  case ExceptionalEdgeKind::SEHHandler:
    return "seh-handler";
  case ExceptionalEdgeKind::SEHFinally:
    return "seh-finally";
  case ExceptionalEdgeKind::CxxCleanup:
    return "cxx-cleanup";
  case ExceptionalEdgeKind::CxxCatch:
    return "cxx-catch";
  case ExceptionalEdgeKind::ItaniumCleanupPad:
    return "itanium-cleanup";
  case ExceptionalEdgeKind::ItaniumCatchPad:
    return "itanium-catch";
  case ExceptionalEdgeKind::ItaniumSpecPad:
    return "itanium-spec";
  case ExceptionalEdgeKind::GoDeferReturn:
    return "go-deferreturn";
  case ExceptionalEdgeKind::GoRecover:
    return "go-recover";
  case ExceptionalEdgeKind::DelphiFinally:
    return "delphi-finally";
  case ExceptionalEdgeKind::DelphiExcept:
    return "delphi-except";
  case ExceptionalEdgeKind::DelphiOnException:
    return "delphi-on";
  case ExceptionalEdgeKind::Unknown:
    return "unknown";
  }
  return "unknown";
}

/// IR-level exceptional transfer kept separate from ordinary CFG edges.  In a
/// successor list BlockId is the target block; in a predecessor list it is the
/// source block.  A value of -1 denotes a valid target outside the current
/// function/funclet while TargetVA retains the exact destination.
struct ExceptionalEdge {
  int BlockId = -1;
  va_t TargetVA = 0;
  ExceptionalEdgeKind Kind = ExceptionalEdgeKind::Unknown;
  uint32_t RegionIndex = 0;
  int32_t State = -1;

  bool operator==(const ExceptionalEdge &Other) const {
    return BlockId == Other.BlockId && TargetVA == Other.TargetVA &&
           Kind == Other.Kind && RegionIndex == Other.RegionIndex &&
           State == Other.State;
  }
};

struct ExceptionFunction {
  ExceptionAddressRange CodeRange;
  RuntimeFunctionKind Kind = RuntimeFunctionKind::Primary;
  ExceptionEncoding Encoding = ExceptionEncoding::Unknown;
  ExceptionParseStatus ParseStatus = ExceptionParseStatus::Complete;

  /// Native table provenance.  RVAs are retained for diagnostics and patch
  /// replacement; addresses are normalized image VAs for IR consumers.
  uint32_t RuntimeFunctionRVA = 0;
  uint32_t UnwindInfoRVA = 0;
  va_t UnwindInfoVA = 0;
  uint8_t UnwindVersion = 0;
  uint8_t UnwindFlags = 0;
  uint32_t PrologueSize = 0;
  uint16_t FrameRegister = 0;
  uint32_t FrameOffset = 0;
  uint32_t PackedUnwindData = 0;
  std::vector<uint8_t> NativeUnwindBytes;
  std::vector<UnwindOperation> UnwindOperations;
  std::vector<UnwindEpilog> Epilogs;

  va_t PersonalityVA = 0;
  va_t HandlerDataVA = 0;
  ExceptionPersonality Personality = ExceptionPersonality::None;
  std::string PersonalityName;
  std::optional<SEHExceptionInfo> SEH;
  std::optional<CxxExceptionInfo> Cxx;
  std::optional<GSCookieInfo> GSCookie;

  /// Itanium model: the DWARF frame description and its LSDA.
  std::optional<DwarfFDE> Dwarf;
  std::optional<ItaniumEHInfo> Itanium;
  /// ARM EHABI model: the index entry and the `.ARM.extab` entry it reached.
  /// Present beside \ref Itanium rather than instead of it, because a C++
  /// frame on this target keeps its language data inside the EHABI entry --
  /// the two describe one frame and neither is readable without the other.
  std::optional<ARMEHABIInfo> ARMEHABI;
  /// Darwin compact-unwind entry covering this range.
  std::optional<CompactUnwindEntry> Compact;
  /// x86-32 registration model: the chain the prologue installed.
  std::optional<RegistrationChainInfo> Registration;
  /// Delphi x86-32 model: the `TExcFrame` the prologue linked, present instead
  /// of \ref Registration because such a frame has no scope table to fill it.
  std::optional<DelphiFrameInfo> Delphi;
  /// Delphi x86-64 model: the `TExcData` scope array in the handler data.
  /// Delphi dropped the registration chain on this target, so a frame carries
  /// one of these or a \ref Delphi record but never both.
  std::optional<DelphiScopeTable> DelphiScopes;
  /// Go model: the runtime's frame metadata for this function.
  std::optional<GoFunctionEH> Go;
  /// Rust reading of whichever table model this record already carries.  Rust
  /// shares the Itanium LSDA with C++ and the MSVC `FuncInfo` with Windows
  /// C++, so this does not replace either -- it says what the shared structure
  /// means for a Rust frame.
  std::optional<RustFunctionEH> Rust;
  /// Objective-C reading of whichever table model this record already carries.
  /// Like \ref Rust this annotates a shared structure rather than replacing
  /// it: Objective-C borrows the Itanium LSDA and, on `*-windows-msvc`, the
  /// MSVC `FuncInfo`, and what differs is how the type table is read and what
  /// a pad's runtime calls say it is for.
  std::optional<ObjCFunctionEH> ObjC;

  /// Index of the primary record for a chained/fragment record, when known.
  std::optional<size_t> PrimaryFunctionIndex;
  std::optional<ExceptionAddressRange> ChainedPrimaryRange;
  uint32_t ChainedUnwindInfoRVA = 0;
  std::vector<std::string> Diagnostics;

  ExceptionModel model() const { return getExceptionEncodingModel(Encoding); }

  /// True when this record carries a decoded language table of any model.
  bool hasLanguageTable() const {
    return SEH.has_value() || Cxx.has_value() || Itanium.has_value() ||
           Registration.has_value() || Delphi.has_value() ||
           DelphiScopes.has_value() || Go.has_value();
  }

  bool canRegenerateLanguageMetadata() const {
    // Native regeneration is currently a Windows-table capability.  Every
    // other model is decoded and structured but must not authorize a rewrite
    // that would have to reproduce a contract NeverD cannot yet emit.
    if (model() != ExceptionModel::WindowsTable)
      return false;
    return ParseStatus == ExceptionParseStatus::Complete &&
           !isGSWrappedPersonality(Personality) &&
           Personality != ExceptionPersonality::CxxFrameHandler4 &&
           Encoding != ExceptionEncoding::X64UnwindV3;
  }
};

/// Image-wide exception table plus a stable address index.  Runtime records
/// may overlap because chained entries and ARM fragments describe one logical
/// function, so lookup returns the most specific containing range.
struct ExceptionInfo {
  std::vector<ExceptionFunction> Functions;
  ExceptionParseStatus ParseStatus = ExceptionParseStatus::Complete;
  uint32_t DirectoryRVA = 0;
  uint32_t DirectorySize = 0;
  std::vector<size_t> FunctionIndex;
  std::vector<std::string> Diagnostics;

  /// Models present in this image.  A single image legitimately carries more
  /// than one: a MinGW PE has both `.pdata` and Itanium tables, and a Go
  /// program that links cgo has Go frames beside DWARF ones.
  std::vector<ExceptionModel> Models;

  /// Which language runtime produced this image, classified from its sections,
  /// symbols, and embedded runtime banners.  A model does not imply a runtime:
  /// an Itanium LSDA is emitted by C, C++, and Rust alike, so a consumer that
  /// needs to know what a landing pad *does* reads this rather than inferring
  /// it from \ref Models.
  LanguageRuntimeInfo Runtime;

  /// Every CIE referenced by a decoded FDE, keyed by its section offset.  CIEs
  /// are shared by many FDEs, so they are stored once here rather than copied
  /// into each function record.
  std::vector<DwarfCIE> CIEs;

  /// Go runtime module state, present when the image carries a `pclntab`.
  /// Held once per image because every Go function record resolves its offsets
  /// against these bases.
  std::optional<GoModuleInfo> GoModule;

  /// Rust panic machinery, present when the image links the Rust runtime.
  std::optional<RustRuntimeInfo> RustRuntime;

  /// Objective-C exception machinery, present when the image links one of the
  /// Objective-C runtimes.
  std::optional<ObjCRuntimeInfo> ObjCRuntime;

  /// Section offset -> index into \ref CIEs.
  const DwarfCIE *findCIE(uint64_t SectionOffset) const {
    for (const DwarfCIE &CIE : CIEs)
      if (CIE.SectionOffset == SectionOffset)
        return &CIE;
    return nullptr;
  }

  bool hasModel(ExceptionModel Model) const {
    return std::find(Models.begin(), Models.end(), Model) != Models.end();
  }

  void addModel(ExceptionModel Model) {
    if (Model != ExceptionModel::None && !hasModel(Model))
      Models.push_back(Model);
  }

  void rebuildIndex() {
    FunctionIndex.resize(Functions.size());
    for (size_t I = 0; I < Functions.size(); ++I)
      FunctionIndex[I] = I;
    std::stable_sort(FunctionIndex.begin(), FunctionIndex.end(),
                     [&](size_t A, size_t B) {
                       const auto &RA = Functions[A].CodeRange;
                       const auto &RB = Functions[B].CodeRange;
                       if (RA.Begin != RB.Begin)
                         return RA.Begin < RB.Begin;
                       return RA.size() < RB.size();
                     });
  }

  const ExceptionFunction *findFunction(va_t Address) const {
    const ExceptionFunction *Best = nullptr;
    for (size_t I : FunctionIndex) {
      if (I >= Functions.size())
        continue;
      const ExceptionFunction &F = Functions[I];
      if (F.CodeRange.Begin > Address)
        break;
      if (!F.CodeRange.contains(Address))
        continue;
      if (!Best || F.CodeRange.size() < Best->CodeRange.size())
        Best = &F;
    }
    return Best;
  }

  ExceptionFunction *findFunction(va_t Address) {
    return const_cast<ExceptionFunction *>(
        static_cast<const ExceptionInfo *>(this)->findFunction(Address));
  }
};

} // namespace neverd

#endif // NEVERD_LOADER_EXCEPTIONINFO_H

//===- ExceptionPersonality.h - Personality identity ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The exact personality identity a frame installs, and the predicates that
/// classify one.  Two personalities that share a table schema stay distinct
/// enumerators whenever their run-time contract differs, so the predicates
/// here are what a consumer asks instead of comparing spellings.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_EXCEPTIONPERSONALITY_H
#define NEVERD_LOADER_EXCEPTIONPERSONALITY_H

#include "neverd/loader/LanguageEHObjC.h"

#include <cstdint>
#include <optional>

namespace neverd {

/// What a positive Itanium action filter points at for one personality.
///
/// The LSDA byte layout is shared by several language runtimes, but the object
/// named by a type-table slot is not.  Following every non-null entry as
/// `std::type_info` is therefore unsafe: an Ada `Exception_Id` and a D
/// `ClassInfo` can both contain a pointer-looking second word that would yield
/// a convincing name from unrelated data.
enum class ItaniumTypeTableEntryKind : uint8_t {
  /// A `std::type_info` object or a layout-compatible Objective-C descriptor.
  CxxRTTI,
  /// A class-name string stored directly in the slot.
  DirectCString,
  /// A runtime-owned descriptor whose identity is preserved but not decoded.
  OpaqueDescriptor,
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

/// Return the language-defined interpretation of an Itanium type-table slot.
///
/// Unknown personalities are deliberately opaque.  The personality routine is
/// the version tag for an LSDA; without its identity there is no evidence that
/// a pointer in the table follows the C++ RTTI layout.
inline ItaniumTypeTableEntryKind
getItaniumTypeTableEntryKind(ExceptionPersonality P) {
  switch (P) {
  case ExceptionPersonality::GxxPersonalityV0:
  case ExceptionPersonality::GxxPersonalitySEH0:
  case ExceptionPersonality::GxxPersonalitySJ0:
  case ExceptionPersonality::ObjCPersonalityV0:
  case ExceptionPersonality::GNUstepObjCXXPersonalityV0:
    return ItaniumTypeTableEntryKind::CxxRTTI;
  case ExceptionPersonality::GnuObjCPersonalityV0:
  case ExceptionPersonality::GnuObjCPersonalitySEH0:
  case ExceptionPersonality::GnuObjCPersonalitySJ0:
  case ExceptionPersonality::GNUstepObjCPersonalityV0:
    return ItaniumTypeTableEntryKind::DirectCString;
  default:
    return ItaniumTypeTableEntryKind::OpaqueDescriptor;
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

} // namespace neverd

#endif // NEVERD_LOADER_EXCEPTIONPERSONALITY_H

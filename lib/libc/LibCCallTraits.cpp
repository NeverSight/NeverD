//===- LibCCallTraits.cpp - libc/POSIX call trait lookup ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// libc/POSIX variadic, control-flow, and memory call trait recognition.
///
//===----------------------------------------------------------------------===//

#include "neverd/Common.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringSwitch.h"

#include <string_view>

namespace neverd::libc {
namespace {

// --- Variadic / va_list signature tables ---
//
// Most of the printf/scanf family's fixed-parameter count follows from the name
// suffix and is computed by pattern below; the handful of irregular cases that
// cannot be patterned live in the declarative tables here, kept table-driven in
// the same style as the kXxxArity tables rather than scattered as inline string
// comparisons inside varArgFixedCount/isVaListConsumer.

unsigned fortifiedChkFixedCount(std::string_view Core) {
  return llvm::StringSwitch<unsigned>(Core)
#define LIBC_FORTIFIED_CHK_FIXED(Name, Count) .Case(Name, Count)
#include "neverd/libc/LibCFortifiedChkFixed.inc"
#undef LIBC_FORTIFIED_CHK_FIXED
      .Default(0);
}

unsigned irregularVarArgFixedCount(std::string_view Name) {
  return llvm::StringSwitch<unsigned>(Name)
#define LIBC_IRREGULAR_VARARG(Name, Count) .Case(Name, Count)
#include "neverd/libc/LibCIrregularVarArg.inc"
#undef LIBC_IRREGULAR_VARARG
      .Default(0);
}

} // anonymous namespace

unsigned varArgFixedCount(std::string_view Name) {
  // Modern Darwin linkers specialize Objective-C dispatch through local
  // `_objc_msgSend$<selector>` stubs.  The stub materializes `_cmd` in x1,
  // while the caller supplies the receiver in x0 and one argument per colon
  // starting at x2.  Model the colon-counted prefix as fixed and leave an
  // ellipsis after it: ordinary methods simply pass no tail, while methods
  // such as stringWithFormat: preserve their true stack-passed varargs.
  constexpr std::string_view ObjCMsgSendPrefix = "objc_msgSend$";
  if (Name.starts_with(ObjCMsgSendPrefix)) {
    std::string_view Selector = Name.substr(ObjCMsgSendPrefix.size());
    if (Selector.empty())
      return 0;
    unsigned Fixed = 2; // receiver and linker-supplied _cmd
    for (char C : Selector)
      if (C == ':')
        ++Fixed;
    return Fixed;
  }

  // Fortified _FORTIFY_SOURCE variants: __<core>_chk.  clang emits these for
  // the buffer-bounded printf family when the destination size is known
  // (default on macOS / glibc).  The caller has stripped at most one leading
  // '_' (a Mach-O symbol prefix), so the name arrives as "__snprintf_chk"
  // (Mach-O) or
  // "_snprintf_chk" (ELF over-strip); normalize by dropping every leading
  // underscore, then map the core name through LibCFortifiedChkFixed.inc.
  if (Name.ends_with("_chk")) {
    std::string_view Core = Name.substr(0, Name.size() - 4);
    while (!Core.empty() && Core.front() == '_')
      Core.remove_prefix(1);
    // v*_chk take a va_list, not "...", so they are not variadic.
    if (!Core.empty() && Core.front() == 'v')
      return 0;
    return fortifiedChkFixedCount(Core);
  }

  // The *printf / *scanf families: the fixed-parameter count follows from the
  // name suffix (covers printf, fprintf, snprintf, the wide w* forms, and
  // platform variants like _snprintf / swprintf).
  if (Name.ends_with("printf") || Name.ends_with("scanf")) {
    // v*printf / v*scanf take a va_list, NOT "..." — they are not variadic.
    if (Name.starts_with('v') && (Name.substr(1).ends_with("printf") ||
                                  Name.substr(1).ends_with("scanf")))
      return 0;
    // "printf" / "scanf" (and the wide w* forms) alone → 1 fixed (format
    // string).
    if (unsigned Count = llvm::StringSwitch<unsigned>(Name)
#define LIBC_VARARG_FORMAT_NAME(Name, FixedCount) .Case(Name, FixedCount)
#include "neverd/libc/LibCVarArgFormatNames.inc"
#undef LIBC_VARARG_FORMAT_NAME
                             .Default(0))
      return Count;
    // "snprintf", "snwprintf", "swprintf" → 3 fixed (buf + size + fmt).  C99
    // swprintf has the same signature shape as snprintf.
    if (Name.substr(0, 2) == "sn" || Name == "swprintf")
      return 3;
    // "fprintf", "fscanf", "sprintf", "sscanf", "dprintf", "asprintf", … → 2.
    return 2;
  }

  // Irregular variadic functions with no printf/scanf suffix.
  return irregularVarArgFixedCount(Name);
}

VarArgFixedParamKind varArgFixedParamKind(std::string_view Name,
                                          unsigned Index) {
  Name = stripLeadingUnderscores(Name);
  const unsigned FixedCount = varArgFixedCount(Name);
  if (Index >= FixedCount)
    return VarArgFixedParamKind::Unknown;

  constexpr std::string_view ObjCMsgSendPrefix = "objc_msgSend$";
  if (Name.starts_with(ObjCMsgSendPrefix)) {
    // The receiver and linker-supplied _cmd are pointers.  Selector arguments
    // can be arbitrary scalars, pointers, or aggregates, so retain the type
    // recovered from their actual registers.
    return Index < 2 ? VarArgFixedParamKind::Pointer
                     : VarArgFixedParamKind::Unknown;
  }

  if (Name.ends_with("_chk")) {
    std::string_view Core = Name.substr(0, Name.size() - 4);
    while (!Core.empty() && Core.front() == '_')
      Core.remove_prefix(1);

    if (Core == "printf")
      return Index == 0 ? VarArgFixedParamKind::Integer
                        : VarArgFixedParamKind::Pointer;
    if (Core == "fprintf" || Core == "asprintf")
      return Index == 1 ? VarArgFixedParamKind::Integer
                        : VarArgFixedParamKind::Pointer;
    if (Core == "dprintf")
      return Index < 2 ? VarArgFixedParamKind::Integer
                       : VarArgFixedParamKind::Pointer;
    if (Core == "sprintf")
      return Index == 0 || Index == 3 ? VarArgFixedParamKind::Pointer
                                      : VarArgFixedParamKind::Integer;
    if (Core == "snprintf")
      return Index == 0 || Index == 4 ? VarArgFixedParamKind::Pointer
                                      : VarArgFixedParamKind::Integer;
    return VarArgFixedParamKind::Unknown;
  }

  if (Name.ends_with("printf") || Name.ends_with("scanf")) {
    if (FixedCount == 1)
      return VarArgFixedParamKind::Pointer;
    if (FixedCount == 3)
      return Index == 1 ? VarArgFixedParamKind::Integer
                        : VarArgFixedParamKind::Pointer;
    if (FixedCount == 2) {
      // dprintf's file descriptor is the only scalar first parameter in the
      // ordinary two-fixed-parameter printf/scanf family.
      if (Name == "dprintf" && Index == 0)
        return VarArgFixedParamKind::Integer;
      return VarArgFixedParamKind::Pointer;
    }
  }

  // The irregular registry intentionally carries only the fixed prefix the
  // recovery pass can prove.  Classify that prefix without guessing the types
  // of any optional tail arguments.
  if (Name == "open")
    return Index == 0 ? VarArgFixedParamKind::Pointer
                      : VarArgFixedParamKind::Integer;
  if (Name == "syslog" || Name == "err" || Name == "errx" || Name == "openat" ||
      Name == "fcntl" || Name == "ioctl")
    return VarArgFixedParamKind::Integer;
  if (Name == "warn" || Name == "warnx" || Name == "execl" ||
      Name == "execlp" || Name == "execle" || Name == "mq_open" ||
      Name == "sem_open")
    return VarArgFixedParamKind::Pointer;

  return VarArgFixedParamKind::Unknown;
}

bool isVaListConsumer(std::string_view Name) {
  // Fortified __v*_chk forms (e.g. __vsnprintf_chk) take a va_list just like
  // their plain v* base; strip the suffix and fall through to the v* check.
  if (Name.ends_with("_chk"))
    Name = Name.substr(0, Name.size() - 4);
  // The v-prefixed printf/scanf family: the trailing argument is a va_list, so
  // any function that routes its own varargs into one of these is variadic.
  if (Name.starts_with('v') &&
      (Name.ends_with("printf") || Name.ends_with("scanf")))
    return true;
  // Irregular va_list consumers (no printf/scanf suffix).
  return llvm::StringSwitch<bool>(Name)
#define LIBC_VA_LIST_CONSUMER(Name) .Case(Name, true)
#include "neverd/libc/LibCVaListConsumers.inc"
#undef LIBC_VA_LIST_CONSUMER
      .Default(false);
}

bool isNoReturnFunction(std::string_view Name) {
  // Every entry is unconditionally __attribute__((noreturn)) in its standard
  // header.  Names whose canonical form keeps a leading underscore appear here
  // already underscore-stripped (_exit -> "exit", _Exit -> "Exit").  Functions
  // that only *sometimes* return (warn/warnx, GNU error/error_at_line) are
  // deliberately excluded so the CFG builder never drops genuinely reachable
  // fall-through code.
  return llvm::StringSwitch<bool>(stripLeadingUnderscores(Name))
#define LIBC_NO_RETURN_SYMBOL(Name) .Case(Name, true)
#include "neverd/libc/LibCNoReturn.inc"
#undef LIBC_NO_RETURN_SYMBOL
      .Default(false);
}

bool isNoReturnTarget(const BinaryImage &Img, va_t Target) {
  if (Target == InvalidVA)
    return false;
  if (const Import *Imp = Img.findImportAt(Target))
    if (isNoReturnFunction(Imp->Name))
      return true;
  if (const Symbol *Sym = Img.findSymbolAt(Target))
    if (isNoReturnFunction(Sym->Name))
      return true;
  return false;
}

bool isReturnsTwiceFunction(std::string_view Name) {
  // setjmp / _setjmp / sigsetjmp / __sigsetjmp all normalize to one of these.
  return llvm::StringSwitch<bool>(stripLeadingUnderscores(Name))
#define LIBC_RETURNS_TWICE_SYMBOL(Name) .Case(Name, true)
#include "neverd/libc/LibCReturnsTwice.inc"
#undef LIBC_RETURNS_TWICE_SYMBOL
      .Default(false);
}

bool isMemCopyName(std::string_view Name) {
  return llvm::StringSwitch<bool>(stripLeadingUnderscores(Name))
#define LIBC_MEM_COPY_SYMBOL(Name) .Case(Name, true)
#include "neverd/libc/LibCMemCopyNames.inc"
#undef LIBC_MEM_COPY_SYMBOL
      .Default(false);
}

bool isMemSetName(std::string_view Name) {
  return llvm::StringSwitch<bool>(stripLeadingUnderscores(Name))
#define LIBC_MEM_SET_SYMBOL(Name) .Case(Name, true)
#include "neverd/libc/LibCMemSetNames.inc"
#undef LIBC_MEM_SET_SYMBOL
      .Default(false);
}

} // namespace neverd::libc

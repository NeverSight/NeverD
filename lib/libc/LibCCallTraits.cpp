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

#include "LibCVariantRows.h"

#include "neverd/Common.h"
#include "neverd/libc/LibCFortify.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/libc/NoReturnLoadedTarget.h"
#include "neverd/libc/StackProbeIndex.h"
#include "neverd/libc/WindowsCRT.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/LanguageRuntime.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringSwitch.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string_view>
#include <unordered_map>

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

/// The category of each fixed parameter of the irregular variadic routine
/// \p Name, or none for any other routine.
llvm::ArrayRef<VarArgFixedParamKind>
irregularVarArgFixedKinds(std::string_view Name) {
  using enum VarArgFixedParamKind;
#define LIBC_IRREGULAR_VARARG(Routine, ...)                                    \
  if (Name == Routine) {                                                       \
    static constexpr VarArgFixedParamKind Kinds[] = {__VA_ARGS__};             \
    return Kinds;                                                              \
  }
#include "neverd/libc/LibCIrregularVarArg.inc"
#undef LIBC_IRREGULAR_VARARG
  return {};
}

/// The standard routine whose arguments a C library's variant \p Name takes
/// or implements, without leading underscores: glibc's ISO C conforming
/// alias `__isoc99_scanf` takes scanf's, and the Universal CRT's
/// `__stdio_common_vfprintf` implements vfprintf.  Otherwise \p Name itself.
std::string_view isoAliasTarget(std::string_view Name) {
  const std::string_view Bare = stripLeadingUnderscores(Name);
  for (const std::string_view Prefix : kIsoAliasPrefixes)
    if (Bare.starts_with(Prefix))
      return Bare.substr(Prefix.size());
  for (const auto Prefix : kUCRTCommonPrefixes)
    if (Bare.starts_with(Prefix)) {
      std::string_view Routine = Bare.substr(Prefix.size());
      for (const auto Suffix : kUCRTCommonSuffixes)
        if (Routine.ends_with(Suffix))
          Routine.remove_suffix(Suffix.size());
      return Routine;
    }
  return Name;
}

} // anonymous namespace

unsigned varArgFixedCount(std::string_view Name, LibCVariant Variant) {
  Name = routineOfSymbol(Name, Variant);
  if (const std::string_view Routine = largeFileRoutine(Name); !Routine.empty())
    Name = Routine;
  if (const std::string_view Standard = isoAliasTarget(Name); Standard != Name)
    return varArgFixedCount(Standard, Variant);
  if (Variant != LibCVariant::GNU)
    if (const auto Kinds =
            variant_rows::variadic(stripLeadingUnderscores(Name), Variant))
      return Kinds->size();
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

  // A routine that takes its further arguments as a va_list is not variadic,
  // whatever its name ends in: obstack_vprintf, Darwin's __svfscanf.
  if (isVaListConsumer(stripLeadingUnderscores(Name)))
    return 0;

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
  return irregularVarArgFixedKinds(Name).size();
}

VarArgFixedParamKind varArgFixedParamKind(std::string_view Name, unsigned Index,
                                          LibCVariant Variant) {
  Name =
      isoAliasTarget(stripLeadingUnderscores(routineOfSymbol(Name, Variant)));
  if (Variant != LibCVariant::GNU)
    if (const auto Kinds = variant_rows::variadic(Name, Variant))
      return Index < Kinds->size() ? (*Kinds)[Index]
                                   : VarArgFixedParamKind::Unknown;
  const unsigned FixedCount = varArgFixedCount(Name, Variant);
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

  // The irregular registry lists every fixed parameter its declaration names,
  // and no optional tail argument.
  const llvm::ArrayRef<VarArgFixedParamKind> Irregular =
      irregularVarArgFixedKinds(Name);
  return Index < Irregular.size() ? Irregular[Index]
                                  : VarArgFixedParamKind::Unknown;
}

bool isVaListConsumer(std::string_view Name) {
  Name = isoAliasTarget(Name);
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

bool isCStringParameter(std::string_view Name, unsigned Index) {
  struct StringParam {
    std::string_view Name;
    unsigned Index;
  };
  static constexpr StringParam StringParams[] = {
#define LIBC_STRING_PARAM(Name, Index) {Name, Index},
#include "neverd/libc/LibCStringParams.inc"
#undef LIBC_STRING_PARAM
  };
  // A row names the routine as its header declares it (`__fwriting`); the
  // lookup ignores leading underscores on both sides, as the variant rows'.
  // A large-file routine takes its strings where its routine does.
  std::string_view Bare = stripLeadingUnderscores(Name);
  if (const std::string_view Routine = largeFileRoutine(Bare); !Routine.empty())
    Bare = Routine;
  for (const StringParam &Param : StringParams)
    if (Param.Index == Index &&
        std::string_view(stripLeadingUnderscores(Param.Name)) == Bare)
      return true;
  return false;
}

std::optional<std::string_view> functionPointerParameter(std::string_view Name,
                                                         unsigned Index,
                                                         LibCVariant Variant) {
  struct FunctionPointerParam {
    std::string_view Name;
    unsigned Index;
    std::string_view Type;
  };
  static constexpr FunctionPointerParam FunctionPointerParams[] = {
#define LIBC_FUNCTION_POINTER_PARAM(Name, Index, Type) {Name, Index, Type},
#include "neverd/libc/LibCFunctionPointerParams.inc"
#undef LIBC_FUNCTION_POINTER_PARAM
  };
  // A large-file routine has rows of its own: its function's type names the
  // large-file records (scandir64's comparison takes struct dirent64).
  const std::string_view Bare =
      stripLeadingUnderscores(routineOfSymbol(Name, Variant));
  if (Variant != LibCVariant::GNU)
    if (const auto Pointers = variant_rows::pointers(Bare, Variant))
      return Pointers->FunctionPointer == static_cast<int>(Index)
                 ? std::optional<std::string_view>(Pointers->Type)
                 : std::nullopt;
  for (const FunctionPointerParam &Param : FunctionPointerParams)
    if (Param.Index == Index &&
        std::string_view(stripLeadingUnderscores(Param.Name)) == Bare)
      return Param.Type;
  return std::nullopt;
}

bool isObjectPointerParameter(std::string_view Name, unsigned Index,
                              LibCVariant Variant) {
  // Each function's object pointer parameters, as a mask of positions, by
  // its name without leading underscores: a row names the routine as its
  // header declares it (`__fwriting`), and the lookup strips the query's.
  static const std::unordered_map<std::string_view, uint64_t> Masks = [] {
    std::unordered_map<std::string_view, uint64_t> Map;
#define LIBC_OBJECT_POINTER_PARAM(Name, Index)                                 \
  Map[std::string_view(stripLeadingUnderscores(Name))] |= uint64_t(1)          \
                                                          << (Index);
#include "neverd/libc/LibCObjectPointerParams.inc"
#undef LIBC_OBJECT_POINTER_PARAM
    return Map;
  }();
  if (Index >= 64)
    return false;
  std::string_view Bare =
      stripLeadingUnderscores(routineOfSymbol(Name, Variant));
  if (const std::string_view Routine = largeFileRoutine(Bare); !Routine.empty())
    Bare = Routine;
  if (Variant != LibCVariant::GNU)
    if (const auto Pointers = variant_rows::pointers(Bare, Variant))
      return Pointers->ObjectPointers >> Index & 1;
  const auto It = Masks.find(Bare);
  return It != Masks.end() && (It->second >> Index & 1);
}

namespace {
struct StackProbeRoutine {
  BinaryFormat Format;
  Arch Architecture;
  std::string_view Symbol;
  StackProbeEffect Effect;
};

constexpr StackProbeRoutine StackProbeRoutines[] = {
#define STACK_PROBE(Format, Architecture, Symbol, Effect)                      \
  {BinaryFormat::Format, Arch::Architecture, Symbol, StackProbeEffect::Effect},
#include "neverd/libc/StackProbeRoutines.inc"
};

bool stackProbeApplies(const StackProbeRoutine &R, const BinaryImage &Img) {
  return R.Format == Img.abiFormat() && R.Architecture == Img.Arch;
}

bool hasStackProbeRoutines(const BinaryImage &Img) {
  return std::any_of(
      std::begin(StackProbeRoutines), std::end(StackProbeRoutines),
      [&](const StackProbeRoutine &R) { return stackProbeApplies(R, Img); });
}

/// The helper \p Name spells for \p Img's format and architecture.
std::optional<StackProbeEffect> stackProbeNamed(const BinaryImage &Img,
                                                std::string_view Name) {
  for (const StackProbeRoutine &R : StackProbeRoutines)
    if (!Name.empty() && stackProbeApplies(R, Img) && R.Symbol == Name)
      return R.Effect;
  return std::nullopt;
}
} // namespace

std::optional<StackProbeEffect> stackProbeEffect(const BinaryImage &Img,
                                                 va_t Target) {
  if (Img.functionHasBytePatch(Target))
    return std::nullopt;
  if (!hasStackProbeRoutines(Img))
    return std::nullopt;
  // Only a name the binary or its debug information states proves the
  // helper; one an analysis guessed does not drop a call.
  std::string_view Name;
  if (const Export *Exp = Img.findExportAt(Target); Exp && !Exp->Name.empty())
    Name = Exp->Name;
  else if (const Symbol *Sym = Img.findSymbolAt(Target);
           Sym && Sym->Origin >= NameOrigin::Stated)
    Name = Sym->Name;
  return stackProbeNamed(Img, Name);
}

StackProbeIndex::StackProbeIndex(const BinaryImage &Img) : Image(&Img) {
  if (!hasStackProbeRoutines(Img))
    return;
  std::set<va_t> Named;
  for (const Export &Exp : Img.Exports)
    if (stackProbeNamed(Img, Exp.Name))
      Named.insert(Exp.Addr);
  for (const Symbol &Sym : Img.Symbols)
    if (stackProbeNamed(Img, Sym.Name))
      Named.insert(Sym.Addr);
  for (va_t Addr : Named)
    if (const auto Effect = stackProbeEffect(Img, Addr))
      Probes.emplace(Addr, *Effect);
}

std::optional<StackProbeEffect> StackProbeIndex::effect(const BinaryImage &Img,
                                                        va_t Target) const {
  if (Image != &Img)
    return stackProbeEffect(Img, Target);
  const auto It = Probes.find(Target);
  if (It == Probes.end())
    return std::nullopt;
  return It->second;
}

std::optional<StackProbeEffect> stackProbeEffect(const BinaryImage &Img,
                                                 va_t Target,
                                                 const StackProbeIndex *Index) {
  return Index ? Index->effect(Img, Target) : stackProbeEffect(Img, Target);
}

namespace {
/// Whether \p Name names a routine of Rust's standard library whose
/// declaration returns `!` (RustRuntimeNoReturn.inc).  A mangled name is
/// matched by its demangled path without generic arguments; an unmangled one
/// by its name.
bool isRustNoReturnRoutine(std::string_view Name) {
  const llvm::StringRef Symbol(Name.data(), Name.size());
  const std::string Demangled = demangleRustName(Symbol);
  llvm::StringRef Path = Demangled;
  if (Demangled.empty()) {
    const std::string_view Bare = stripLeadingUnderscores(Name);
    Path = llvm::StringRef(Bare.data(), Bare.size());
  } else if (const size_t Generic = Path.find("::<");
             Generic != llvm::StringRef::npos) {
    Path = Path.take_front(Generic);
  }
  return llvm::StringSwitch<bool>(Path)
#define RUST_NO_RETURN_PATH(Path) .Case(Path, true)
#define RUST_NO_RETURN_PATH_PREFIX(Prefix) .StartsWith(Prefix, true)
#include "neverd/libc/RustRuntimeNoReturn.inc"
#undef RUST_NO_RETURN_PATH_PREFIX
#undef RUST_NO_RETURN_PATH
      .Default(false);
}
} // namespace

bool isNoReturnFunction(std::string_view Name) {
  // Every entry is unconditionally __attribute__((noreturn)) in its standard
  // header.  Names whose canonical form keeps a leading underscore appear here
  // already underscore-stripped (_exit -> "exit", _Exit -> "Exit").  Functions
  // that only *sometimes* return (warn/warnx, GNU error/error_at_line) are
  // deliberately excluded so the CFG builder never drops genuinely reachable
  // fall-through code.
  const bool Listed = llvm::StringSwitch<bool>(stripLeadingUnderscores(Name))
#define LIBC_NO_RETURN_SYMBOL(Name) .Case(Name, true)
#include "neverd/libc/CxxRuntimeNoReturn.inc"
#include "neverd/libc/LibCNoReturn.inc"
#include "neverd/libc/WindowsNoReturn.inc"
#undef LIBC_NO_RETURN_SYMBOL
                          .Default(false);
  return Listed || isRustNoReturnRoutine(Name);
}

namespace {
template <typename... Widths>
constexpr WindowsKernelPrototype makeWindowsPrototype(std::string_view Name,
                                                      Widths... W) {
  static_assert(sizeof...(W) <= 16, "too many prototype parameters");
  WindowsKernelPrototype P{
      Name, {static_cast<uint8_t>(W)...}, static_cast<uint8_t>(sizeof...(W))};
  return P;
}
constexpr WindowsKernelPrototype WindowsKernelPrototypes[] = {
#define ND_WINDOWS_DISPATCH_THUNK(Name)
#define ND_WINDOWS_PROTO(Name, ...) makeWindowsPrototype(Name, ##__VA_ARGS__),
#include "neverd/libc/WindowsKernelRoutines.inc"
#undef ND_WINDOWS_PROTO
#undef ND_WINDOWS_DISPATCH_THUNK
};
} // namespace

const WindowsKernelPrototype *windowsKernelPrototype(std::string_view Name) {
  // The first prototype the table gives each name; a run asks for every
  // callee and import it summarizes.
  static const auto ByName = [] {
    std::unordered_map<std::string_view, const WindowsKernelPrototype *> Index;
    for (const WindowsKernelPrototype &P : WindowsKernelPrototypes)
      Index.try_emplace(P.Name, &P);
    return Index;
  }();
  const auto Found = ByName.find(stripLeadingUnderscores(Name));
  return Found == ByName.end() ? nullptr : Found->second;
}

bool isIndirectCallDispatchThunk(std::string_view Name) {
  return llvm::StringSwitch<bool>(stripLeadingUnderscores(Name))
#define ND_WINDOWS_DISPATCH_THUNK(Name) .Case(Name, true)
#define ND_WINDOWS_PROTO(Name, ...)
#include "neverd/libc/WindowsKernelRoutines.inc"
#undef ND_WINDOWS_PROTO
#undef ND_WINDOWS_DISPATCH_THUNK
      .Default(false);
}

namespace {
/// A routine \p Imp's module exports under a name a program may also give a
/// returning routine of its own (WindowsNoReturnImports.inc).
bool isNoReturnModuleExport(const Import &Imp) {
  const llvm::StringRef Module(Imp.Module);
#define LIBC_NO_RETURN_IMPORT(Library, Routine)                                \
  if (Imp.Name == Routine && Module.equals_insensitive(Library))               \
    return true;
#include "neverd/libc/WindowsNoReturnImports.inc"
#undef LIBC_NO_RETURN_IMPORT
  return false;
}
} // namespace

bool isNoReturnImport(const Import &Imp) {
  return isNoReturnFunction(Imp.Name) || isNoReturnModuleExport(Imp);
}

bool isNoReturnTarget(const BinaryImage &Img, va_t Target) {
  if (Target == InvalidVA)
    return false;
  // The user's statement decides over every name.
  if (const auto Stated = Img.userNoReturnAt(Target))
    return *Stated;
  if (Img.functionHasBytePatch(Target))
    return false;
  const Import *Imp = Img.findImportAt(Target);
  if (Imp)
    if (isNoReturnImport(*Imp))
      return true;
  // A pointer slot the loader binds to an import (an ELF GLOB_DAT or
  // JUMP_SLOT, a Mach-O bind): x86 calls through it as `call [slot]`.
  // An exact IAT identity takes precedence over another storage spelling,
  // including a conflicting no-return name, as in NoReturnTargetIndex.
  if (!Imp || Imp->IATAddr != Target) {
    const ImportStorageSlotCollection Storage =
        Img.collectImportStorageSlot(Target);
    if (auto It = Storage.Slots.find(Target);
        It != Storage.Slots.end() && !Storage.Conflicts.count(Target) &&
        It->second.Addend == 0 && isNoReturnFunction(It->second.Name))
      return true;
  }
  if (const Symbol *Sym = Img.findSymbolAt(Target))
    if (isNoReturnFunction(Sym->Name))
      return true;
  // A routine the image exports, which its own code calls.
  if (const Export *Exp = Img.findExportAt(Target))
    if (isNoReturnFunction(Exp->Name))
      return true;
  return false;
}

NoReturnTargetIndex::NoReturnTargetIndex(const BinaryImage &Img,
                                         NameResolver ResolveName,
                                         DeclaredNoReturn Declared)
    : Image(&Img), SymbolsIndexed(Img.LoadOnlyFunctionEntries.empty()),
      ResolveName(std::move(ResolveName)), Declared(std::move(Declared)) {
  const auto NoReturn = [&](llvm::StringRef Name) {
    return isNoReturnFunction(Name) ||
           (this->Declared && !Name.empty() && this->Declared(Name));
  };
  const auto NoReturnImport = [&](const Import &Imp) {
    return NoReturn(Imp.Name) || isNoReturnModuleExport(Imp);
  };
  // Preserve first-IAT precedence even when its name is empty or returning.
  // Only an address absent from the IAT map can use an exact stub spelling.
  for (const Import &Imp : Img.Imports)
    if (ImportAddresses.insert(Imp.IATAddr).second && NoReturnImport(Imp))
      Targets.insert(Imp.IATAddr);
  for (const auto &[Address, Index] : Img.ImportStubIndices)
    if (!ImportAddresses.count(Address) && Index < Img.Imports.size() &&
        NoReturnImport(Img.Imports[Index]) && Img.findImportStubAt(Address))
      Targets.insert(Address);
  // Pointer slots the loader binds to imports, which x86 calls through as
  // `call [slot]`; an import directory entry for the same slot decides.
  const ImportStorageSlotCollection Storage = Img.collectImportStorageSlots();
  for (const auto &[SlotVA, Slot] : Storage.Slots)
    if (!ImportAddresses.count(SlotVA) && !Storage.Conflicts.count(SlotVA) &&
        Slot.Addend == 0 && NoReturn(Slot.Name))
      Targets.insert(SlotVA);

  // A returning import still permits the first same-address symbol to prove
  // no-return. Later symbol aliases do not override that first symbol.
  // `--func` already has the IAT map; walking every image symbol is the
  // whole-PE cost that single-function export is trying to skip.  The first
  // export at an address proves it too: the image's own code calls the
  // routines it exports, such as the kernel's ExRaiseStatus.
  if (SymbolsIndexed) {
    std::set<va_t> SymbolAddresses;
    for (const Symbol &Sym : Img.Symbols)
      if (!Sym.IsSection && SymbolAddresses.insert(Sym.Addr).second &&
          NoReturn(Sym.Name))
        Targets.insert(Sym.Addr);
    std::set<va_t> ExportAddresses;
    for (const Export &Exp : Img.Exports)
      if (ExportAddresses.insert(Exp.Addr).second && NoReturn(Exp.Name))
        Targets.insert(Exp.Addr);
  }
  Targets.erase(InvalidVA);
}

void NoReturnTargetIndex::clearMemoizedNames() {
  std::lock_guard<std::mutex> Lock(LazyMutex);
  LazyAnswers.clear();
}

bool NoReturnTargetIndex::contains(const BinaryImage &Img, va_t Target) const {
  if (Image != &Img)
    return isNoReturnTarget(Img, Target);
  if (Target == InvalidVA)
    return false;
  if (const auto Stated = Img.userNoReturnAt(Target))
    return *Stated;
  if (Img.functionHasBytePatch(Target))
    return false;
  if (Targets.count(Target))
    return true;
  // A restricted x86 load may not have registered this decoded veneer, and
  // Thumb callers can use its mode-tagged spelling. Ask the same veneer owner
  // as the live lookup, retaining exact IAT precedence and memoizing the work.
  const bool ResolveImport =
      !ImportAddresses.count(Target) &&
      (Img.decodesImportThunks() ||
       normalizeCodeAddress(Target, Img.Arch, Img.Mode) != Target);
  if (SymbolsIndexed && !ResolveImport)
    return false;
  // The first symbol at Target decides, as in the whole-image walk; a name
  // the restricted load has not ingested comes from the resolver.
  std::lock_guard<std::mutex> Lock(LazyMutex);
  auto [It, Inserted] = LazyAnswers.try_emplace(Target, false);
  if (!Inserted)
    return It->second;
  const auto NoReturn = [&](llvm::StringRef Name) {
    return isNoReturnFunction(Name) ||
           (Declared && !Name.empty() && Declared(Name));
  };
  if (ResolveImport)
    if (const Import *Imp = Img.findImportStubAt(Target))
      It->second = NoReturn(Imp->Name) || isNoReturnModuleExport(*Imp);
  if (SymbolsIndexed || It->second)
    return It->second;
  if (const Symbol *Sym = Img.findSymbolAt(Target))
    It->second = NoReturn(Sym->Name);
  else if (ResolveName)
    if (std::optional<std::string> Name = ResolveName(Target))
      It->second = NoReturn(*Name);
  if (!It->second)
    if (const Export *Exp = Img.findExportAt(Target))
      It->second = NoReturn(Exp->Name);
  return It->second;
}

bool isNoReturnLoadedTarget(const BinaryImage &Image, va_t Slot,
                            const NoReturnTargetIndex *Index) {
  if (const auto Stated = Image.userNoReturnAt(Slot))
    return *Stated;
  if (isNoReturnTarget(Image, Slot, Index))
    return true;
  if (const auto Target = readImmutableImageCodePointer(Image, Slot))
    return isNoReturnTarget(Image, *Target, Index);
  return false;
}

bool isNoReturnTarget(const BinaryImage &Img, va_t Target,
                      const NoReturnTargetIndex *Index) {
  return Index ? Index->contains(Img, Target) : isNoReturnTarget(Img, Target);
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

const LibCPrintfFormat *libcPrintfFormat(std::string_view Name) {
  static constexpr LibCPrintfFormat Formats[] = {
#define LIBC_PRINTF_FORMAT(Routine, ReturnType, ...)                           \
  {Routine,                                                                    \
   ReturnType,                                                                 \
   {__VA_ARGS__},                                                              \
   static_cast<uint8_t>(                                                       \
       std::initializer_list<std::string_view>{__VA_ARGS__}.size())},
#include "neverd/libc/LibCPrintfFormats.inc"
#undef LIBC_PRINTF_FORMAT
  };
  for (const LibCPrintfFormat &Format : Formats)
    if (Format.Name == Name)
      return &Format;
  return nullptr;
}

bool variadicIn(const LibCVariadicForward &Forward, LibCVariant Variant) {
  switch (Forward.Format) {
  case BinaryFormat::Unknown:
    return true;
  case BinaryFormat::ELF:
    return Variant == LibCVariant::GNU || Variant == LibCVariant::Musl ||
           Variant == LibCVariant::Bionic;
  case BinaryFormat::MachO:
    return Variant == LibCVariant::Darwin;
  case BinaryFormat::COFF:
    return Variant == LibCVariant::Windows;
  default:
    return false;
  }
}

std::string_view forwardFixedType(const LibCVariadicForward &Forward,
                                  size_t Index, LibCVariant Variant) {
  if (const auto Types = variant_rows::forwardFixed(Forward.Name, Variant);
      Types && Index < Types->size())
    return (*Types)[Index];
  return Forward.Fixed[Index];
}

const LibCVariadicForward *libcVariadicForward(std::string_view Name) {
  using Kind = LibCVariadicForward::Kind;
#define NEVERD_FIXED_PARAMETERS(...)                                           \
  {__VA_ARGS__},                                                               \
      static_cast<uint8_t>(                                                    \
          std::initializer_list<std::string_view>{__VA_ARGS__}.size())
  static constexpr LibCVariadicForward Forwards[] = {
#define LIBC_VA_LIST_FORM(Routine, ReturnType, TypeHeader, Form, ...)          \
  {Kind::VaList,                                                               \
   Routine,                                                                    \
   ReturnType,                                                                 \
   TypeHeader,                                                                 \
   Form,                                                                       \
   NEVERD_FIXED_PARAMETERS(__VA_ARGS__),                                       \
   0,                                                                          \
   {},                                                                         \
   false},
#define LIBC_BOUNDED_VARARGS(Routine, ReturnType, TypeHeader, Read, ReadType,  \
                             ...)                                              \
  {Kind::Bounded, Routine,  ReturnType,                                        \
   TypeHeader,    Routine,  NEVERD_FIXED_PARAMETERS(__VA_ARGS__),              \
   Read,          ReadType, false},
#define LIBC_ELF_BOUNDED_VARARGS(Routine, ReturnType, TypeHeader, Read,        \
                                 ReadType, ...)                                \
  {Kind::Bounded, Routine,                                                     \
   ReturnType,    TypeHeader,                                                  \
   Routine,       NEVERD_FIXED_PARAMETERS(__VA_ARGS__),                        \
   Read,          ReadType,                                                    \
   false,         BinaryFormat::ELF},
#define LIBC_SENTINEL_VARARGS(Routine, ReturnType, TypeHeader, Vector,         \
                              WithEnvironment, ...)                            \
  {Kind::Sentinel,                                                             \
   Routine,                                                                    \
   ReturnType,                                                                 \
   TypeHeader,                                                                 \
   Vector,                                                                     \
   NEVERD_FIXED_PARAMETERS(__VA_ARGS__),                                       \
   0,                                                                          \
   {},                                                                         \
   WithEnvironment},
#include "neverd/libc/LibCVariadicForwards.inc"
  };
#undef NEVERD_FIXED_PARAMETERS
  for (const LibCVariadicForward &Forward : Forwards)
    if (Forward.Name == Name)
      return &Forward;
  return nullptr;
}

std::optional<unsigned> vaListParameter(std::string_view Name) {
  // A va_list form takes the variadic routine's fixed parameters, then the
  // va_list.
  static const std::unordered_map<std::string_view, unsigned> Positions = [] {
    std::unordered_map<std::string_view, unsigned> Map;
#define LIBC_VA_LIST_FORM(Routine, ReturnType, TypeHeader, Form, ...)          \
  Map.emplace(                                                                 \
      Form, static_cast<unsigned>(                                             \
                std::initializer_list<std::string_view>{__VA_ARGS__}.size()));
#include "neverd/libc/LibCVariadicForwards.inc"
    return Map;
  }();
  const auto It = Positions.find(Name);
  if (It == Positions.end())
    return std::nullopt;
  return It->second;
}

} // namespace neverd::libc

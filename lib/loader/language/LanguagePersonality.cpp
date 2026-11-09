//===- LanguagePersonality.cpp - Personality and routine name identity ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Classifies a personality routine from the name an image spells it with, and
/// names the routine an address or a dynamically bound slot stands for.  Kept
/// apart from the image-wide runtime detection in LanguageRuntime.cpp: that
/// pass asks what produced an image, this one asks what a single address is.
///
/// A stripped, statically linked image spells no name at all, so the two
/// routines at the end of this file open a way in for an identification made
/// elsewhere -- they say which addresses are worth identifying, and they take
/// a name back on terms strict enough that a wrong one cannot get in.
///
//===----------------------------------------------------------------------===//

#include "LanguageRuntimeDetail.h"

#include "neverd/loader/DWARF/ItaniumEH.h"
#include "neverd/loader/LanguageRuntime.h"
#include "neverd/loader/SymbolSpelling.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

#include <optional>

namespace neverd {

llvm::SmallVector<llvm::StringRef, 4>
symbolNameCandidates(llvm::StringRef Name) {
  llvm::SmallVector<llvm::StringRef, 4> Candidates;
  auto add = [&](llvm::StringRef Candidate) {
    if (Candidate.empty())
      return;
    for (llvm::StringRef Existing : Candidates)
      if (Existing == Candidate)
        return;
    Candidates.push_back(Candidate);
  };

  add(Name);
  llvm::StringRef Base = Name;
  // `DW.ref.<routine>` is not a routine: it is the word-sized slot a CIE
  // points at when its personality is encoded indirectly, which is what every
  // non-x86 ELF target does.  The slot is named after what it holds, so the
  // routine's own name is recoverable without following the pointer -- and it
  // has to be, because a shared object resolves that pointer at load time and
  // the file on disk holds a relocation rather than an address.
  if (Base.consume_front("DW.ref."))
    add(Base);
  if (Base.consume_front("__imp_"))
    add(Base);
  // An x86-32 PE decorates a stdcall symbol with `@<bytes>`.
  if (size_t At = Base.find('@'); At != llvm::StringRef::npos && At > 0) {
    Base = Base.take_front(At);
    add(Base);
  }
  if (Base.size() > 1 && Base[0] == '_')
    add(Base.drop_front());
  return Candidates;
}

namespace {

llvm::StringRef canonicalizeSymbolName(llvm::StringRef Name) {
  auto Candidates = symbolNameCandidates(Name);
  return Candidates.empty() ? Name : Candidates.back();
}

struct PersonalityName {
  const char *Name;
  ExceptionPersonality Personality;
};

constexpr PersonalityName kPersonalityNames[] = {
    // Windows table model.
    {"__C_specific_handler", ExceptionPersonality::CSpecificHandler},
    {"__CxxFrameHandler3", ExceptionPersonality::CxxFrameHandler3},
    {"__CxxFrameHandler4", ExceptionPersonality::CxxFrameHandler4},
    {"__GSHandlerCheck_SEH", ExceptionPersonality::GSHandlerCheckSEH},
    {"__GSHandlerCheck", ExceptionPersonality::GSHandlerCheck},
    {"__GSHandlerCheck_EH", ExceptionPersonality::GSHandlerCheckEH},
    {"__GSHandlerCheck_EH4", ExceptionPersonality::GSHandlerCheckEH4},
    // Windows x86-32 registration model.  `_except_handler3` canonicalizes to
    // `except_handler3` once the C underscore is removed.
    {"except_handler3", ExceptionPersonality::ExceptHandler3},
    {"except_handler4", ExceptionPersonality::ExceptHandler4},
    {"except_handler4_common", ExceptionPersonality::ExceptHandler4},
    {"__CxxFrameHandler", ExceptionPersonality::CxxFrameHandlerX86},
    // Itanium model.
    {"__gxx_personality_v0", ExceptionPersonality::GxxPersonalityV0},
    {"__gxx_personality_seh0", ExceptionPersonality::GxxPersonalitySEH0},
    {"__gxx_personality_sj0", ExceptionPersonality::GxxPersonalitySJ0},
    {"__gcc_personality_v0", ExceptionPersonality::GccPersonalityV0},
    {"__gcc_personality_seh0", ExceptionPersonality::GccPersonalitySEH0},
    {"__gcc_personality_sj0", ExceptionPersonality::GccPersonalitySJ0},
    // Objective-C.  Which of these an image installs is decided by the
    // runtime it was built against and, for GNUstep, by whether the
    // translation unit was Objective-C++ -- and that choice is the whole of
    // how the type table is read, so the spellings stay distinct rather than
    // collapsing onto one Objective-C enumerator.
    {"__objc_personality_v0", ExceptionPersonality::ObjCPersonalityV0},
    {"__gnu_objc_personality_v0", ExceptionPersonality::GnuObjCPersonalityV0},
    {"__gnu_objc_personality_seh0",
     ExceptionPersonality::GnuObjCPersonalitySEH0},
    {"__gnu_objc_personality_sj0", ExceptionPersonality::GnuObjCPersonalitySJ0},
    {"__gnustep_objc_personality_v0",
     ExceptionPersonality::GNUstepObjCPersonalityV0},
    {"__gnustep_objcxx_personality_v0",
     ExceptionPersonality::GNUstepObjCXXPersonalityV0},
    {"rust_eh_personality", ExceptionPersonality::RustEhPersonality},
    // Ada.  GNAT's Windows routine is two symbols for one frame's dispatch:
    // `_seh0` is what the image registers and `_imp` is the GCC-shaped routine
    // `_GCC_specific_handler` forwards to once it has translated the SEH
    // state, so both spellings name the same handling and reach one
    // enumerator.
    {"__gnat_personality_v0", ExceptionPersonality::GnatPersonalityV0},
    {"__gnat_personality_sj0", ExceptionPersonality::GnatPersonalitySJ0},
    {"__gnat_personality_seh0", ExceptionPersonality::GnatPersonalitySEH0},
    {"__gnat_personality_imp", ExceptionPersonality::GnatPersonalitySEH0},
    // D.  Three compilers, one set of tables, three names for the routine
    // that reads them.
    {"__dmd_personality_v0", ExceptionPersonality::DmdPersonalityV0},
    {"_d_eh_personality", ExceptionPersonality::DRuntimeEhPersonality},
    {"__gdc_personality_v0", ExceptionPersonality::GdcPersonalityV0},
    {"__gdc_personality_sj0", ExceptionPersonality::GdcPersonalitySJ0},
    {"__gdc_personality_seh0", ExceptionPersonality::GdcPersonalitySEH0},
    {"__gdc_personality_imp", ExceptionPersonality::GdcPersonalitySEH0},
    // ARM EHABI.  An index entry names one of these by number rather than by
    // address, but a linked image also carries the symbol, so both spellings
    // have to reach the same enumerator.  They say nothing about the source
    // language: every language that unwinds on this target uses them for the
    // frames that only need stepping over.
    {"__aeabi_unwind_cpp_pr0", ExceptionPersonality::AeabiUnwindCppPr0},
    {"__aeabi_unwind_cpp_pr1", ExceptionPersonality::AeabiUnwindCppPr1},
    {"__aeabi_unwind_cpp_pr2", ExceptionPersonality::AeabiUnwindCppPr2},
    // Go.  The linker emits this on windows/amd64 only, and only for
    // `runtime.asmcgocall_landingpad`; the `.abi0` suffix is how the Go
    // assembler spells a function that uses the pre-register ABI.
    {"runtime.sehtramp", ExceptionPersonality::GoSEHTrampoline},
    {"runtime.sehtramp.abi0", ExceptionPersonality::GoSEHTrampoline},
    // Delphi.
    {"__DelphiExceptionHandler", ExceptionPersonality::DelphiExceptionHandler},
    {"@HandleAnyException", ExceptionPersonality::DelphiX86Handler},
    {"@HandleFinally", ExceptionPersonality::DelphiX86Handler},
    {"@HandleOnException", ExceptionPersonality::DelphiX86Handler},
    {"@HandleAutoException", ExceptionPersonality::DelphiX86Handler},
};

/// Rust's personality is emitted as an ordinary Rust symbol, so the canonical
/// spelling above only matches an unmangled build.  A mangled one demangles to
/// a path ending in `rust_eh_personality`.
bool isMangledRustPersonality(llvm::StringRef Name) {
  if (!isRustMangledName(Name))
    return false;
  std::string Demangled = demangleRustName(Name);
  llvm::StringRef Ref(Demangled);
  return Ref.ends_with("rust_eh_personality") ||
         Ref.ends_with("rust_eh_personality_catch");
}

} // namespace

bool isRustMangledName(llvm::StringRef Name) {
  const SymbolScheme Scheme = symbolScheme(Name);
  return Scheme == SymbolScheme::RustLegacy || Scheme == SymbolScheme::RustV0;
}

std::string demangleRustName(llvm::StringRef Name) {
  return isRustMangledName(Name) ? readableSymbolName(Name) : std::string();
}

ExceptionPersonality classifyPersonalityName(llvm::StringRef Name) {
  if (Name.empty())
    return ExceptionPersonality::None;
  // What precedes a name is decoration, not identity: the same routine is
  // `__DelphiExceptionHandler` in a PE import table, `DelphiExceptionHandler`
  // once a caller has already undecorated it, and `@DelphiExceptionHandler` in
  // the spelling Delphi's own RTL and every tool that reads it use — `@` is
  // how Delphi marks a `System` unit symbol.  The table below is written the
  // way each platform spells its routine, so the comparison ignores the prefix
  // on both sides rather than requiring every caller to guess how much of it
  // to leave on.
  auto undecorated = [](llvm::StringRef Value) {
    while (Value.consume_front("_") || Value.consume_front("@"))
      ;
    return Value;
  };
  for (llvm::StringRef Candidate : symbolNameCandidates(Name)) {
    const llvm::StringRef Bare = undecorated(Candidate);
    for (const PersonalityName &Entry : kPersonalityNames)
      if (Bare == undecorated(Entry.Name))
        return Entry.Personality;
  }
  if (isMangledRustPersonality(Name))
    return ExceptionPersonality::RustEhPersonality;
  return ExceptionPersonality::Unknown;
}

SourceLanguageRuntime getPersonalityRuntime(ExceptionPersonality P) {
  switch (P) {
  case ExceptionPersonality::RustEhPersonality:
    return SourceLanguageRuntime::Rust;
  case ExceptionPersonality::GoRuntimeDispatch:
  case ExceptionPersonality::GoSEHTrampoline:
    return SourceLanguageRuntime::Go;
  case ExceptionPersonality::GxxPersonalityV0:
  case ExceptionPersonality::GxxPersonalitySEH0:
  case ExceptionPersonality::GxxPersonalitySJ0:
    return SourceLanguageRuntime::CxxItanium;
  case ExceptionPersonality::GccPersonalityV0:
  case ExceptionPersonality::GccPersonalitySEH0:
  case ExceptionPersonality::GccPersonalitySJ0:
    return SourceLanguageRuntime::C;
  case ExceptionPersonality::ObjCPersonalityV0:
  case ExceptionPersonality::GnuObjCPersonalityV0:
  case ExceptionPersonality::GnuObjCPersonalitySEH0:
  case ExceptionPersonality::GnuObjCPersonalitySJ0:
  case ExceptionPersonality::GNUstepObjCPersonalityV0:
  case ExceptionPersonality::GNUstepObjCXXPersonalityV0:
    return SourceLanguageRuntime::ObjectiveC;
  case ExceptionPersonality::CxxFrameHandler3:
  case ExceptionPersonality::CxxFrameHandler4:
  case ExceptionPersonality::CxxFrameHandlerX86:
  case ExceptionPersonality::GSHandlerCheckEH:
  case ExceptionPersonality::GSHandlerCheckEH4:
    return SourceLanguageRuntime::CxxMSVC;
  case ExceptionPersonality::DelphiX86Handler:
  case ExceptionPersonality::DelphiExceptionHandler:
    return SourceLanguageRuntime::Delphi;
  case ExceptionPersonality::GnatPersonalityV0:
  case ExceptionPersonality::GnatPersonalitySJ0:
  case ExceptionPersonality::GnatPersonalitySEH0:
    return SourceLanguageRuntime::Ada;
  case ExceptionPersonality::DmdPersonalityV0:
  case ExceptionPersonality::DRuntimeEhPersonality:
  case ExceptionPersonality::GdcPersonalityV0:
  case ExceptionPersonality::GdcPersonalitySJ0:
  case ExceptionPersonality::GdcPersonalitySEH0:
    return SourceLanguageRuntime::D;
  default:
    return SourceLanguageRuntime::Unknown;
  }
}

/// True for an ARM or AArch64 mapping symbol.
///
/// The ELF ABI for the ARM architecture defines `$a`, `$d`, `$t` and `$x` --
/// each optionally followed by `.` and a suffix -- to mark where the
/// instruction set changes or data begins.  The assembler emits one at the
/// start of practically every function, at the same address as the function's
/// own symbol, so a lookup by address that does not skip them answers `$x` for
/// half the image.  That is not a cosmetic wrong name: it is what makes an
/// aarch64 Rust object report an unknown personality for every frame it has.
bool isArmMappingSymbol(llvm::StringRef Name) {
  if (Name.size() < 2 || Name[0] != '$')
    return false;
  if (Name[1] != 'a' && Name[1] != 'd' && Name[1] != 't' && Name[1] != 'x')
    return false;
  return Name.size() == 2 || Name[2] == '.';
}

/// True when \p Name can stand for the routine at an address, as opposed to a
/// marker the toolchain put there for its own bookkeeping.
bool namesARoutine(llvm::StringRef Name) {
  return !Name.empty() && !Name.starts_with(kAutoFuncPrefix) &&
         !isArmMappingSymbol(Name);
}

namespace {

struct RoutineSlotBinding {
  std::optional<llvm::StringRef> Name;
  bool HasRelocation = false;

  bool hasAuthority() const { return Name.has_value() || HasRelocation; }
};

RoutineSlotBinding routineSlotBinding(const BinaryImage &Img, va_t SlotVA) {
  if (!SlotVA)
    return {};
  // Chained slots need not have an indirect-symbol-table entry. The loader's
  // exact binding owns their name; authentication remains a separate recipe
  // and never becomes ordinary import-storage evidence.
  if (auto It = Img.MachOAuthenticatedPointers.find(SlotVA);
      It != Img.MachOAuthenticatedPointers.end()) {
    if (!It->second.Symbol.empty() && !It->second.Addend)
      return {It->second.Symbol, true};
    return {std::nullopt, true};
  }
  if (auto It = Img.ImportStorageSlots.find(SlotVA);
      It != Img.ImportStorageSlots.end() && !It->second.Addend)
    return {It->second.Name, true};
  if (auto It = Img.ImportPtrSlots.find(SlotVA); It != Img.ImportPtrSlots.end())
    return {It->second, false};
  for (const Import &Imp : Img.Imports)
    if (Imp.IATAddr == SlotVA && !Imp.Name.empty())
      return {Imp.Name, false};
  for (const RelocationEntry &Rel : Img.Relocations)
    if (Rel.Address == SlotVA && !Rel.SymbolName.empty())
      return {Rel.SymbolName, true};
  // An unnamed relative relocation can still supply the dispatch target.
  // Keep its slot identity even when that target has no usable name.
  bool HasRelocation = false;
  for (const RelocationEntry &Rel : Img.Relocations) {
    if (Rel.Address != SlotVA)
      continue;
    HasRelocation = true;
    if (!Rel.SymbolName.empty() || Rel.Addend <= 0)
      continue;
    const va_t Target = static_cast<va_t>(Rel.Addend);
    for (va_t Candidate : {Target, Img.Base + Target})
      for (const Symbol &Sym : Img.Symbols)
        if (Sym.Addr == Candidate && namesARoutine(Sym.Name))
          return {Sym.Name, true};
  }
  return {std::nullopt, HasRelocation};
}

} // namespace

bool sameRoutineProvider(const BinaryImage &Img, va_t AddressA, va_t SlotA,
                         va_t AddressB, va_t SlotB) {
  if (routineSlotBinding(Img, SlotA).hasAuthority() ||
      routineSlotBinding(Img, SlotB).hasAuthority())
    return SlotA && SlotA == SlotB;
  return AddressA && AddressA == AddressB;
}

std::string resolveRoutineName(const BinaryImage &Img, va_t Address,
                               va_t SlotVA) {
  // A dynamically bound slot is authoritative when present: the value it holds
  // in the file image is a placeholder, but the binding names the routine.
  if (SlotVA != 0) {
    if (const auto Binding = routineSlotBinding(Img, SlotVA); Binding.Name)
      return Binding.Name->str();
  }

  if (Address == 0)
    return {};

  for (const Symbol &Sym : Img.Symbols)
    if (Sym.Addr == Address && namesARoutine(Sym.Name))
      return Sym.Name;
  for (const Import &Imp : Img.Imports)
    if (Imp.IATAddr == Address && !Imp.Name.empty())
      return Imp.Name;
  if (auto It = Img.ImportStubIndices.find(Address);
      It != Img.ImportStubIndices.end() && It->second < Img.Imports.size())
    return Img.Imports[It->second].Name;
  if (auto It = Img.ImportPtrSlots.find(Address);
      It != Img.ImportPtrSlots.end())
    return It->second;
  for (const Export &Exp : Img.Exports)
    if (Exp.Addr == Address && !Exp.Name.empty())
      return Exp.Name;
  for (const RelocationEntry &Rel : Img.Relocations)
    if (Rel.Address == Address && !Rel.SymbolName.empty())
      return Rel.SymbolName;

  // Last resort: the slot's own name.  Every non-x86 ELF target reaches its
  // personality through an indirection slot, and in a shared object that slot
  // holds a relative relocation -- one with no symbol to read, resolved by the
  // loader from an addend.  Nothing above can name the routine in that case.
  // The static symbol table can, because the compiler names the slot after
  // what it holds: `DW.ref.<routine>`, which `symbolNameCandidates` unwraps.
  if (SlotVA != 0)
    for (const Symbol &Sym : Img.Symbols)
      if (Sym.Addr == SlotVA && namesARoutine(Sym.Name))
        return Sym.Name;
  return {};
}

/// True when a frame's personality is still open to identification.
///
/// `None` and `Unknown` are both unclassified, and which of the two a decoder
/// left behind says only whether it got as far as reading a name.  Neither
/// says the routine has been identified, which is the question here.
static bool personalityIsUnresolved(ExceptionPersonality P) {
  return P == ExceptionPersonality::None || P == ExceptionPersonality::Unknown;
}

std::vector<va_t> collectUnnamedPersonalityRoutines(const BinaryImage &Img) {
  std::vector<va_t> Candidates;
  for (const ExceptionFunction &F : Img.ExceptionMetadata.Functions) {
    if (F.PersonalityVA == 0 || !personalityIsUnresolved(F.Personality))
      continue;
    // A name the image spells but the table does not know is still the
    // image's answer, and it is a better one than a guess: it says the
    // routine is something other than the personalities NeverD models.
    if (!resolveRoutineName(Img, F.PersonalityVA).empty())
      continue;
    if (!llvm::is_contained(Candidates, F.PersonalityVA))
      Candidates.push_back(F.PersonalityVA);
  }
  return Candidates;
}

bool adoptPersonalityRoutineName(BinaryImage &Img, va_t Address,
                                 llvm::StringRef Name) {
  if (Address == 0 || Name.empty())
    return false;
  const ExceptionPersonality Adopted = classifyPersonalityName(Name);
  if (personalityIsUnresolved(Adopted))
    return false;
  if (!resolveRoutineName(Img, Address).empty())
    return false;

  struct StagedFunction {
    size_t Index;
    ExceptionFunction Function;
  };
  std::vector<StagedFunction> Staged;

  for (size_t I = 0; I < Img.ExceptionMetadata.Functions.size(); ++I) {
    const ExceptionFunction &F = Img.ExceptionMetadata.Functions[I];
    if (F.PersonalityVA != Address || !personalityIsUnresolved(F.Personality))
      continue;

    ExceptionFunction Next = F;
    Next.PersonalityName = Name.str();
    Next.Personality = Adopted;

    const bool HasItaniumLanguageData =
        Next.HandlerDataVA != 0 || Next.Itanium.has_value();
    if (HasItaniumLanguageData) {
      if (!dwarf_eh::refreshItaniumLanguageData(Img, Next))
        return false;
    } else {
      ExceptionFunctionDecodeProvenance &Decode =
          Next.establishDecodeProvenance();
      // The only language contribution a frame without handler data can have
      // here is the provisional personality classification.  Replace it as a
      // unit; structural state remains independently owned.
      if (F.DecodeProvenance)
        Decode.Language = ExceptionDecodeState{};
      Next.Rust.reset();
      Next.ObjC.reset();
      Next.rebuildParseSummary();
    }

    // The name did not come from the file, so a reader comparing this record
    // against the bytes will not find it there.  Saying so is what keeps the
    // classification auditable rather than merely asserted.
    Next.DecodeProvenance->Language.Diagnostics.push_back(
        ("personality routine name inferred rather than read from the image: " +
         Name)
            .str());
    Next.rebuildParseSummary();
    Staged.push_back({I, std::move(Next)});
  }
  if (Staged.empty())
    return false;

  // From this point every operation is non-failing.  Capture any image-level
  // compatibility contributions made since the previous summary, then commit
  // every replacement and the inferred symbol as one transaction.
  Img.ExceptionMetadata.captureUnstructuredParseContributions();
  for (StagedFunction &Replacement : Staged)
    Img.ExceptionMetadata.Functions[Replacement.Index] =
        std::move(Replacement.Function);

  Symbol Sym;
  Sym.Name = Name.str();
  Sym.Origin = NameOrigin::Analysis;
  Sym.Addr = Address;
  Sym.IsFunc = true;
  Img.Symbols.push_back(std::move(Sym));
  Img.ExceptionMetadata.rebuildParseSummary();
  return true;
}

} // namespace neverd

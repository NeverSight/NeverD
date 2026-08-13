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
//===----------------------------------------------------------------------===//

#include "LanguageRuntimeDetail.h"

#include "neverd/loader/LanguageRuntime.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Demangle/Demangle.h"

#include <cstdlib>

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
  llvm::StringRef Bare = Name;
  Bare.consume_front("_");
  if (Bare.starts_with("R"))
    return true;
  // Legacy Rust mangling is Itanium with a 17-character final component that
  // is the letter 'h' followed by 16 lowercase hex digits.
  if (!Name.starts_with("_ZN") && !Name.starts_with("__ZN"))
    return false;
  llvm::StringRef Tail = Name;
  if (!Tail.consume_back("E"))
    return false;
  if (Tail.size() < 19)
    return false;
  llvm::StringRef Hash = Tail.take_back(17);
  if (!Hash.starts_with("h"))
    return false;
  if (!Tail.drop_back(17).ends_with("17"))
    return false;
  for (char C : Hash.drop_front())
    if (!llvm::isHexDigit(C) || (llvm::isAlpha(C) && !llvm::isLower(C)))
      return false;
  return true;
}

std::string demangleRustName(llvm::StringRef Name) {
  std::string Input = Name.str();
  // Darwin prefixes every C symbol with an underscore, which the demanglers
  // do not expect ahead of the scheme marker.
  if (llvm::StringRef(Input).starts_with("__Z") ||
      llvm::StringRef(Input).starts_with("__R"))
    Input.erase(0, 1);

  if (llvm::StringRef(Input).starts_with("_R")) {
    if (char *Demangled = llvm::rustDemangle(Input)) {
      std::string Result(Demangled);
      std::free(Demangled);
      return Result;
    }
    return {};
  }

  if (!isRustMangledName(Input))
    return {};
  char *Demangled = llvm::itaniumDemangle(Input);
  if (!Demangled)
    return {};
  std::string Result(Demangled);
  std::free(Demangled);
  // Legacy mangling appends the crate disambiguator as a final `::h<hex>`
  // path component.  It is an artifact of the mangling, not part of the name.
  llvm::StringRef Ref(Result);
  if (Ref.size() > 19) {
    llvm::StringRef Tail = Ref.take_back(19);
    if (Tail.starts_with("::h")) {
      bool AllHex = true;
      for (char C : Tail.drop_front(3))
        AllHex = AllHex && llvm::isHexDigit(C);
      if (AllHex)
        Result.resize(Result.size() - 19);
    }
  }
  return Result;
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

std::string resolveRoutineName(const BinaryImage &Img, va_t Address,
                               va_t SlotVA) {
  // A dynamically bound slot is authoritative when present: the value it holds
  // in the file image is a placeholder, but the binding names the routine.
  if (SlotVA != 0) {
    if (auto It = Img.ImportPtrSlots.find(SlotVA);
        It != Img.ImportPtrSlots.end())
      return It->second;
    for (const Import &Imp : Img.Imports)
      if (Imp.IATAddr == SlotVA && !Imp.Name.empty())
        return Imp.Name;
    for (const RelocationEntry &Rel : Img.Relocations)
      if (Rel.Address == SlotVA && !Rel.SymbolName.empty())
        return Rel.SymbolName;
    // A relative relocation names no symbol because it needs none: the routine
    // is defined in this image, so the loader only has to add the load bias to
    // an addend that already holds the address.  That addend is the one thing
    // in the file that says where the slot will point, and without reading it
    // a position-independent image resolves every personality to nothing --
    // the slot's own contents are zero until the loader writes them.
    for (const RelocationEntry &Rel : Img.Relocations) {
      if (Rel.Address != SlotVA || !Rel.SymbolName.empty() || Rel.Addend <= 0)
        continue;
      // The addend is a link-time address, which is the loaded one only when
      // the image was linked at the base it is being read at.  Both readings
      // are tried because only one of them can name a routine.
      const va_t Target = static_cast<va_t>(Rel.Addend);
      for (va_t Candidate : {Target, Img.Base + Target})
        for (const Symbol &Sym : Img.Symbols)
          if (Sym.Addr == Candidate && namesARoutine(Sym.Name))
            return Sym.Name;
    }
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

} // namespace neverd

//===- LanguageRuntime.cpp - Source language runtime detection ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Classifies which language runtime produced an image from its sections,
/// symbols, and embedded runtime banners.  The per-address side of the same
/// question -- which routine a personality pointer names, and which runtime
/// that personality belongs to -- lives in LanguagePersonality.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/loader/LanguageRuntime.h"

#include "LanguageRuntimeDetail.h"

#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "neverd-language-runtime"

namespace neverd {
namespace {

bool sectionExists(const BinaryImage &Img, llvm::StringRef Name) {
  return Img.getSectionByName(Name) != nullptr;
}

/// Search every readable segment for a byte pattern.  Used only for the small
/// number of fixed runtime magics that identify a language, each of which is
/// long enough that a false positive would be a deliberate plant rather than
/// an accident.
bool imageContains(const BinaryImage &Img, llvm::StringRef Needle,
                   va_t *FoundVA = nullptr) {
  if (Needle.empty())
    return false;
  for (const Segment &Seg : Img.Segments) {
    if (Seg.Data.size() < Needle.size())
      continue;
    llvm::StringRef Haystack(reinterpret_cast<const char *>(Seg.Data.data()),
                             Seg.Data.size());
    size_t Pos = Haystack.find(Needle);
    if (Pos == llvm::StringRef::npos)
      continue;
    if (FoundVA)
      *FoundVA = Seg.VA + Pos;
    return true;
  }
  return false;
}

bool hasSymbolPrefix(const BinaryImage &Img, llvm::StringRef Prefix,
                     std::string *Match = nullptr) {
  for (const Symbol &Sym : Img.Symbols) {
    if (!llvm::StringRef(Sym.Name).starts_with(Prefix))
      continue;
    if (Match)
      *Match = Sym.Name;
    return true;
  }
  for (const Import &Imp : Img.Imports) {
    if (!llvm::StringRef(Imp.Name).starts_with(Prefix))
      continue;
    if (Match)
      *Match = Imp.Name;
    return true;
  }
  return false;
}

bool hasExactSymbol(const BinaryImage &Img, llvm::StringRef Name) {
  auto matches = [&](llvm::StringRef Candidate) {
    for (llvm::StringRef Spelling : symbolNameCandidates(Candidate))
      if (Spelling == Name)
        return true;
    return false;
  };
  for (const Symbol &Sym : Img.Symbols)
    if (matches(Sym.Name))
      return true;
  for (const Import &Imp : Img.Imports)
    if (matches(Imp.Name))
      return true;
  return false;
}

} // namespace

const char *getSourceLanguageRuntimeName(SourceLanguageRuntime Runtime) {
  switch (Runtime) {
  case SourceLanguageRuntime::Unknown:
    return "unknown";
  case SourceLanguageRuntime::C:
    return "c";
  case SourceLanguageRuntime::CxxItanium:
    return "c++-itanium";
  case SourceLanguageRuntime::CxxMSVC:
    return "c++-msvc";
  case SourceLanguageRuntime::Rust:
    return "rust";
  case SourceLanguageRuntime::Go:
    return "go";
  case SourceLanguageRuntime::Delphi:
    return "delphi";
  case SourceLanguageRuntime::ObjectiveC:
    return "objective-c";
  case SourceLanguageRuntime::Swift:
    return "swift";
  case SourceLanguageRuntime::Ada:
    return "ada";
  case SourceLanguageRuntime::D:
    return "d";
  }
  return "unknown";
}

LanguageRuntimeInfo detectLanguageRuntime(const BinaryImage &Img) {
  LanguageRuntimeInfo Info;
  std::vector<SourceLanguageRuntime> Found;
  auto record = [&](SourceLanguageRuntime Runtime, std::string Evidence) {
    Info.Evidence.push_back(std::move(Evidence));
    for (SourceLanguageRuntime R : Found)
      if (R == Runtime)
        return;
    Found.push_back(Runtime);
  };

  // --- Go -----------------------------------------------------------------
  // The Go linker always emits the function table; its section name differs by
  // format and the symbol is present even when the section is merged away.
  if (sectionExists(Img, ".gopclntab") || sectionExists(Img, "__gopclntab") ||
      sectionExists(Img, ".gosymtab"))
    record(SourceLanguageRuntime::Go, "go function table section");
  else if (hasExactSymbol(Img, "runtime.pclntab") ||
           hasExactSymbol(Img, "runtime.firstmoduledata"))
    record(SourceLanguageRuntime::Go, "go runtime module symbol");
  else if (hasSymbolPrefix(Img, "runtime.gopanic") ||
           hasSymbolPrefix(Img, "runtime.deferreturn"))
    record(SourceLanguageRuntime::Go, "go panic/defer runtime symbol");

  // `\xff Go buildinf:` is the fixed 14-byte header of the build-info blob the
  // Go linker writes into every binary from 1.13 onward.
  {
    va_t BuildInfoVA = 0;
    if (imageContains(Img, llvm::StringRef("\xff Go buildinf:", 14),
                      &BuildInfoVA))
      record(SourceLanguageRuntime::Go, "go build-info blob");
    va_t VersionVA = 0;
    if (imageContains(Img, "go1.", &VersionVA)) {
      const uint8_t *Bytes = Img.readVA(VersionVA, 16);
      if (Bytes) {
        llvm::StringRef Candidate(reinterpret_cast<const char *>(Bytes), 16);
        size_t Length = 4;
        while (Length < Candidate.size() &&
               (llvm::isDigit(Candidate[Length]) || Candidate[Length] == '.'))
          ++Length;
        if (Length > 4 && Info.Version.empty())
          Info.Version = Candidate.take_front(Length).str();
      }
    }
  }

  // --- Rust ---------------------------------------------------------------
  {
    std::string Match;
    if (hasExactSymbol(Img, "rust_eh_personality") ||
        hasSymbolPrefix(Img, "_ZN4core", &Match) ||
        hasSymbolPrefix(Img, "_ZN3std", &Match) ||
        hasSymbolPrefix(Img, "__ZN4core", &Match) ||
        hasSymbolPrefix(Img, "_RN", &Match))
      record(SourceLanguageRuntime::Rust, "rust core/std symbol");
    else if (imageContains(Img, "/rustc/") ||
             imageContains(Img, "library/std/src/panicking.rs"))
      record(SourceLanguageRuntime::Rust, "rust standard library path");
  }

  // --- Delphi -------------------------------------------------------------
  if (hasSymbolPrefix(Img, "@System@") || hasSymbolPrefix(Img, "@Sysutils@") ||
      hasExactSymbol(Img, "@HandleAnyException") ||
      hasExactSymbol(Img, "__DelphiExceptionHandler"))
    record(SourceLanguageRuntime::Delphi, "delphi runtime symbol");
  else if (imageContains(Img, "Portions Copyright (c) 1983,99 Borland") ||
           imageContains(Img, "Embarcadero Delphi") ||
           imageContains(Img, "SOFTWARE\\Borland\\Delphi"))
    record(SourceLanguageRuntime::Delphi, "delphi runtime banner");

  // --- Ada -----------------------------------------------------------------
  // GNAT mangles an Ada name as `package__subprogram`, so the runtime's own
  // units are the most reliable evidence an image is Ada: a program can be
  // built without ever raising an exception, but not without `system__`.
  // `__gnat_rcheck_` is the family of routines a compiler-inserted language
  // check calls, which is how an Ada image raises `Constraint_Error`.
  if (hasExactSymbol(Img, "__gnat_personality_v0") ||
      hasExactSymbol(Img, "__gnat_personality_sj0") ||
      hasExactSymbol(Img, "__gnat_personality_seh0") ||
      hasSymbolPrefix(Img, "__gnat_rcheck_") ||
      hasSymbolPrefix(Img, "ada__exceptions__") ||
      hasSymbolPrefix(Img, "system__standard_library"))
    record(SourceLanguageRuntime::Ada, "gnat runtime symbol");

  // --- D -------------------------------------------------------------------
  // A D symbol is `_D` followed by a length-prefixed path, which is too weak
  // a prefix to test on its own: it matches an ordinary C identifier that
  // starts with a digit-free `D`.  The runtime's own package roots are not
  // ambiguous, and neither are druntime's C-linkage entry points.
  if (hasExactSymbol(Img, "__dmd_personality_v0") ||
      hasExactSymbol(Img, "_d_eh_personality") ||
      hasExactSymbol(Img, "__gdc_personality_v0") ||
      hasExactSymbol(Img, "_d_throw_exception") ||
      hasExactSymbol(Img, "_d_throwdwarf") || hasSymbolPrefix(Img, "_D3std") ||
      hasSymbolPrefix(Img, "__D3std") || hasSymbolPrefix(Img, "_D4core") ||
      hasSymbolPrefix(Img, "__D4core") || hasSymbolPrefix(Img, "_D6object") ||
      hasSymbolPrefix(Img, "__D6object"))
    record(SourceLanguageRuntime::D, "d runtime symbol");

  // --- C++ ----------------------------------------------------------------
  if (hasExactSymbol(Img, "__gxx_personality_v0") ||
      hasExactSymbol(Img, "__gxx_personality_seh0") ||
      hasSymbolPrefix(Img, "_ZSt") || hasSymbolPrefix(Img, "_ZNSt") ||
      hasSymbolPrefix(Img, "__ZNSt"))
    record(SourceLanguageRuntime::CxxItanium, "itanium c++ runtime symbol");
  if (hasExactSymbol(Img, "__CxxFrameHandler3") ||
      hasExactSymbol(Img, "__CxxFrameHandler4") ||
      hasExactSymbol(Img, "__CxxFrameHandler") ||
      hasSymbolPrefix(Img, "??_7type_info"))
    record(SourceLanguageRuntime::CxxMSVC, "microsoft c++ runtime symbol");

  // --- Objective-C and Swift ----------------------------------------------
  // Apple's section names and Apple's personality are only half the language.
  // The GNU runtimes put their classes in `.objc_class_refs`/`__objc_data` and
  // never emit `__objc_personality_v0`, so an image built against libobjc,
  // GNUstep, or ObjFW is recognized by its own personalities and by
  // `objc_msg_lookup`, the message send that replaces `objc_msgSend` there.
  if (sectionExists(Img, "__objc_classlist") ||
      hasExactSymbol(Img, "__objc_personality_v0"))
    record(SourceLanguageRuntime::ObjectiveC, "objective-c runtime section");
  else if (hasExactSymbol(Img, "__gnu_objc_personality_v0") ||
           hasExactSymbol(Img, "__gnu_objc_personality_seh0") ||
           hasExactSymbol(Img, "__gnu_objc_personality_sj0") ||
           hasExactSymbol(Img, "__gnustep_objc_personality_v0") ||
           hasExactSymbol(Img, "__gnustep_objcxx_personality_v0"))
    record(SourceLanguageRuntime::ObjectiveC, "gnu objective-c personality");
  else if (sectionExists(Img, ".objc_class_refs") ||
           hasExactSymbol(Img, "objc_msg_lookup") ||
           hasExactSymbol(Img, "objc_msgSend"))
    record(SourceLanguageRuntime::ObjectiveC, "objective-c message send");
  if (hasSymbolPrefix(Img, "$s") || sectionExists(Img, "__swift5_types"))
    record(SourceLanguageRuntime::Swift, "swift metadata");

  if (Found.empty()) {
    if (hasExactSymbol(Img, "__gcc_personality_v0")) {
      Info.Runtime = SourceLanguageRuntime::C;
      Info.Evidence.emplace_back("c cleanup-only personality");
    }
    return Info;
  }

  Info.Runtime = Found.front();
  Info.IsMixed = Found.size() > 1;
  Info.SecondaryRuntimes.assign(Found.begin() + 1, Found.end());
  LLVM_DEBUG(llvm::dbgs() << "language-runtime: "
                          << getSourceLanguageRuntimeName(Info.Runtime)
                          << (Info.IsMixed ? " (mixed)" : "") << "\n");
  return Info;
}

} // namespace neverd

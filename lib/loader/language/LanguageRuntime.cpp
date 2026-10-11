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

#include "neverd/loader/SymbolSpelling.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>

#define DEBUG_TYPE "neverd-language-runtime"

namespace neverd {
namespace {

bool sectionExists(const BinaryImage &Img, llvm::StringRef Name) {
  return Img.getSectionByName(Name) != nullptr;
}

/// Where \p Needle, which is not empty and not longer than \p Haystack, first
/// occurs in \p Haystack, or llvm::StringRef::npos.  A data segment is mostly
/// zeros and strings, and memchr passes over it for the needle's first byte
/// several times faster than StringRef::find's skip table does.
size_t findBytes(llvm::StringRef Haystack, llvm::StringRef Needle) {
  const char *const Begin = Haystack.begin();
  const char *const LastStart = Haystack.end() - Needle.size();
  for (const char *Start = Begin; Start <= LastStart; ++Start) {
    Start = static_cast<const char *>(std::memchr(
        Start, Needle.front(), static_cast<size_t>(LastStart - Start) + 1));
    if (!Start)
      break;
    if (std::memcmp(Start + 1, Needle.data() + 1, Needle.size() - 1) == 0)
      return static_cast<size_t>(Start - Begin);
  }
  return llvm::StringRef::npos;
}

/// Search every readable segment for a byte pattern.  Used only for the small
/// number of fixed runtime magics that identify a language, each of which is
/// long enough that a false positive would be a deliberate plant rather than
/// an accident.
bool imageContains(const BinaryImage &Img, llvm::StringRef Needle,
                   va_t *FoundVA = nullptr) {
  if (Needle.empty())
    return false;
  // `--func` already has the requested VA.  Banner scans of a multi-megabyte
  // image are only a fallback for unclassified full loads.
  if (!Img.LoadOnlyFunctionEntries.empty())
    return false;
  for (const Segment &Seg : Img.Segments) {
    // Runtime banners live in data.  Scanning a 24MB `.text` for `go1.` is
    // what made language detection dominate `--func` load of a large PE.
    if (Seg.isExecutable())
      continue;
    if (Seg.Data.size() < Needle.size())
      continue;
    llvm::StringRef Haystack(reinterpret_cast<const char *>(Seg.Data.data()),
                             Seg.Data.size());
    size_t Pos = findBytes(Haystack, Needle);
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
  for (const Export &Exp : Img.Exports) {
    if (!llvm::StringRef(Exp.Name).starts_with(Prefix))
      continue;
    if (Match)
      *Match = Exp.Name;
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
  for (const Export &Exp : Img.Exports)
    if (matches(Exp.Name))
      return true;
  return false;
}

// The evidence detectLanguageRuntime reads; see LanguageRuntime.def.
enum class EvidenceOrder { Chain, Otherwise, Fallback };
enum class ProbeKind {
  Section,
  Symbol,
  SymbolPrefix,
  Bytes,
  Itanium,
  Microsoft
};

enum class EvidenceId {
#define NEVERD_LANGUAGE_EVIDENCE(Id, Runtime, Order, Description) Id,
#include "LanguageRuntime.def"
};

struct Evidence {
  EvidenceId Id;
  SourceLanguageRuntime Runtime;
  EvidenceOrder Order;
  llvm::StringLiteral Description;
};

constexpr Evidence Evidences[] = {
#define NEVERD_LANGUAGE_EVIDENCE(Id, Runtime, Order, Description)              \
  {EvidenceId::Id, SourceLanguageRuntime::Runtime, EvidenceOrder::Order,       \
   Description},
#include "LanguageRuntime.def"
};

struct Probe {
  EvidenceId Group;
  ProbeKind Kind;
  llvm::StringLiteral Value;
};

constexpr Probe Probes[] = {
#define NEVERD_LANGUAGE_PROBE(Id, Kind, Value)                                 \
  {EvidenceId::Id, ProbeKind::Kind, Value},
#include "LanguageRuntime.def"
};

#define NEVERD_LANGUAGE_STRING(Name, Value)                                    \
  constexpr llvm::StringLiteral Name(Value);
#define NEVERD_LANGUAGE_CHAR(Name, Value) constexpr char Name = Value;
#define NEVERD_LANGUAGE_VALUE(Name, Value) constexpr size_t Name = Value;
#include "LanguageRuntime.def"

bool probeHolds(const BinaryImage &Img, const Probe &P) {
  switch (P.Kind) {
  case ProbeKind::Section:
    return sectionExists(Img, P.Value);
  case ProbeKind::Symbol:
    return hasExactSymbol(Img, P.Value);
  case ProbeKind::SymbolPrefix:
    return hasSymbolPrefix(Img, P.Value);
  case ProbeKind::Bytes:
    return imageContains(Img, P.Value);
  case ProbeKind::Itanium:
  case ProbeKind::Microsoft: {
    const auto Scheme = P.Kind == ProbeKind::Itanium ? SymbolScheme::Itanium
                                                     : SymbolScheme::Microsoft;
    const auto Matches = [Scheme](const auto &Symbol) {
      return symbolScheme(Symbol.Name) == Scheme &&
             !cxxSourceName(Symbol.Name).empty();
    };
    return llvm::any_of(Img.Symbols, Matches) ||
           llvm::any_of(Img.Imports, Matches) ||
           llvm::any_of(Img.Exports, Matches);
  }
  }
  return false;
}

/// Whether any probe of \p Group holds, trying them in the order listed: the
/// byte searches, which scan every data segment, come last in their groups.
bool evidenceHolds(const BinaryImage &Img, EvidenceId Group) {
  return llvm::any_of(Probes, [&](const Probe &P) {
    return P.Group == Group && probeHolds(Img, P);
  });
}

/// The Go release \p Img names first, `go1.` and the digits and dots after
/// it, or an empty string.
std::string findGoVersion(const BinaryImage &Img) {
  va_t VersionVA = 0;
  if (!imageContains(Img, GoVersionPrefix, &VersionVA))
    return {};
  const uint8_t *Bytes = Img.readVA(VersionVA, GoVersionWindow);
  if (!Bytes)
    return {};
  llvm::StringRef Candidate(reinterpret_cast<const char *>(Bytes),
                            GoVersionWindow);
  size_t Length = GoVersionPrefix.size();
  while (Length < Candidate.size() && (llvm::isDigit(Candidate[Length]) ||
                                       Candidate[Length] == GoVersionSeparator))
    ++Length;
  if (Length == GoVersionPrefix.size())
    return {};
  return Candidate.take_front(Length).str();
}

} // namespace

const char *getSourceLanguageRuntimeName(SourceLanguageRuntime Runtime) {
  switch (Runtime) {
#define NEVERD_LANGUAGE_RUNTIME_NAME(Name, Spelling)                           \
  case SourceLanguageRuntime::Name:                                            \
    return Spelling;
#include "LanguageRuntime.def"
  }
  return getSourceLanguageRuntimeName(SourceLanguageRuntime::Unknown);
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

  // A group marked Otherwise is tried only while no group of its chain has
  // held; a Fallback group only while nothing has been found.
  bool ChainHeld = false;
  for (const Evidence &E : Evidences) {
    if (E.Order == EvidenceOrder::Fallback)
      continue;
    if (E.Order == EvidenceOrder::Chain)
      ChainHeld = false;
    else if (ChainHeld)
      continue;
    if (evidenceHolds(Img, E.Id)) {
      record(E.Runtime, E.Description.str());
      ChainHeld = true;
    }
  }
  for (const Evidence &E : Evidences)
    if (E.Order == EvidenceOrder::Fallback && Found.empty() &&
        evidenceHolds(Img, E.Id))
      record(E.Runtime, E.Description.str());
  if (Found.empty())
    return Info;
  // `go1.` is only a Go release in a Go image.
  if (llvm::is_contained(Found, SourceLanguageRuntime::Go))
    Info.Version = findGoVersion(Img);

  Info.Runtime = Found.front();
  Info.IsMixed = Found.size() > 1;
  Info.SecondaryRuntimes.assign(Found.begin() + 1, Found.end());
  LLVM_DEBUG(llvm::dbgs() << "language-runtime: "
                          << getSourceLanguageRuntimeName(Info.Runtime)
                          << (Info.IsMixed ? " (mixed)" : "") << "\n");
  return Info;
}

} // namespace neverd

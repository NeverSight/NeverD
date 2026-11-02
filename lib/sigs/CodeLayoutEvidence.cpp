//===- CodeLayoutEvidence.cpp - Nonsplittable source code layouts
//----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Emits complete non-COMDAT COFF x64 contributions and explicit unsupported
/// declarations. Symbol positions come from the source object, never a probe.
///
//===----------------------------------------------------------------------===//

#include "CodeLayoutContext.h"
#include "X86/X86DataReference.h"
#include "X86/X86ImportReference.h"

#include "neverd/sigs/PatternGenerator.h"
#include "neverd/sigs/PatternParser.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/JSON.h"

#include <cstdint>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;
using namespace llvm::object;

namespace neverd::sigs {
namespace {

using Members = std::map<uint32_t, std::vector<std::string>>;

Expected<json::Object>
declaration(const COFFObjectFile &Obj, int32_t Section,
            const Members &Functions,
            const std::map<uint32_t, COFFSymbolRef> &Symbols) {
  json::Array Publics;
  for (const auto &[Offset, Names] : Functions) {
    json::Array Aliases;
    for (const auto &Name : Names)
      Aliases.push_back(Name);
    Publics.push_back(json::Object{{"offset", int64_t(Offset)},
                                   {"names", std::move(Aliases)}});
  }
  json::Object Out{{"schema", 1},
                   {"member", Obj.getFileName()},
                   {"section", Section},
                   {"complete", false},
                   {"members", std::move(Publics)}};
  auto Incomplete = [&](StringRef Reason) -> Expected<json::Object> {
    Out["reason"] = Reason;
    return std::move(Out);
  };
  auto Source = Obj.getSection(Section);
  if (!Source)
    return Source.takeError();
  const coff_section &Sec = **Source;
  ArrayRef<uint8_t> Code;
  if (auto Err = Obj.getSectionContents(&Sec, Code))
    return std::move(Err);
  const uint32_t Required = COFF::IMAGE_SCN_CNT_CODE |
                            COFF::IMAGE_SCN_MEM_EXECUTE |
                            COFF::IMAGE_SCN_MEM_READ;
  if ((Sec.Characteristics & Required) != Required || Sec.VirtualAddress ||
      (Sec.Characteristics &
       (COFF::IMAGE_SCN_LNK_COMDAT | COFF::IMAGE_SCN_MEM_WRITE)) ||
      Code.empty() || Code.size() > SignatureLimits::MaxCodeLayoutBytes ||
      Functions.begin()->first || Functions.rbegin()->first >= Code.size())
    return Incomplete("unsupported, split or mutable source contribution");
  std::set<std::string> UniqueNames;
  for (const auto &[Offset, Names] : Functions)
    for (const auto &Name : Names)
      if (isSynthesizedRoutineName(Name) || !UniqueNames.insert(Name).second)
        return Incomplete("synthesized or multiply defined function name");
  if (UniqueNames.size() > SignatureLimits::MaxCodeLayoutPublicNames)
    return Incomplete("source contribution exceeds public-name budget");

  std::vector<bool> Wildcards(Code.size(), false);
  std::vector<FuncRef> References;
  std::map<std::string, std::pair<int32_t, uint32_t>> StorageCoordinates;
  for (const auto &Rel : Obj.getRelocations(&Sec)) {
    const uint32_t Offset = Rel.VirtualAddress;
    auto Member = Functions.upper_bound(Offset);
    if (Member == Functions.begin())
      return Incomplete("relocation is outside a function member");
    const uint32_t Begin = std::prev(Member)->first;
    const uint32_t End =
        Member == Functions.end() ? Code.size() : Member->first;
    if (Offset >= End || End - Offset < sizeof(uint32_t) ||
        readLE<uint32_t>(Code.data() + Offset))
      return Incomplete("unsupported or nonzero relocation field");
    const auto Symbol = Symbols.find(Rel.SymbolTableIndex);
    if (Symbol == Symbols.end())
      return Incomplete("relocation names an invalid or auxiliary symbol");
    const auto Target = Symbol->second;
    auto Name = Obj.getSymbolName(Target);
    if (!Name)
      return Name.takeError();
    if (Name->empty() || Target.getNumberOfAuxSymbols() ||
        (Target.getStorageClass() != COFF::IMAGE_SYM_CLASS_EXTERNAL &&
         Target.getStorageClass() != COFF::IMAGE_SYM_CLASS_STATIC) ||
        Target.getComplexType() || Target.getSectionNumber() < 0 ||
        (!Target.getSectionNumber() &&
         (Target.getValue() ||
          Target.getStorageClass() == COFF::IMAGE_SYM_CLASS_STATIC)))
      return Incomplete("unsupported shared-storage or import ownership");
    if (Target.getSectionNumber() > 0) {
      auto Storage = Obj.getSection(Target.getSectionNumber());
      if (!Storage)
        return Storage.takeError();
      if (((*Storage)->Characteristics & COFF::IMAGE_SCN_MEM_EXECUTE) ||
          !((*Storage)->Characteristics & COFF::IMAGE_SCN_MEM_READ) ||
          Target.getValue() >= (*Storage)->SizeOfRawData)
        return Incomplete("shared storage is not a defined data coordinate");
    }
    const auto Bytes = Code.slice(Begin, End - Begin);
    const uint32_t Field = Offset - Begin;
    const bool Import =
        Name->starts_with(COFFImportPointerPrefix) &&
        Target.getStorageClass() == COFF::IMAGE_SYM_CLASS_EXTERNAL &&
        !Target.getSectionNumber() && Rel.Type == COFF::IMAGE_REL_AMD64_REL32 &&
        isX86ImportReferenceField(Bytes, Field, true);
    const auto Data = x64DataReferenceField(Bytes, Field);
    if (!Import && (Name->starts_with(COFFImportPointerPrefix) || !Data ||
                    Rel.Type != COFF::IMAGE_REL_AMD64_REL32 + Data->Bias ||
                    isX86ImportReferenceField(Bytes, Field, true)))
      return Incomplete("unsupported source relocation instruction");
    if (!Import) {
      const auto Coordinate =
          std::pair(Target.getSectionNumber(), Target.getValue());
      auto [At, Fresh] = StorageCoordinates.emplace(Name->str(), Coordinate);
      if (!Fresh && At->second != Coordinate)
        return Incomplete(
            "one storage name has conflicting source coordinates");
    }
    for (uint32_t At = Offset; At < Offset + sizeof(uint32_t); ++At) {
      if (Wildcards[At])
        return Incomplete("source relocation fields overlap");
      Wildcards[At] = true;
    }
    References.emplace_back(Offset, Name->str(), !Import);
  }
  std::string Pattern;
  raw_string_ostream Text(Pattern);
  for (size_t At = 0; At < Code.size(); ++At)
    if (Wildcards[At])
      Text << "..";
    else
      Text << format_hex_no_prefix(Code[At], 2, true);
  Text << " 00 0000 " << format_hex_no_prefix(Code.size(), 4, true)
       << " !layout";
  for (const auto &[Offset, Names] : Functions)
    for (const auto &Name : Names)
      Text << " :" << format_hex_no_prefix(Offset, 4, true) << ' ' << Name;
  for (const auto &Ref : References)
    Text << (Ref.IsData ? " &" : " ^")
         << format_hex_no_prefix(Ref.Offset, 4, true) << ' ' << Ref.Name;
  // The same native validator owns source output, text admission and cache
  // reads. A missing operand or unexplained wildcard remains a refusal.
  auto Parsed = PatternParser::parseLine(Pattern);
  if (!Parsed) {
    Out["reason"] = toString(Parsed.takeError());
    return std::move(Out);
  }
  Out["complete"] = true;
  Out["pattern"] = Pattern;
  return std::move(Out);
}

} // namespace

Expected<CodeLayoutEvidenceStats>
generateCodeLayoutEvidence(const ObjectFile &Obj, raw_ostream &OS) {
  CodeLayoutEvidenceStats Stats;
  const auto *COFF = dyn_cast<COFFObjectFile>(&Obj);
  if (!COFF || COFF->getMachine() != COFF::IMAGE_FILE_MACHINE_AMD64)
    return Stats;
  std::map<uint32_t, COFFSymbolRef> Symbols;
  std::map<int32_t, Members> Sections;
  for (uint32_t I = 0, N = COFF->getNumberOfSymbols(); I < N; ++I) {
    auto Sym = COFF->getSymbol(I);
    if (!Sym)
      return Sym.takeError();
    if (Sym->getNumberOfAuxSymbols() >= N - I)
      return createStringError(inconvertibleErrorCode(),
                               "truncated COFF auxiliary symbol range");
    Symbols.emplace(I, *Sym);
    I += Sym->getNumberOfAuxSymbols();
    if (Sym->getSectionNumber() <= 0 ||
        Sym->getComplexType() != COFF::IMAGE_SYM_DTYPE_FUNCTION ||
        (Sym->getStorageClass() != COFF::IMAGE_SYM_CLASS_EXTERNAL &&
         Sym->getStorageClass() != COFF::IMAGE_SYM_CLASS_STATIC))
      continue;
    auto Name = COFF->getSymbolName(*Sym);
    if (!Name)
      return Name.takeError();
    if (!Name->empty())
      Sections[Sym->getSectionNumber()][Sym->getValue()].push_back(Name->str());
  }
  for (const auto &[Section, Functions] : Sections) {
    if (Functions.size() < 2)
      continue;
    auto Record = declaration(*COFF, Section, Functions, Symbols);
    if (!Record)
      return Record.takeError();
    if (*Record->getBoolean("complete"))
      ++Stats.Complete;
    else
      ++Stats.Incomplete;
    OS << json::Value(std::move(*Record)) << '\n';
  }
  return Stats;
}

} // namespace neverd::sigs

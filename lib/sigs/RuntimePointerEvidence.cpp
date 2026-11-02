//===- RuntimePointerEvidence.cpp - Source runtime pointer declarations ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Retains selected COFF definitions and incomplete counterclaims for offline
/// configuration -> storage -> explicitly typed function joins.
///
//===----------------------------------------------------------------------===//

#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/sigs/PatternGenerator.h"
#include "neverd/sigs/PatternParser.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/JSON.h"

#include <cstdint>
#include <map>
#include <string>
#include <utility>

using namespace llvm;
using namespace llvm::object;

namespace neverd::sigs {
namespace {

bool external(const COFFSymbolRef &Sym) {
  return Sym.getStorageClass() == COFF::IMAGE_SYM_CLASS_EXTERNAL &&
         Sym.getSectionNumber() >= 0 &&
         (Sym.getSectionNumber() || !Sym.getValue());
}

Expected<json::Object>
declaration(const COFFObjectFile &Obj, COFFSymbolRef Sym, StringRef Name,
            const std::map<uint32_t, COFFSymbolRef> &Symbols,
            const std::map<std::string, std::string, std::less<>> &Bodies) {
  const bool Config = Name == "_load_config_used";
  const bool Function = Sym.getComplexType() == COFF::IMAGE_SYM_DTYPE_FUNCTION;
  json::Object Out{{"schema", 1},
                   {"member", Obj.getFileName()},
                   {"name", Name},
                   {"complete", false},
                   {"kind", Config     ? "load_config"
                            : Function ? "function"
                                       : "pointer"}};
  auto Incomplete = [&](StringRef Reason) -> Expected<json::Object> {
    Out["reason"] = Reason;
    return std::move(Out);
  };
  if (!external(Sym) || Sym.getSectionNumber() <= 0)
    return Incomplete("unsupported weak, common or absolute definition");
  auto Section = Obj.getSection(Sym.getSectionNumber());
  if (!Section)
    return Section.takeError();
  const coff_section &Sec = **Section;
  ArrayRef<uint8_t> Bytes;
  if (auto Err = Obj.getSectionContents(&Sec, Bytes))
    return std::move(Err);
  if (Function && !Config) {
    const auto Body = Bodies.find(Name);
    if (!(Sec.Characteristics & COFF::IMAGE_SCN_MEM_EXECUTE) ||
        !(Sec.Characteristics & COFF::IMAGE_SCN_CNT_CODE) ||
        Sym.getValue() >= Bytes.size() || Body == Bodies.end() ||
        Body->second.empty())
      return Incomplete(
          "no complete explicitly typed executable function body");
    Out["pattern"] = Body->second;
    Out["complete"] = true;
    return std::move(Out);
  }
  const uint32_t Required =
      COFF::IMAGE_SCN_CNT_INITIALIZED_DATA | COFF::IMAGE_SCN_MEM_READ;
  if ((Sec.Characteristics & Required) != Required ||
      (Sec.Characteristics &
       (COFF::IMAGE_SCN_MEM_WRITE | COFF::IMAGE_SCN_MEM_EXECUTE)) ||
      Sec.VirtualAddress || Sym.getValue() || Sym.getComplexType() ||
      (Config ? Bytes.size() < sizeof(uint32_t) ||
                    readLE<uint32_t>(Bytes.data()) != Bytes.size()
              : Bytes.size() != sizeof(uint64_t)))
    return Incomplete("unsupported data ownership or extent");
  for (const auto &[Index, Other] : Symbols)
    if (Other.getSectionNumber() == Sym.getSectionNumber() &&
        Other.getStorageClass() == COFF::IMAGE_SYM_CLASS_EXTERNAL &&
        Other.getRawPtr() != Sym.getRawPtr())
      return Incomplete("another external symbol owns the data section");
  // This declaration covers callable pointer fields, not unrelated scalar
  // linker fixups in the same source configuration. Check overlap before
  // inspecting an operand; a relocation touching a callable field must pass
  // the full pointer contract below. Unknown-width fixups before a field
  // cannot be proved disjoint and remain counterclaims.
  unsigned UnrelatedRelocations = 0;
  std::map<uint32_t, std::string> Relocations;
  for (const auto &Rel : Obj.getRelocations(&Sec)) {
    const uint32_t Offset = Rel.VirtualAddress;
    if (Config) {
      const auto Width =
          coffRelocationWidth(COFF::IMAGE_FILE_MACHINE_AMD64, Rel.Type);
      bool Touches = false;
      for (const auto &Field :
           coff_loader::loadConfigCallableFields(Bitness::Bits64)) {
        if (Field.Offset >= Bytes.size())
          continue;
        if (Bytes.size() - Field.Offset < sizeof(uint64_t))
          return Incomplete("truncated callable configuration field");
        Touches |= Offset < Field.Offset + sizeof(uint64_t) &&
                   (!Width || uint64_t(Offset) + *Width > Field.Offset);
      }
      if (!Touches) {
        ++UnrelatedRelocations;
        continue;
      }
    }
    if (Rel.Type != COFF::IMAGE_REL_AMD64_ADDR64 || Offset > Bytes.size() ||
        Bytes.size() - Offset < sizeof(uint64_t) || Offset % sizeof(uint64_t) ||
        readLE<uint64_t>(Bytes.data() + Offset) || Relocations.contains(Offset))
      return Incomplete(
          "unsupported, overlapping or nonzero-addend pointer relocation");
    const auto Target = Symbols.find(Rel.SymbolTableIndex);
    if (Target == Symbols.end())
      return Incomplete("relocation names an invalid or auxiliary symbol");
    if (!external(Target->second) ||
        (Config && Target->second.getComplexType()))
      return Incomplete("unsupported relocation target binding");
    auto TargetName = Obj.getSymbolName(Target->second);
    if (!TargetName)
      return TargetName.takeError();
    if (TargetName->empty())
      return Incomplete("unnamed relocation target");
    Relocations.emplace(Offset, TargetName->str());
  }
  if (Config) {
    json::Array Fields;
    for (const auto &Field :
         coff_loader::loadConfigCallableFields(Bitness::Bits64)) {
      if (Field.Offset > Bytes.size() ||
          Bytes.size() - Field.Offset < sizeof(uint64_t))
        continue;
      const auto At = Relocations.find(Field.Offset);
      if (At == Relocations.end()) {
        if (readLE<uint64_t>(Bytes.data() + Field.Offset))
          return Incomplete("nonzero callable field has no relocation");
        continue;
      }
      Fields.push_back(json::Object{{"offset", int64_t(Field.Offset)},
                                    {"name", At->second}});
    }
    Out["bytes"] = int64_t(Bytes.size());
    Out["unrelated_relocations"] = int64_t(UnrelatedRelocations);
    Out["fields"] = std::move(Fields);
  } else {
    if (Relocations.size() != 1 || Relocations.begin()->first)
      return Incomplete("pointer storage requires one complete relocation");
    Out["target"] = Relocations.begin()->second;
  }
  Out["complete"] = true;
  return std::move(Out);
}

} // namespace

Expected<RuntimePointerEvidenceStats>
generateRuntimePointerEvidence(const ObjectFile &Object,
                               ArrayRef<std::string> Names, raw_ostream &OS) {
  RuntimePointerEvidenceStats Stats;
  const auto *Obj = dyn_cast<COFFObjectFile>(&Object);
  if (!Obj || Obj->getMachine() != COFF::IMAGE_FILE_MACHINE_AMD64)
    return Stats;
  std::map<uint32_t, COFFSymbolRef> Symbols;
  const uint32_t Count = Obj->getNumberOfSymbols();
  for (uint32_t I = 0; I < Count; ++I) {
    auto Sym = Obj->getSymbol(I);
    if (!Sym)
      return Sym.takeError();
    if (Sym->getNumberOfAuxSymbols() >= Count - I)
      return createStringError(inconvertibleErrorCode(),
                               "COFF auxiliary symbols exceed the table");
    Symbols.emplace(I, *Sym);
    I += Sym->getNumberOfAuxSymbols();
  }
  std::vector<std::pair<COFFSymbolRef, StringRef>> Selected;
  bool NeedBodies = false;
  for (const auto &[Index, Sym] : Symbols) {
    auto Name = Obj->getSymbolName(Sym);
    if (!Name)
      return Name.takeError();
    if (*Name != "_load_config_used" && !llvm::is_contained(Names, *Name))
      continue;
    // An ordinary undefined declaration claims no initializer or body.
    if (Sym.getStorageClass() == COFF::IMAGE_SYM_CLASS_EXTERNAL &&
        !Sym.getSectionNumber() && !Sym.getValue())
      continue;
    Selected.emplace_back(Sym, *Name);
    NeedBodies |= Sym.getComplexType() == COFF::IMAGE_SYM_DTYPE_FUNCTION;
  }
  std::map<std::string, std::string, std::less<>> Bodies;
  if (NeedBodies) {
    PatternGeneratorOptions Options;
    Options.RequireCaller = Options.EmitReferences =
        Options.EmitDataReferences = true;
    std::string Text;
    raw_string_ostream Stream(Text);
    generatePatterns(Object, Options, Stream);
    SmallVector<StringRef> Lines;
    StringRef(Text).split(Lines, '\n', -1, false);
    for (StringRef Line : Lines) {
      if (Line.starts_with(";") || Line == "---")
        continue;
      auto Mod = PatternParser::parseLine(Line);
      if (!Mod)
        return Mod.takeError();
      for (const auto &Name : Mod->PublicNames) {
        auto [At, Fresh] = Bodies.try_emplace(Name.Name, Line.str());
        if (!Fresh)
          At->second
              .clear(); // Distinct definitions cannot choose a first body.
      }
    }
  }
  for (const auto &[Sym, Name] : Selected) {
    auto Record = declaration(*Obj, Sym, Name, Symbols, Bodies);
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

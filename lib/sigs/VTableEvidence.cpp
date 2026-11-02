//===- VTableEvidence.cpp - Primary COFF table declarations ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Emits source-owned table layouts and incomplete counterclaims for offline
/// selection. Weak externals retain both alternatives, including search rules.
///
//===----------------------------------------------------------------------===//

#include "VTableContext.h"

#include "neverd/loader/COFF/RTTI.h"
#include "neverd/sigs/PatternGenerator.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/COFF.h"
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

/// An external definition/undefined symbol, not a common or absolute symbol.
bool isExternal(const COFFSymbolRef &Sym) {
  return Sym.getStorageClass() == COFF::IMAGE_SYM_CLASS_EXTERNAL &&
         Sym.getSectionNumber() >= 0 &&
         (Sym.getSectionNumber() || !Sym.getValue());
}

Expected<bool> isFunction(const COFFObjectFile &Obj, COFFSymbolRef Sym) {
  if (!isExternal(Sym) ||
      Sym.getComplexType() != COFF::IMAGE_SYM_DTYPE_FUNCTION)
    return false;
  if (!Sym.getSectionNumber())
    return true;
  auto Sec = Obj.getSection(Sym.getSectionNumber());
  if (!Sec)
    return Sec.takeError();
  return ((*Sec)->Characteristics & COFF::IMAGE_SCN_CNT_CODE) &&
         ((*Sec)->Characteristics & COFF::IMAGE_SCN_MEM_EXECUTE) &&
         Sym.getValue() < (*Sec)->SizeOfRawData;
}

Expected<json::Object>
declaration(const COFFObjectFile &Obj, COFFSymbolRef Sym, StringRef Name,
            StringRef Type, const std::map<uint32_t, COFFSymbolRef> &Symbols) {
  json::Object Out{{"schema", 1},
                   {"member", Obj.getFileName()},
                   {"name", Name},
                   {"complete", false}};
  auto Incomplete = [&](StringRef Reason) -> Expected<json::Object> {
    Out["reason"] = Reason;
    return std::move(Out);
  };
  auto Section = Obj.getSection(Sym.getSectionNumber());
  if (!Section)
    return Section.takeError();
  const coff_section &Sec = **Section;
  ArrayRef<uint8_t> Bytes;
  if (auto Err = Obj.getSectionContents(&Sec, Bytes))
    return std::move(Err);
  const uint32_t Required = COFF::IMAGE_SCN_CNT_INITIALIZED_DATA |
                            COFF::IMAGE_SCN_MEM_READ |
                            COFF::IMAGE_SCN_LNK_COMDAT;
  if ((Sec.Characteristics & Required) != Required ||
      (Sec.Characteristics &
       (COFF::IMAGE_SCN_MEM_WRITE | COFF::IMAGE_SCN_MEM_EXECUTE)) ||
      Sec.VirtualAddress || Sym.getValue() != VTablePointerBytes ||
      Sym.getComplexType() || Bytes.size() <= VTablePointerBytes ||
      Bytes.size() % VTablePointerBytes)
    return Incomplete("unsupported table ownership or extent");
  for (const auto &[Index, Other] : Symbols)
    if (Other.getSectionNumber() == Sym.getSectionNumber() &&
        Other.getStorageClass() == COFF::IMAGE_SYM_CLASS_EXTERNAL &&
        Other.getRawPtr() != Sym.getRawPtr())
      return Incomplete("another external symbol owns the table section");

  std::map<uint32_t, const coff_relocation *> Fields;
  for (const coff_relocation &Rel : Obj.getRelocations(&Sec)) {
    const uint32_t Offset = Rel.VirtualAddress;
    if (Rel.Type != COFF::IMAGE_REL_AMD64_ADDR64 || Offset >= Bytes.size() ||
        Offset % VTablePointerBytes || !Fields.emplace(Offset, &Rel).second ||
        readLE<uint64_t>(Bytes.data() + Offset))
      return Incomplete(
          "unsupported, overlapping or nonzero-addend relocation");
  }
  if (Fields.size() != Bytes.size() / VTablePointerBytes)
    return Incomplete("a table field has no relocation");

  json::Array Slots;
  for (const auto &[Offset, Rel] : Fields) {
    auto Found = Symbols.find(Rel->SymbolTableIndex);
    if (Found == Symbols.end())
      return Incomplete("relocation names an invalid or auxiliary symbol");
    COFFSymbolRef Target = Found->second;
    auto TargetName = Obj.getSymbolName(Target);
    if (!TargetName)
      return TargetName.takeError();
    if (TargetName->empty())
      return Incomplete("unnamed relocation target");
    if (!Offset) {
      if (!isExternal(Target) || Target.getComplexType() ||
          *TargetName != ("??_R4" + Type + "6B@").str())
        return Incomplete("locator does not name the complete table type");
      continue;
    }
    json::Object Slot{{"offset", int64_t(Offset - VTablePointerBytes)}};
    json::Array Names;
    Names.push_back(*TargetName);
    if (Target.getStorageClass() == COFF::IMAGE_SYM_CLASS_WEAK_EXTERNAL) {
      const auto *Weak = Target.getWeakExternal();
      if (!Weak || Target.getNumberOfAuxSymbols() != 1 ||
          Target.getSectionNumber() || Target.getValue() ||
          Target.getComplexType() != COFF::IMAGE_SYM_DTYPE_FUNCTION ||
          (Weak->Characteristics != COFF::IMAGE_WEAK_EXTERN_SEARCH_NOLIBRARY &&
           Weak->Characteristics != COFF::IMAGE_WEAK_EXTERN_SEARCH_LIBRARY &&
           Weak->Characteristics != COFF::IMAGE_WEAK_EXTERN_SEARCH_ALIAS))
        return Incomplete("unsupported weak binding");
      Found = Symbols.find(Weak->TagIndex);
      if (Found == Symbols.end())
        return Incomplete("weak fallback names an invalid or auxiliary symbol");
      auto Function = isFunction(Obj, Found->second);
      if (!Function)
        return Function.takeError();
      if (!*Function)
        return Incomplete("weak fallback is not an external function");
      auto FallbackName = Obj.getSymbolName(Found->second);
      if (!FallbackName)
        return FallbackName.takeError();
      if (FallbackName->empty() || *FallbackName == *TargetName)
        return Incomplete("empty or cyclic weak fallback");
      Slot["weak_search"] = int64_t(Weak->Characteristics);
      Names.push_back(*FallbackName);
    } else {
      auto Function = isFunction(Obj, Target);
      if (!Function)
        return Function.takeError();
      if (!*Function)
        return Incomplete("table target is not an external function");
    }
    Slot["names"] = std::move(Names);
    Slots.push_back(std::move(Slot));
  }
  Out["complete"] = true;
  Out["bytes"] = int64_t(Bytes.size() - VTablePointerBytes);
  Out["slots"] = std::move(Slots);
  return std::move(Out);
}

} // namespace

Expected<VTableEvidenceStats> generateVTableEvidence(const ObjectFile &Object,
                                                     raw_ostream &OS) {
  VTableEvidenceStats Stats;
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
  for (const auto &[Index, Sym] : Symbols) {
    if (Sym.getSectionNumber() <= 0 ||
        Sym.getStorageClass() != COFF::IMAGE_SYM_CLASS_EXTERNAL)
      continue;
    auto Name = Obj->getSymbolName(Sym);
    if (!Name)
      return Name.takeError();
    const auto Type = coff_loader::primaryVTableTypeName(*Name);
    if (!Type)
      continue;
    auto Record = declaration(*Obj, Sym, *Name, *Type, Symbols);
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

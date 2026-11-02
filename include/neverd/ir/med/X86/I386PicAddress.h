//===- I386PicAddress.h - CFG-authenticated i386 PC arithmetic --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_MED_X86_I386PICADDRESS_H
#define NEVERD_IR_MED_X86_I386PICADDRESS_H
#include "neverd/ir/Limits.h"
#include "neverd/ir/med/MedIR.h"

#include <functional>
#include <optional>
#include <set>
#include <tuple>

namespace neverd {
/// Folds only COPY/ADD/zero-extension expressions rooted at exactly one
/// authenticated call-next/POP occurrence. Equal numeric values elsewhere
/// carry no permission to use the original image's address space.
template <typename LookupDefinition>
std::optional<uint64_t> foldI386PicAddress(const MedFunc &Function,
                                           const MedVar &Address,
                                           LookupDefinition Lookup) {
  if (Function.I386GetPcModels.empty() || Address.isConst())
    return std::nullopt;
  auto key = [](const MedVar &Value) {
    return std::tuple(Value.Kind, Value.Id, Value.SSAVer, Value.Size,
                      Value.RegOff, Value.RenameTag, Value.TheArch);
  };
  using Key = decltype(key(Address));
  struct Folded {
    uint64_t Value = 0;
    unsigned GetPcRoots = 0;
  };
  std::set<Key> Active;
  auto atSize = [](uint64_t Value, uint16_t Size) {
    if (Size == 0 || Size >= 8)
      return Value;
    return Value & ((uint64_t(1) << (Size * 8)) - 1);
  };
  std::function<std::optional<Folded>(const MedVar &, unsigned)> Visit =
      [&](const MedVar &Value, unsigned Depth) -> std::optional<Folded> {
    if (Depth > ::neverd::limits::ir::
                    kI386PicAddressFoldI386PicAddressVisitDepthLimit ||
        Value.Size == 0)
      return std::nullopt;
    if (Value.isConst()) {
      if (isAddressProvenance(Value.Provenance))
        return std::nullopt;
      return Folded{atSize(Value.ConstVal, Value.Size), 0};
    }

    const Key Identity = key(Value);
    if (!Active.insert(Identity).second)
      return std::nullopt;
    auto Leave = [&](std::optional<Folded> Result) {
      Active.erase(Identity);
      return Result;
    };

    std::optional<uint32_t> AuthenticatedPC;
    for (const MedI386GetPcModel &Model : Function.I386GetPcModels) {
      if (key(Model.Value) != Identity)
        continue;
      if (AuthenticatedPC && *AuthenticatedPC != Model.PCValue)
        return Leave(std::nullopt);
      AuthenticatedPC = Model.PCValue;
    }
    if (AuthenticatedPC)
      return Leave(Folded{atSize(*AuthenticatedPC, Value.Size), 1});

    const MedOp *Def = Lookup(Value);
    if (!Def || Def->Output.Size != Value.Size)
      return Leave(std::nullopt);
    if (Def->Opcode == NdOp::COPY && Def->NumInputs == 1 &&
        Def->Inputs[0].Size == Def->Output.Size)
      return Leave(Visit(Def->Inputs[0], Depth + 1));
    if (Def->Opcode == NdOp::INT_ZEXT && Def->NumInputs == 1 &&
        Def->Inputs[0].Size == 4 && Def->Output.Size >= Def->Inputs[0].Size) {
      auto Input = Visit(Def->Inputs[0], Depth + 1);
      if (Input)
        Input->Value = atSize(Input->Value, Def->Output.Size);
      return Leave(std::move(Input));
    }
    if (Def->Opcode != NdOp::INT_ADD || Def->NumInputs != 2)
      return Leave(std::nullopt);
    auto Left = Visit(Def->Inputs[0], Depth + 1);
    auto Right = Visit(Def->Inputs[1], Depth + 1);
    if (!Left || !Right || Left->GetPcRoots + Right->GetPcRoots > 1)
      return Leave(std::nullopt);
    return Leave(Folded{atSize(Left->Value + Right->Value, Def->Output.Size),
                        Left->GetPcRoots + Right->GetPcRoots});
  };

  auto Result = Visit(Address, 0);
  if (!Result || Result->GetPcRoots != 1)
    return std::nullopt;
  return Result->Value;
}
} // namespace neverd
#endif

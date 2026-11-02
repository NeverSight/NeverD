//===- COFFRegistrationSEHScopeState.cpp - Protected logical state -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationSEHDynamicScope.h"

#include "neverd/Limits.h"
#include "neverd/backend/Limits.h"
#include "neverd/backend/llvm/X86RegistrationSEHStateStore.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/ir/med/X86/X86RegistrationSEHStateStore.h"
#include "neverd/ir/med/X86RegistrationSEH4PhysicalFrame.h"

#include <functional>
#include <tuple>

namespace neverd::coff_registration {

llvm::Expected<RegistrationSEHScopeState>
bindDynamicSEHState(const MedFunc &Source, const RegistrationFrame &Frame,
                    llvm::ArrayRef<const llvm::Function *> Functions,
                    const std::set<const llvm::StoreInst *> &RuntimeWrites) {
  const auto *States = getX86RegistrationSEHPhysicalStates(Source);
  if (!States)
    return rejectIR("dynamic scope has no current physical frame binding");
  RegistrationSEHScopeState Result;
  Result.Offset = int64_t(Frame.EntrySP) - 8;
  const auto CallbackStores = getX86RegistrationSEHStateStores(Source);
  if (!CallbackStores)
    return rejectIR("logical state has incomplete callback store receipts");
  if (CallbackStores->empty() &&
      !llvm::any_of(Source.RegistrationStates->Blocks, [](const auto &Block) {
        return !Block.CallbackOnly && Block.levels().size() > 1;
      }))
    return Result;
  Result.Enabled = true;
  for (const auto *Write : RuntimeWrites)
    Result.Writes.emplace(Write, 4);
  using Key = std::tuple<MedVar::VarKind, int, int>;
  auto KeyOf = [](const MedVar &V) { return Key{V.Kind, V.Id, V.SSAVer}; };
  using Identity = std::pair<va_t, int>;
  size_t Work = 0;
  std::set<va_t> Observed;
  for (const auto &Store :
       Source.ExceptionMetadata->Registration->TryLevelStores) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return rejectIR(
          "dynamic scope source observations exceed their work budget");
    Observed.insert(Store.StoreVA);
  }
  const auto SourceStores = getX86RegistrationSEHStateStores(Source, true);
  if (!SourceStores)
    return rejectIR("dynamic scope has incomplete source state writes");
  std::map<Identity, const X86RegistrationSEHStateStoreBinding *> StateWrites;
  for (const auto &Store : *SourceStores)
    StateWrites.emplace(Identity{Store.Source->Address, Store.Source->OpSeq},
                        &Store);
  std::map<Identity, std::optional<int32_t>> Frames;
  for (const auto &Value : States->FrameValues) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return rejectIR("dynamic scope frame values exceed their work budget");
    if (!Frames
             .emplace(Identity{Value.Address, Value.OpSeq},
                      Value.EstablishedFrameOffset)
             .second)
      return rejectIR("dynamic scope frame value identity is ambiguous");
  }
  std::map<Key, const MedOp *> Definitions;
  std::map<Key, const PhiNode *> Phis;
  std::map<Key, int32_t> Offsets;
  for (const auto &Block : Source.Blocks) {
    for (const auto &Phi : Block.Phis) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return rejectIR("dynamic scope phis exceed their work budget");
      const auto Key = KeyOf(Phi.Output);
      const auto [Definition, Added] = Definitions.emplace(Key, nullptr);
      if (Added)
        Phis.emplace(Key, &Phi);
      else {
        Definition->second = nullptr;
        Phis.erase(Key);
        Offsets.erase(Key);
      }
    }
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return rejectIR("dynamic scope definitions exceed their work budget");
      if (!Op.Output.Size || Op.Output.isConst())
        continue;
      const auto [It, Added] = Definitions.emplace(KeyOf(Op.Output), &Op);
      if (!Added) {
        It->second = nullptr;
        Offsets.erase(KeyOf(Op.Output));
        Phis.erase(KeyOf(Op.Output));
        continue;
      }
      const auto Found = Frames.find({Op.Addr, Op.OriginSeq});
      if (Found != Frames.end() && Found->second)
        Offsets.emplace(KeyOf(Op.Output), *Found->second);
    }
  }
  std::function<std::optional<int64_t>(const MedVar &, bool, unsigned)>
      Resolve = [&](const MedVar &Value, bool Address,
                    unsigned Depth) -> std::optional<int64_t> {
    if (++Work > limits::kMaxRegistrationEHStateWork ||
        Depth >
            ::neverd::limits::backend::
                kCOFFRegistrationSEHScopeStateBindDynamicSEHStateResolveDepthLimit)
      return std::nullopt;
    if (Value.isConst())
      return Address ? std::nullopt
                     : std::optional<int64_t>(int32_t(Value.ConstVal));
    const auto Found = Definitions.find(KeyOf(Value));
    if (Found == Definitions.end())
      return std::nullopt;
    if (!Found->second) {
      const auto Phi = Phis.find(KeyOf(Value));
      if (!Address || Phi == Phis.end() || Phi->second->Output.Size != 4 ||
          Phi->second->Args.empty())
        return std::nullopt;
      // An unreachable join has no dynamic frame receipt. Every source arm
      // must independently name the same established frame coordinate; an
      // integer with matching bits, a narrow alias or a cycle is insufficient.
      std::optional<int64_t> Offset;
      for (const auto &[Predecessor, Input] : Phi->second->Args) {
        if (Input.Size != 4)
          return std::nullopt;
        const auto Arm = Resolve(Input, true, Depth + 1);
        if (!Arm || (Offset && Offset != Arm))
          return std::nullopt;
        Offset = Arm;
      }
      if (const auto &Entry = Phi->second->ExceptionalEntry)
        if (Entry->Size != 4 || Resolve(*Entry, true, Depth + 1) != Offset)
          return std::nullopt;
      return Offset;
    }
    if (Address) {
      const auto Offset = Offsets.find(KeyOf(Value));
      if (Offset != Offsets.end())
        return Offset->second;
    }
    const auto &Op = *Found->second;
    // Handler entry definitions are runtime roots, not source instructions,
    // so they have no FrameValues identity. Use the shared coordinate proof
    // and convert its entry-ESP origin to the established EBP origin.
    if (Address)
      if (const auto Offset = registrationRootEntryStackOffset(Op))
        return *Offset + 4;
    if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
      return Resolve(Op.Inputs[0], Address, Depth + 1);
    // A semantically unreachable source block still exists in native IR.
    // Its address temporaries have no execution receipt, but may derive from
    // a proved frame value in a dominating block. Preserve that static
    // address proof without inventing a reaching state or callback transfer.
    if (Address && Op.Opcode == NdOp::INT_ZEXT && Op.NumInputs == 1 &&
        Op.Output.Size == 8 && Op.Inputs[0].Size == 4)
      return Resolve(Op.Inputs[0], true, Depth + 1);
    if (Address && (Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
        Op.NumInputs == 2 && Op.Output.Size == 4 && Op.Inputs[0].Size == 4 &&
        Op.Inputs[1].Size == 4) {
      unsigned Constant = 1;
      if (Op.Opcode == NdOp::INT_ADD && Op.Inputs[0].isConst())
        Constant = 0;
      if (!Op.Inputs[Constant].isConst())
        return std::nullopt;
      const auto Base = Resolve(Op.Inputs[1 - Constant], true, Depth + 1);
      if (!Base)
        return std::nullopt;
      const int64_t Delta = int32_t(Op.Inputs[Constant].ConstVal);
      const int64_t Offset =
          Op.Opcode == NdOp::INT_ADD ? *Base + Delta : *Base - Delta;
      if (Offset >= INT32_MIN && Offset <= INT32_MAX)
        return Offset;
    }
    return std::nullopt;
  };
  struct Write {
    int Block;
    uint32_t Value;
    uint16_t Width;
    bool Computed;
  };
  std::map<Identity, Write> Expected;
  for (const auto &Block : Source.Blocks)
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return rejectIR("dynamic scope source exceeds its work budget");
      if (Op.Opcode != NdOp::STORE || Op.NumInputs != 2 ||
          (Op.Inputs[1].Size != 1 && Op.Inputs[1].Size != 2 &&
           Op.Inputs[1].Size != 4) ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          Op.MemoryOrdering != NdMemoryOrdering::None ||
          Resolve(Op.Inputs[0], true, 0) != -4)
        continue;
      auto Value = Resolve(Op.Inputs[1], false, 0);
      const auto Bound = StateWrites.find({Op.Addr, Op.OriginSeq});
      const bool Computed = Bound != StateWrites.end() &&
                            Bound->second->Operation == &Op &&
                            !Observed.count(Op.Addr);
      if (!Value && Computed)
        Value = Bound->second->Source->Value;
      const uint16_t Width = Op.Inputs[1].Size;
      const uint32_t Mask =
          Width == 4 ? UINT32_MAX : (uint32_t(1) << (8 * Width)) - 1;
      if (!Value || Op.OriginSeq < 0 ||
          !Expected
               .emplace(
                   Identity{Op.Addr, Op.OriginSeq},
                   Write{Block.Id, uint32_t(*Value) & Mask, Width, Computed})
               .second)
        return rejectIR("dynamic scope has an unproved source state write");
    }
  std::set<Identity> Seen;
  for (const auto *Function : Functions)
    for (const auto &Block : *Function)
      for (const auto &I : Block) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return rejectIR("dynamic scope write replay exceeds its work budget");
        const auto *MD =
            I.getMetadata(windows_eh_md::RegistrationOperationAttachment);
        if (!MD)
          continue;
        const auto Address = metadataInteger(*MD, 1, 64);
        const auto Seq = metadataInteger(*MD, 2, 32);
        if (!Address || !Seq || *Seq > INT_MAX)
          return rejectIR("dynamic scope write identity is malformed");
        const Identity Key{*Address, int(*Seq)};
        const auto Found = Expected.find(Key);
        if (Found == Expected.end())
          continue;
        const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
        if (!Store || !Seen.insert(Key).second || MD->getNumOperands() != 5 ||
            metadataInteger(*MD, 0, 64) != Source.Entry ||
            metadataInteger(*MD, 3, 32) != uint32_t(Found->second.Block) ||
            metadataInteger(*MD, 4, 8) != 1 || Store->isAtomic() ||
            !Store->isVolatile() || Store->getAlign() != llvm::Align(1) ||
            (Found->second.Computed && Found->second.Width == 4
                 ? !isX86RegistrationSEHStateValue(*Store->getValueOperand(),
                                                   Found->second.Value)
                 : integer(Store->getValueOperand(), 8 * Found->second.Width) !=
                       Found->second.Value))
          return rejectIR("dynamic scope changed its source state write");
        Result.Writes.emplace(Store, Found->second.Width);
      }
  if (Seen.size() != Expected.size())
    return rejectIR("dynamic scope state writes are incomplete");
  return Result;
}

} // namespace neverd::coff_registration

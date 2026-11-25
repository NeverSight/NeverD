//===- ELF32Calls.cpp - PPC32 PLT and GOT-relative calls ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Resolve a PIC transfer with the caller's reaching r30. Retain the load
/// bias on PC-derived addresses and leave writable GOT entries as loads.
///
//===----------------------------------------------------------------------===//

#include "ELFCalls.h"

#include "neverd/ir/med/IndirectCallSlot.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/lift/IntegerRegisters.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/loader/ELF/PowerPCABI.h"

#include "llvm/ADT/SmallVector.h"

#include <optional>
#include <utility>

namespace neverd {
namespace {
using namespace ppc_elf;

struct PICValue {
  uint32_t Bits;
  bool HasLoadBias;
};

std::optional<PICValue> picValue(const MedVar &V, MedDefinitionIndex &Defs,
                                 unsigned &Work) {
  if (V.Size != PPC32WordSize || !Work)
    return std::nullopt;
  --Work;
  if (V.isConst()) {
    if (V.Provenance == ConstantAddressProvenance::Scalar ||
        isExactAddressProvenance(V.Provenance))
      return PICValue{static_cast<uint32_t>(V.ConstVal),
                      isExactAddressProvenance(V.Provenance)};
    return std::nullopt;
  }
  const auto *D = Defs.find(V);
  if (!D)
    return std::nullopt;
  if (D->Opcode == NdOp::COPY && D->NumInputs == 1) {
    // The link-to-next instruction materializes the relocated PC in LR.
    // Its address fragment is an arithmetic base within this input image.
    const auto &Source = D->Inputs[0];
    if (V.Kind == MedVar::Reg && V.RegOff == integerreg::ppc::LR &&
        Source.isConst() &&
        Source.Provenance == ConstantAddressProvenance::AddressFragment &&
        Source.ConstVal == uint32_t(D->Addr + InstructionAlignment))
      return PICValue{static_cast<uint32_t>(Source.ConstVal), true};
    return picValue(D->Inputs[0], Defs, Work);
  }
  if ((D->Opcode != NdOp::INT_ADD && D->Opcode != NdOp::INT_SUB) ||
      D->NumInputs != 2)
    return std::nullopt;
  auto L = picValue(D->Inputs[0], Defs, Work);
  if (!L)
    return std::nullopt;
  auto R = picValue(D->Inputs[1], Defs, Work);
  if (!R)
    return std::nullopt;
  if (D->Opcode == NdOp::INT_SUB) {
    if (!L->HasLoadBias && R->HasLoadBias)
      return std::nullopt;
    return PICValue{L->Bits - R->Bits, L->HasLoadBias != R->HasLoadBias};
  }
  if (L->HasLoadBias && R->HasLoadBias)
    return std::nullopt;
  return PICValue{L->Bits + R->Bits, L->HasLoadBias || R->HasLoadBias};
}
} // namespace

llvm::DenseMap<uint32_t, uint32_t>
preparePPC32PLTCalls(MedFunc &Function, const BinaryImage &Image, MedVar GOT) {
  llvm::DenseMap<uint32_t, uint32_t> Calls;
  for (auto &Block : Function.Blocks)
    for (auto &Op : Block.Ops) {
      if (Op.Opcode != NdOp::CALL || !Op.NumInputs || !Op.Inputs[0].isConst())
        continue;
      auto Stub = Image.PPC32PLTGOTDisplacements.find(Op.Inputs[0].ConstVal);
      if (Stub == Image.PPC32PLTGOTDisplacements.end())
        continue;
      Calls.try_emplace(Op.CallSiteId, Stub->second);
      Op.Opcode = NdOp::INDIR_CALL;
      Op.Inputs[0] = GOT;
    }
  return Calls;
}

void recoverPPC32PICAddresses(MedFunc &Function, const BinaryImage &Image,
                              const llvm::DenseMap<uint32_t, uint32_t> &Calls) {
  MedDefinitionIndex Defs(Function);
  // Resolve literal reads against their original reaching definitions before
  // folding any of them. Block order must not influence the admissible GOTs.
  if (!Image.PPC32PICLiterals.empty()) {
    llvm::SmallVector<std::pair<MedOp *, uint32_t>> Literals;
    for (auto &Block : Function.Blocks)
      for (auto &Op : Block.Ops) {
        if (Op.Opcode != NdOp::LOAD || Op.NumInputs != 1 ||
            Op.Output.Size != PPC32WordSize ||
            Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
            Op.MemoryOrdering != NdMemoryOrdering::None)
          continue;
        unsigned Work = limits::kCallTargetSlotDepth;
        auto EA = picValue(Op.Inputs[0], Defs, Work);
        if (EA && EA->HasLoadBias)
          if (auto Word = Image.PPC32PICLiterals.find(EA->Bits);
              Word != Image.PPC32PICLiterals.end())
            Literals.emplace_back(&Op, Word->second);
      }
    for (auto [Op, Word] : Literals) {
      Op->Opcode = NdOp::COPY;
      Op->Inputs[0] = MedVar::makeConst(Word, PPC32WordSize,
                                        ConstantAddressProvenance::Scalar);
    }
  }
  for (auto &Block : Function.Blocks)
    for (auto &Op : Block.Ops) {
      unsigned Work = limits::kCallTargetSlotDepth;
      if (Op.Opcode == NdOp::INDIR_CALL) {
        auto Call = Calls.find(Op.CallSiteId);
        if (Call == Calls.end())
          continue;
        auto GOT = picValue(Op.Inputs[0], Defs, Work);
        if (!GOT || (!GOT->HasLoadBias && !Image.LoadsAtLinkAddress) ||
            !Image.PPC32PLTSlots.contains(GOT->Bits + Call->second))
          throw LowToMedConversionError(
              "PPC32 PIC PLT call has no proven r30-relative jump slot");
        Op.Inputs[0] =
            MedVar::makeConst(GOT->Bits + Call->second, PPC32WordSize,
                              ConstantAddressProvenance::DataAddress);
      } else if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
                 Op.NumInputs &&
                 Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
        auto EA = picValue(Op.Inputs[0], Defs, Work);
        if (!EA || !EA->HasLoadBias)
          continue;
        if (Image.isDataAddress(EA->Bits)) {
          Op.Inputs[0] = MedVar::makeConst(
              EA->Bits, PPC32WordSize, ConstantAddressProvenance::DataAddress);
        } else if (Image.Sections.empty()) {
          // GNU BSS-PLT can share an RWX mapping with the caller's private
          // data. Without its sections, rebuilding a data pointer into that
          // mapping as a code address (or a link-time integer) loses identity.
          const auto *Seg = Image.getSegmentFor(EA->Bits);
          if (Seg && Seg->isExecutable() && Seg->isWritable())
            throw LowToMedConversionError("PPC32 PIC access needs data bounds "
                                          "in a mixed executable mapping");
        }
      }
    }
}
} // namespace neverd

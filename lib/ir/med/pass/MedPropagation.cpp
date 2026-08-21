//===- MedPropagation.cpp - Copy / constant propagation for MedIR ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Copy propagation and constant folding on MedIR SSA form.
/// Handles ADRP+ADD patterns on AArch64 (page_base + page_offset).
///
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/loader/BinaryImage.h"

#include <map>
#include <optional>
#include <tuple>

namespace neverd {

void LowToMedConverter::propagate(MedFunc &Func) {
  // Key by (Kind, Id, SSAVer): the MedVar Id space is shared across kinds, and
  // a stack Param's Id (its argument index, e.g. arg6) overlaps with the small
  // allocVarId() ids handed to registers/temps.  Ignoring Kind lets a Param
  // input alias an unrelated register def and get rewritten to it (e.g. arg6 ->
  // RSP), corrupting recovered stack arguments.
  using DefKey = std::tuple<int, int, int>;
  auto keyOf = [](const MedVar &V) -> DefKey {
    return {static_cast<int>(V.Kind), V.Id, V.SSAVer};
  };
  std::map<DefKey, const MedOp *> Defs;

  const unsigned PointerSize = getTargetRegInfo(TargetArch).PointerSize;
  auto HasExactLoaderProvenance = [&](uint64_t Value) {
    // Value-global inventories cannot distinguish an unrelated numeric/null
    // zero from a different relocation occurrence that legitimately targets
    // VA zero. Exact DataAddress/CodeAddress occurrences are handled by the
    // provenance branch in Describe below.
    return Value != 0 && Image &&
           (Image->RelocDataAddrs.count(Value) != 0 ||
            Image->WritableRelocDataAddrs.count(Value) != 0 ||
            Image->RodataAnchorSeg.count(Value) != 0 ||
            Image->CodeRefTargets.count(Value) != 0);
  };
  auto MayConstantRelocate = [&](uint64_t Value, uint16_t Size) {
    const bool IsPointerWidth =
        Size == 0 || PointerSize == 0 || Size >= PointerSize;
    if (!Image)
      return false;
    if (HasExactLoaderProvenance(Value))
      return true;
    if (!IsPointerWidth)
      return false;
    const bool Potential = Image->isPotentiallyRelocatableAddress(Value);
    const bool Eligible = Value > limits::kMinGlobalDataAddr ||
                          Image->hasObjectDataProvenance(Value) ||
                          Image->WritableRelocDataAddrs.count(Value) != 0 ||
                          Image->CodeRefTargets.count(Value) != 0 ||
                          (!Image->getSegmentFor(Value) && Potential);
    return Eligible && Potential;
  };

  for (auto &Blk : Func.Blocks) {
    for (auto &Op : Blk.Ops) {
      if (Op.Output.Id >= 0 && Op.Output.Size > 0)
        Defs[keyOf(Op.Output)] = &Op;
    }
  }

  // Copy propagation: if def is COPY from another var, replace uses
  bool Changed = true;
  int Iters = 0;
  while (Changed && Iters++ < 10) {
    Changed = false;
    for (auto &Blk : Func.Blocks) {
      for (auto &Op : Blk.Ops) {
        for (uint8_t I = 0; I < Op.NumInputs; ++I) {
          auto It = Defs.find(keyOf(Op.Inputs[I]));
          if (It == Defs.end())
            continue;

          const auto *Def = It->second;
          // A COPY may also be an explicit register-view conversion (for
          // example, the scalar 4-byte view of a 16-byte vector register).
          // Replacing such a use with a differently sized source erases that
          // conversion boundary and can make SUBBYTES or integer extensions
          // structurally invalid.
          if (Def->Opcode == NdOp::COPY && Def->NumInputs == 1 &&
              Def->Inputs[0].Size == Op.Inputs[I].Size) {
            if (Op.Inputs[I] != Def->Inputs[0]) {
              Op.Inputs[I] = Def->Inputs[0];
              Changed = true;
            }
          }
        }
      }
    }
  }

  // Constant folding: fold binary ops of two constants
  Changed = true;
  Iters = 0;
  while (Changed && Iters++ < 5) {
    Changed = false;
    Defs.clear();
    for (auto &Blk : Func.Blocks) {
      for (auto &Op : Blk.Ops) {
        if (Op.Output.Id >= 0 && Op.Output.Size > 0)
          Defs[keyOf(Op.Output)] = &Op;
      }
    }

    for (auto &Blk : Func.Blocks) {
      for (auto &Op : Blk.Ops) {
        if (Op.NumInputs < 2)
          continue;
        if (Op.Opcode != NdOp::INT_ADD && Op.Opcode != NdOp::INT_SUB &&
            Op.Opcode != NdOp::INT_AND && Op.Opcode != NdOp::INT_OR &&
            Op.Opcode != NdOp::INT_XOR && Op.Opcode != NdOp::INT_MULT)
          continue;

        struct ConstantInfo {
          uint64_t Value = 0;
          bool MayRelocate = false;
          bool HasAddressOrigin = false;
          bool HasAddressFragment = false;
          bool IsStableScalar = false;
          ConstantAddressProvenance Provenance =
              ConstantAddressProvenance::Unknown;
          uint64_t AddressOwnerVA = InvalidVA;
        };
        auto ResolveConst =
            [&](const MedVar &V) -> std::optional<ConstantInfo> {
          auto Describe = [&](const MedVar &C) {
            const bool FullWidth =
                (C.Size == 0 || PointerSize == 0 || C.Size >= PointerSize);
            const bool ExactOrigin =
                isExactAddressProvenance(C.Provenance) && FullWidth;
            const bool Fragment =
                C.Provenance == ConstantAddressProvenance::AddressFragment &&
                FullWidth;
            // A narrow address marker cannot be materialized, but its bit
            // pattern is still relocation-dependent.  Keep it out of ordinary
            // constant folding so a later widen cannot resurrect a frozen VA.
            bool MayRelocate = isAddressProvenance(C.Provenance);
            if (C.Provenance == ConstantAddressProvenance::Unknown)
              MayRelocate = HasExactLoaderProvenance(C.ConstVal) ||
                            MayConstantRelocate(C.ConstVal, C.Size);
            const bool StableScalar =
                C.Provenance == ConstantAddressProvenance::Scalar ||
                (C.Provenance == ConstantAddressProvenance::Unknown &&
                 !MayRelocate);
            return ConstantInfo{C.ConstVal,      MayRelocate,  ExactOrigin,
                                Fragment,        StableScalar, C.Provenance,
                                C.AddressOwnerVA};
          };
          if (V.isConst())
            return Describe(V);
          auto It = Defs.find(keyOf(V));
          if (It != Defs.end() && It->second->Opcode == NdOp::COPY &&
              It->second->NumInputs == 1 && It->second->Inputs[0].isConst() &&
              It->second->Output.Size == V.Size &&
              It->second->Inputs[0].Size == V.Size)
            return Describe(It->second->Inputs[0]);
          return std::nullopt;
        };

        auto A = ResolveConst(Op.Inputs[0]);
        auto B = ResolveConst(Op.Inputs[1]);
        if (!A || !B)
          continue;

        uint64_t Result = 0;
        switch (Op.Opcode) {
        case NdOp::INT_ADD:
          Result = A->Value + B->Value;
          break;
        case NdOp::INT_SUB:
          Result = A->Value - B->Value;
          break;
        case NdOp::INT_AND:
          Result = A->Value & B->Value;
          break;
        case NdOp::INT_OR:
          Result = A->Value | B->Value;
          break;
        case NdOp::INT_XOR:
          Result = A->Value ^ B->Value;
          break;
        case NdOp::INT_MULT:
          Result = A->Value * B->Value;
          break;
        default:
          continue;
        }

        uint16_t OutSz = Op.Output.Size;
        if (OutSz > 0 && OutSz < 8) {
          uint64_t Mask = (1ULL << (OutSz * 8)) - 1;
          Result &= Mask;
        }

        bool ResultHasAddressOrigin = false;
        if (A->MayRelocate || B->MayRelocate) {
          // Preserve expressions whose value depends on two independently
          // rebuilt symbols, or on pointer bit patterns. The one safe fold is
          // canonical address construction: address +/- a numeric displacement
          // that itself lands on another loader-recognized address. This keeps
          // AArch64 ADRP+ADD and equivalent PIC materialization compact while
          // preventing a later compare from observing a frozen link-time
          // distance.
          const bool OutputIsPointerWidth = Op.Output.Size == 0 ||
                                            PointerSize == 0 ||
                                            Op.Output.Size >= PointerSize;
          const bool CanonicalAddressAdjustment =
              ((Op.Opcode == NdOp::INT_ADD &&
                (A->HasAddressOrigin || A->HasAddressFragment) &&
                B->IsStableScalar) ||
               (Op.Opcode == NdOp::INT_ADD &&
                (B->HasAddressOrigin || B->HasAddressFragment) &&
                A->IsStableScalar) ||
               (Op.Opcode == NdOp::INT_SUB &&
                (A->HasAddressOrigin || A->HasAddressFragment) &&
                B->IsStableScalar)) &&
              OutputIsPointerWidth && Image &&
              Image->isPotentiallyRelocatableAddress(Result);
          if (!CanonicalAddressAdjustment)
            continue;
          ResultHasAddressOrigin = true;
        }

        Op.Opcode = NdOp::COPY;
        ConstantAddressProvenance ResultProvenance =
            ConstantAddressProvenance::Unknown;
        uint64_t ResultOwnerVA = InvalidVA;
        if (ResultHasAddressOrigin) {
          const ConstantInfo &Base =
              (A->HasAddressOrigin || A->HasAddressFragment) ? *A : *B;
          ResultOwnerVA = Base.AddressOwnerVA;
          if (Base.Provenance == ConstantAddressProvenance::DataAddress)
            ResultProvenance = ConstantAddressProvenance::DataAddress;
          else if (Base.Provenance == ConstantAddressProvenance::CodeAddress)
            ResultProvenance = ConstantAddressProvenance::CodeAddress;
          else
            ResultProvenance = ConstantAddressProvenance::Address;
        } else if (A->Provenance == ConstantAddressProvenance::Scalar &&
                   B->Provenance == ConstantAddressProvenance::Scalar)
          ResultProvenance = ConstantAddressProvenance::Scalar;
        Op.Inputs[0] = MedVar::makeConst(Result, Op.Output.Size,
                                         ResultProvenance, ResultOwnerVA);
        Op.NumInputs = 1;
        Changed = true;
      }
    }
  }
}

} // namespace neverd

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
#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/BlockRegisterConstant.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedConstantPropagation.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/DenseMap.h"

#include <optional>
#include <tuple>

namespace neverd {

void LowToMedConverter::propagate(MedFunc &Func) {
  propagateInvariantConstants(Func);
  // Key by (Kind, Id, SSAVer): the MedVar Id space is shared across kinds, and
  // a stack Param's Id (its argument index, e.g. arg6) overlaps with the small
  // allocVarId() ids handed to registers/temps.  Ignoring Kind lets a Param
  // input alias an unrelated register def and get rewritten to it (e.g. arg6 ->
  // RSP), corrupting recovered stack arguments.
  using DefKey = std::tuple<int, int, int>;
  auto keyOf = [](const MedVar &V) -> DefKey {
    return {static_cast<int>(V.Kind), V.Id, V.SSAVer};
  };
  // Output identities and operation storage stay unchanged below. Keep one
  // index for both propagation loops; its pointers observe each rewrite's
  // current opcode and inputs, including folds earlier in the same round.
  // Duplicate definitions keep the last operation in traversal order.
  llvm::DenseMap<DefKey, const MedOp *> Defs;

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
  while (Changed &&
         Iters++ < ::neverd::limits::ir::kMaxMedCopyPropagationRounds) {
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
              Def->RegistrationRoot == MedOp::RegistrationRootKind::None &&
              Def->Inputs[0].Size == Op.Inputs[I].Size) {
            MedVar Replacement = Def->Inputs[0];
            if (Replacement.isConst() &&
                !canPropagateConstantToOperand(Op.Opcode, I))
              continue;
            // COPY is value transport, so an untagged immediate remains
            // role-neutral at its definition.  Once SSA substitutes it into a
            // numeric operand, classify only this occurrence as scalar.  Exact
            // loader/decoder provenance is never overwritten, and other COPY,
            // SELECT, memory, or control uses of the same definition remain
            // pointer-capable.
            if (Replacement.isConst() &&
                Replacement.Provenance == ConstantAddressProvenance::Unknown &&
                isNumericConstantOperand(Op.Opcode, I))
              Replacement.Provenance = ConstantAddressProvenance::Scalar;
            if (Op.Inputs[I] != Replacement) {
              Op.Inputs[I] = Replacement;
              Changed = true;
            }
          }
        }
      }
    }
  }

  // Absorption only compares already named SSA values. It neither expands
  // their definitions nor evaluates their producers, so a large dispatcher
  // expression costs the same as a register operand. Keep loads, calls and
  // their effects as separate operations for the ordinary DCE policy.
  auto FoldWordAbsorption = [&](MedOp &Op) {
    const auto PlainWord = [](const MedOp &Value) {
      return (Value.Opcode == NdOp::INT_AND || Value.Opcode == NdOp::INT_OR) &&
             Value.NumInputs == 2 && Value.Output.Size &&
             Value.Output.Size <= sizeof(uint64_t) &&
             Value.Inputs[0].Size == Value.Output.Size &&
             Value.Inputs[1].Size == Value.Output.Size && !Value.Dead &&
             Value.RegistrationRoot == MedOp::RegistrationRootKind::None &&
             !Value.SourceCallHint && Value.IntrinsicOutputs.empty() &&
             Value.MemoryOrdering == NdMemoryOrdering::None &&
             Value.MemoryAddressSpace == NdMemoryAddressSpace::Default;
    };
    if (!PlainWord(Op))
      return false;
    for (unsigned I = 0; I < 2; ++I) {
      const auto &Retained = Op.Inputs[1 - I];
      // Two Unspecified occurrences do not denote one stable machine word.
      if (!Retained.isConst() &&
          (Retained.Id < 0 ||
           (Retained.Kind != MedVar::Reg && Retained.Kind != MedVar::Temp &&
            Retained.Kind != MedVar::Param)))
        continue;
      const auto It = Defs.find(keyOf(Op.Inputs[I]));
      if (It == Defs.end())
        continue;
      const auto &Nested = *It->second;
      if (!PlainWord(Nested) || Nested.Opcode == Op.Opcode ||
          Nested.Output.Size != Op.Output.Size ||
          (Nested.Inputs[0] != Retained && Nested.Inputs[1] != Retained))
        continue;
      // x | (x & y) and x & (x | y) retain x's width and address provenance.
      const auto Value = Retained;
      Op.Opcode = NdOp::COPY;
      Op.Inputs[0] = Value;
      Op.NumInputs = 1;
      return true;
    }
    return false;
  };

  // Constant folding: fold binary ops of two constants
  Changed = true;
  Iters = 0;
  while (Changed &&
         Iters++ < ::neverd::limits::ir::kMaxMedFoldPropagationRounds) {
    Changed = false;
    for (auto &Blk : Func.Blocks) {
      for (auto &Op : Blk.Ops) {
        if (Op.NumInputs < 2)
          continue;
        // Some lifters emit the boolean flag directly, without passing
        // through MedFlags' compound-condition rewrite. Both routes use the
        // same identity rule; producers and their effects remain separate.
        if (Op.NumInputs == 2 && Op.Output.Size == 1 && !Op.Dead &&
            Op.RegistrationRoot == MedOp::RegistrationRootKind::None &&
            !Op.SourceCallHint && Op.IntrinsicOutputs.empty() &&
            Op.MemoryOrdering == NdMemoryOrdering::None &&
            Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
          if (const auto Known = selfComparisonConstant(Op.Opcode, Op.Inputs[0],
                                                        Op.Inputs[1])) {
            Op.Opcode = NdOp::COPY;
            Op.Inputs[0] = MedVar::makeConst(*Known, 1);
            Op.NumInputs = 1;
            Changed = true;
            continue;
          }
        }
        if (FoldWordAbsorption(Op)) {
          Changed = true;
          continue;
        }
        switch (Op.Opcode) {
        case NdOp::INT_ADD:
        case NdOp::INT_SUB:
        case NdOp::INT_AND:
        case NdOp::INT_OR:
        case NdOp::INT_XOR:
        case NdOp::INT_MULT:
        case NdOp::INT_LEFT:
        case NdOp::INT_RIGHT:
        case NdOp::INT_ASHR:
        case NdOp::INT_EQUAL:
        case NdOp::INT_NOTEQUAL:
        case NdOp::INT_LESS:
        case NdOp::INT_SLESS:
        case NdOp::INT_LESSEQUAL:
        case NdOp::INT_SLESSEQUAL:
        case NdOp::SELECT:
          break;
        default:
          continue;
        }

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
        if (Op.Opcode == NdOp::SELECT) {
          // A scalar constant chooses one SSA value. Preserve the chosen
          // value's identity and relocation provenance, without evaluating
          // or retaining dependencies on the unselected register.
          if (Op.NumInputs != 3 || !A || !A->IsStableScalar ||
              Op.RegistrationRoot != MedOp::RegistrationRootKind::None ||
              Op.SourceCallHint || !Op.IntrinsicOutputs.empty() ||
              Op.MemoryOrdering != NdMemoryOrdering::None ||
              Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
              !Op.Inputs[0].Size || Op.Inputs[0].Size > 8)
            continue;
          const unsigned Bits = Op.Inputs[0].Size * 8;
          const uint64_t Condition =
              Bits == 64 ? A->Value : A->Value & ((uint64_t{1} << Bits) - 1);
          const auto Selected = Op.Inputs[Condition ? 1 : 2];
          if (!Op.Output.Size || Selected.Size != Op.Output.Size)
            continue;
          Op.Opcode = NdOp::COPY;
          Op.Inputs[0] = Selected;
          Op.NumInputs = 1;
          Changed = true;
          continue;
        }
        auto B = ResolveConst(Op.Inputs[1]);
        // An absorbing scalar mask observes none of the other operand's bits.
        // Keep that operand's producer and memory effects for ordinary DCE;
        // only this pure value operation becomes a constant. An address
        // whose original VA is zero is not a scalar zero after relocation.
        if ((Op.Opcode == NdOp::INT_AND || Op.Opcode == NdOp::INT_OR) &&
            Op.NumInputs == 2 && Op.Output.Size &&
            Op.Output.Size <= sizeof(uint64_t) &&
            Op.Inputs[0].Size == Op.Output.Size &&
            Op.Inputs[1].Size == Op.Output.Size && !Op.Dead &&
            Op.RegistrationRoot == MedOp::RegistrationRootKind::None &&
            !Op.SourceCallHint && Op.IntrinsicOutputs.empty() &&
            Op.MemoryOrdering == NdMemoryOrdering::None &&
            Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
          std::optional<uint64_t> Absorbed;
          for (const auto *Value : {&A, &B})
            if (*Value && (*Value)->IsStableScalar)
              if (auto Bits = absorbingBitwiseConstant(
                      Op.Opcode, Op.Output.Size, (*Value)->Value))
                Absorbed = Bits;
          if (Absorbed) {
            Op.Opcode = NdOp::COPY;
            Op.Inputs[0] = MedVar::makeConst(*Absorbed, Op.Output.Size,
                                             ConstantAddressProvenance::Scalar);
            Op.NumInputs = 1;
            Changed = true;
            continue;
          }
        }
        if (!A || !B)
          continue;

        // The operands' bytes, unsigned and signed.
        const uint16_t InSize = Op.Inputs[0].Size;
        const unsigned InBits = InSize > 0 && InSize < 8 ? InSize * 8u : 64u;
        const auto Unsigned = [&](uint64_t Value) {
          return InBits == 64 ? Value : Value & ((1ULL << InBits) - 1);
        };
        const auto Signed = [&](uint64_t Value) {
          return InBits == 64 ? static_cast<int64_t>(Value)
                              : static_cast<int64_t>(Value << (64 - InBits)) >>
                                    (64 - InBits);
        };
        uint64_t Result = 0;
        switch (Op.Opcode) {
        case NdOp::INT_LEFT:
          Result = B->Value >= InBits ? 0 : A->Value << B->Value;
          break;
        case NdOp::INT_RIGHT:
          Result = B->Value >= InBits ? 0 : Unsigned(A->Value) >> B->Value;
          break;
        case NdOp::INT_ASHR:
          Result = static_cast<uint64_t>(
              Signed(A->Value) >> (B->Value >= InBits ? InBits - 1 : B->Value));
          break;
        case NdOp::INT_EQUAL:
          Result = Unsigned(A->Value) == Unsigned(B->Value);
          break;
        case NdOp::INT_NOTEQUAL:
          Result = Unsigned(A->Value) != Unsigned(B->Value);
          break;
        case NdOp::INT_LESS:
          Result = Unsigned(A->Value) < Unsigned(B->Value);
          break;
        case NdOp::INT_SLESS:
          Result = Signed(A->Value) < Signed(B->Value);
          break;
        case NdOp::INT_LESSEQUAL:
          Result = Unsigned(A->Value) <= Unsigned(B->Value);
          break;
        case NdOp::INT_SLESSEQUAL:
          Result = Signed(A->Value) <= Signed(B->Value);
          break;
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
        // An address less itself is 0 however the loader places it: the same
        // object, at the same offset, on both sides.
        const bool SameAddress = Op.Opcode == NdOp::INT_SUB &&
                                 A->Value == B->Value && A->HasAddressOrigin &&
                                 B->HasAddressOrigin &&
                                 A->AddressOwnerVA == B->AddressOwnerVA;
        if ((A->MayRelocate || B->MayRelocate) && !SameAddress) {
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
        } else if (SameAddress ||
                   (A->Provenance == ConstantAddressProvenance::Scalar &&
                    B->Provenance == ConstantAddressProvenance::Scalar))
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

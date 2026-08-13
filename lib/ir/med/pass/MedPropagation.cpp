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

#include "neverd/ir/med/LowToMed.h"

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

        auto ResolveConst = [&](const MedVar &V) -> std::optional<uint64_t> {
          if (V.isConst())
            return V.ConstVal;
          auto It = Defs.find(keyOf(V));
          if (It != Defs.end() && It->second->Opcode == NdOp::COPY &&
              It->second->NumInputs == 1 && It->second->Inputs[0].isConst())
            return It->second->Inputs[0].ConstVal;
          return std::nullopt;
        };

        auto A = ResolveConst(Op.Inputs[0]);
        auto B = ResolveConst(Op.Inputs[1]);
        if (!A || !B)
          continue;

        uint64_t Result = 0;
        switch (Op.Opcode) {
        case NdOp::INT_ADD:
          Result = *A + *B;
          break;
        case NdOp::INT_SUB:
          Result = *A - *B;
          break;
        case NdOp::INT_AND:
          Result = *A & *B;
          break;
        case NdOp::INT_OR:
          Result = *A | *B;
          break;
        case NdOp::INT_XOR:
          Result = *A ^ *B;
          break;
        case NdOp::INT_MULT:
          Result = *A * *B;
          break;
        default:
          continue;
        }

        uint16_t OutSz = Op.Output.Size;
        if (OutSz > 0 && OutSz < 8) {
          uint64_t Mask = (1ULL << (OutSz * 8)) - 1;
          Result &= Mask;
        }

        Op.Opcode = NdOp::COPY;
        Op.Inputs[0] = MedVar::makeConst(Result, Op.Output.Size);
        Op.NumInputs = 1;
        Changed = true;
      }
    }
  }
}

} // namespace neverd

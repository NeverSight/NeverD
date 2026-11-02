//===- X86NativeMemory.h - Native raw-address roles -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep segment offsets and physical addresses scalar across both type layers.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_MED_X86_X86NATIVEMEMORY_H
#define NEVERD_IR_MED_X86_X86NATIVEMEMORY_H

#include "neverd/ir/intrinsics/X86/X86NativeMemory.h"
#include "neverd/ir/med/MedIR.h"

#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace neverd {
inline bool x86NativeMemoryHasSegmentOffset(const MedOp &Op) {
  if (Op.Opcode != NdOp::INTRINSIC || !Op.NumInputs ||
      Op.Inputs.size() < Op.NumInputs || !Op.Inputs[0].isConst())
    return false;
  const auto &Control = Op.Inputs[Op.NumInputs - 1];
  return Control.isConst() &&
         x86NativeMemoryHasSegmentOffset(Intrinsic(Op.Inputs[0].ConstVal),
                                         Op.NumInputs, Control.ConstVal);
}

/// Physical addresses consume original integer bits. Follow their SSA value
/// dependencies through register slices and arithmetic, stopping at memory and
/// call producers. Both parameter-inference layers must exclude these values
/// from a process-pointer ABI, even beside an ordinary memory use.
inline std::vector<MedVar> x86PhysicalAddressInputValues(const MedFunc &Func) {
  std::vector<MedVar> Pending;
  for (const auto &Block : Func.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs == 2 &&
          Op.Inputs.size() >= Op.NumInputs && Op.Inputs[0].isConst() &&
          isX86SystemStatePhysicalAddressArgument(
              Intrinsic(Op.Inputs[0].ConstVal), 0))
        Pending.push_back(Op.Inputs[1]);
  if (Pending.empty())
    return {};
  std::map<std::pair<int, int>, const MedOp *> Definitions;
  std::map<std::pair<int, int>, const PhiNode *> Phis;
  for (const auto &Block : Func.Blocks) {
    for (const auto &Phi : Block.Phis)
      Phis[{Phi.Output.Id, Phi.Output.SSAVer}] = &Phi;
    for (const auto &Op : Block.Ops)
      if (Op.Output.Id >= 0 && Op.Output.Size)
        Definitions[{Op.Output.Id, Op.Output.SSAVer}] = &Op;
  }
  std::vector<MedVar> Values;
  std::set<std::tuple<int, int, int>> Seen;
  while (!Pending.empty()) {
    const auto Value = Pending.back();
    Pending.pop_back();
    if (Value.isConst() || Value.Id < 0 ||
        !Seen.emplace(Value.Kind, Value.Id, Value.SSAVer).second)
      continue;
    Values.push_back(Value);
    if (const auto It = Phis.find({Value.Id, Value.SSAVer}); It != Phis.end()) {
      for (const auto &[Predecessor, Input] : It->second->Args)
        Pending.push_back(Input);
      if (It->second->ExceptionalEntry)
        Pending.push_back(*It->second->ExceptionalEntry);
      continue;
    }
    const auto It = Definitions.find({Value.Id, Value.SSAVer});
    if (It == Definitions.end())
      continue;
    const auto &Op = *It->second;
    switch (Op.Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
    case NdOp::SUBBYTES:
    case NdOp::CONCAT:
    case NdOp::CAST:
    case NdOp::INSERT:
    case NdOp::EXTRACT:
    case NdOp::SELECT:
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
    case NdOp::INT_MULT:
    case NdOp::INT_DIV:
    case NdOp::INT_SDIV:
    case NdOp::INT_REM:
    case NdOp::INT_SREM:
    case NdOp::INT_AND:
    case NdOp::INT_OR:
    case NdOp::INT_XOR:
    case NdOp::INT_LEFT:
    case NdOp::INT_RIGHT:
    case NdOp::INT_ASHR:
    case NdOp::INT_NEGATE:
    case NdOp::INT_NEG2:
    case NdOp::INT_NOT:
    case NdOp::INT_CARRY:
    case NdOp::INT_SOVF:
    case NdOp::INT_SBOR:
    case NdOp::INT_EQUAL:
    case NdOp::INT_NOTEQUAL:
    case NdOp::INT_LESS:
    case NdOp::INT_SLESS:
    case NdOp::INT_LESSEQUAL:
    case NdOp::INT_SLESSEQUAL:
    case NdOp::BOOL_AND:
    case NdOp::BOOL_OR:
    case NdOp::BOOL_XOR:
    case NdOp::BOOL_NOT:
    case NdOp::POPCOUNT:
    case NdOp::LZCOUNT:
      for (uint8_t I = 0; I < Op.NumInputs && I < Op.Inputs.size(); ++I)
        Pending.push_back(Op.Inputs[I]);
      break;
    default:
      break;
    }
  }
  return Values;
}
} // namespace neverd
#endif // NEVERD_IR_MED_X86_X86NATIVEMEMORY_H

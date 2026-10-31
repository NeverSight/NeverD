//===- ComparisonStackRoles.cpp - Fixed-frame address-use evidence --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Retains the role of each observed stack address while allowing local layout
/// permutations to propose review alternatives to literal body similarity.
///
//===----------------------------------------------------------------------===//

#include "ComparisonStackRoles.h"

#include "neverd/diff/Limits.h"
#include "neverd/diff/StableHash.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <tuple>
#include <vector>

namespace neverd::sdk::x86 {

StackRoleUse readStackRoleUse(csh Handle, const cs_insn &I,
                              diff::InstructionFlow Flow) {
  StackRoleUse R;
  const auto Refuse = [&] {
    R.Effect = StackRoleEffect::Unsafe;
    return R;
  };
  if (Flow == diff::InstructionFlow::Unknown)
    return Refuse();
  const auto &X = I.detail->x86;
  if (X.addr_size != sizeof(uint64_t))
    return Refuse();
  const auto StackPointer = [](unsigned Reg) {
    return Reg == X86_REG_RSP || Reg == X86_REG_ESP || Reg == X86_REG_SP ||
           Reg == X86_REG_SPL;
  };
  const bool Adjust = (I.id == X86_INS_SUB || I.id == X86_INS_ADD) &&
                      X.op_count == 2 && X.operands[0].type == X86_OP_REG &&
                      X.operands[0].reg == X86_REG_RSP &&
                      X.operands[1].type == X86_OP_IMM && X.operands[1].imm > 0;
  for (unsigned N = 0; N < X.op_count; ++N) {
    const auto &O = X.operands[N];
    if (O.type == X86_OP_REG && StackPointer(O.reg) && !Adjust)
      return Refuse();
    if (O.type != X86_OP_MEM)
      continue;
    if (O.mem.base == X86_REG_RBP || O.mem.base == X86_REG_EBP ||
        O.mem.base == X86_REG_BP || O.mem.base == X86_REG_BPL ||
        StackPointer(O.mem.index))
      return Refuse();
    if (!StackPointer(O.mem.base))
      continue;
    if (O.mem.base != X86_REG_RSP || O.mem.index != X86_REG_INVALID ||
        (O.mem.segment != X86_REG_INVALID && O.mem.segment != X86_REG_SS) ||
        R.Offset)
      return Refuse();
    R.Offset = O.mem.disp;
    R.Operand = N;
    R.AddressTaken = I.id == X86_INS_LEA;
    R.Width = R.AddressTaken ? 0 : O.size;
    if (!R.AddressTaken && !R.Width)
      return Refuse();
  }
  cs_regs Reads, Writes;
  uint8_t ReadCount = 0, WriteCount = 0;
  if (cs_regs_access(Handle, &I, Reads, &ReadCount, Writes, &WriteCount) !=
      CS_ERR_OK)
    return Refuse();
  for (unsigned N = 0; N < WriteCount; ++N)
    if (StackPointer(Writes[N])) {
      if (Adjust) {
        R.Effect = I.id == X86_INS_SUB ? StackRoleEffect::Allocate
                                       : StackRoleEffect::Release;
        R.Adjustment = X.operands[1].imm;
      } else if (Flow != diff::InstructionFlow::Call &&
                 Flow != diff::InstructionFlow::IndirectCall &&
                 Flow != diff::InstructionFlow::Return)
        return Refuse();
    }
  return R;
}

uint64_t stackRoleCandidate(llvm::ArrayRef<StackRoleUse> Uses, size_t EntryEnd,
                            const diff::ComparisonControl &Control) {
  uint64_t Frame = 0;
  size_t Allocate = Uses.size(), Release = Uses.size();
  for (size_t I = 0; I < Uses.size(); ++I) {
    if (Control.stop() || Uses[I].Effect == StackRoleEffect::Unsafe)
      return 0;
    if (Uses[I].Effect == StackRoleEffect::Allocate) {
      if (Allocate != Uses.size() || I >= EntryEnd)
        return 0;
      Allocate = I;
      Frame = Uses[I].Adjustment;
    } else if (Uses[I].Effect == StackRoleEffect::Release) {
      if (!Frame || Uses[I].Adjustment != Frame)
        return 0;
      Release = std::min(Release, I);
    }
  }
  if (!Frame || Release == Uses.size())
    return 0;
  std::vector<size_t> Indices;
  Indices.reserve(Uses.size());
  for (size_t I = 0; I < Uses.size(); ++I) {
    if (Control.stop())
      return 0;
    const auto &U = Uses[I];
    if (!U.Offset)
      continue;
    if (I <= Allocate || I >= Release || *U.Offset < 0 ||
        uint64_t(*U.Offset) >= Frame || U.Width > Frame - uint64_t(*U.Offset))
      return 0;
    Indices.push_back(I);
  }
  llvm::sort(Indices, [&](size_t A, size_t B) {
    return std::tie(Uses[A].Offset, A) < std::tie(Uses[B].Offset, B);
  });
  unsigned Addressed = 0;
  bool Observed = false;
  uint64_t End = 0;
  std::vector<uint64_t> Traces;
  Traces.reserve(Indices.size());
  for (size_t I = 0; I < Indices.size();) {
    const uint64_t Offset = *Uses[Indices[I]].Offset;
    if (Control.stop() || Offset < End)
      return 0;
    diff::StableHash Trace;
    uint64_t Width = 0;
    bool AddressTaken = false, MemoryUse = false;
    size_t J = I;
    do {
      if (Control.stop())
        return 0;
      const auto &U = Uses[Indices[J++]];
      Trace.add(U.Shape).add(U.Operand).add(U.Width).add(U.AddressTaken);
      Width = std::max<uint64_t>(Width, U.Width);
      AddressTaken |= U.AddressTaken;
      MemoryUse |= !U.AddressTaken;
    } while (J < Indices.size() && *Uses[Indices[J]].Offset == Offset);
    End = Offset + Width;
    Addressed += AddressTaken;
    Observed |= AddressTaken && MemoryUse;
    Traces.push_back(Trace.value());
    I = J;
  }
  if (Addressed < limits::diff::kMinStackRoleAddressedSlots || !Observed)
    return 0;
  llvm::sort(Traces);
  diff::StableHash Key;
  Key.add(Frame).add(Traces.size());
  for (uint64_t Trace : Traces)
    Key.add(Trace);
  return Key.value();
}

} // namespace neverd::sdk::x86

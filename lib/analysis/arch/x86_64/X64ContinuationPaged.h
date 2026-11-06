//===- X64ContinuationPaged.h - Scalar operand roles ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve original stack-reference and read-modify-write identities before
/// source lowering turns architectural registers into private state loads.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_ANALYSIS_ARCH_X86_64_X64CONTINUATIONPAGED_H
#define NEVERD_ANALYSIS_ARCH_X86_64_X64CONTINUATIONPAGED_H

#include "X64PagedSse.h"

#include "neverd/analysis/InterpreterSpecialization.h"
#include "neverd/decode/Decoder.h"
#include "neverd/support/X86/X86Addressing.h"

namespace neverd::analysis::detail {
struct X64PagedMemoryRoles {
  bool Supported = false;
  bool StackRead = false;
  bool StackWrite = false;
  bool ReadModifyWrite = false;
  bool Sse = false;
  bool SseMemory = false;
};

inline X64PagedMemoryRoles
x64PagedMemoryRoles(const SpecializationInstruction &Instruction) {
  X64PagedMemoryRoles Roles;
  Decoder Decode;
  if (!Decode.init(Arch::X64))
    return Roles;
  DecodedInsn Insn{};
  if (!Decode.decodeOneForLift(Instruction.NativeBytes.data(),
                               Instruction.NativeBytes.size(),
                               Instruction.Origin.Address, Insn) ||
      Insn.Size != Instruction.NativeBytes.size() || !Insn.Raw->detail)
    return Roles;
  const auto &X86 = Insn.Raw->detail->x86;
  Roles.Sse = x64PagedSseInstruction(Insn.Id, X86);
  if (X86.prefix[0]) // LOCK and repeat require their instruction commit model.
    return Roles;
  const bool StackRead = Insn.Id == X86_INS_POP || Insn.Id == X86_INS_RET ||
                         Insn.Id == X86_INS_POPFQ;
  const bool StackWrite = Insn.Id == X86_INS_PUSH || Insn.Id == X86_INS_CALL ||
                          Insn.Id == X86_INS_PUSHFQ;
  unsigned ExplicitMemory = 0;
  bool ExplicitStack = false;
  for (unsigned I = 0; I != X86.op_count; ++I) {
    const auto &Operand = X86.operands[I];
    if (Operand.type != X86_OP_MEM)
      continue;
    ++ExplicitMemory;
    ExplicitStack = X86.addr_size == 8 && Operand.mem.segment != X86_REG_FS &&
                    Operand.mem.segment != X86_REG_GS &&
                    isStackDefaultBase(Operand.mem.base);
    Roles.ReadModifyWrite = (Operand.access & (CS_AC_READ | CS_AC_WRITE)) ==
                            (CS_AC_READ | CS_AC_WRITE);
  }
  if (ExplicitMemory > 1)
    return Roles;
  unsigned Reads = 0, Writes = 0;
  for (const auto &Op : Instruction.Ops) {
    if (Op.MemoryOrdering != NdMemoryOrdering::None)
      return Roles;
    if (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) {
      const bool Write = Op.Opcode == NdOp::STORE;
      const auto Bytes = Write ? Op.Inputs[1].Size : Op.Output.Size;
      if ((Bytes != 1 && Bytes != 2 && Bytes != 4 && Bytes != 8 &&
           !(Roles.Sse && Bytes == 16)) ||
          Writes)
        return Roles;
      Write ? ++Writes : ++Reads;
    }
  }
  if (Reads > 1 || Writes > 1)
    return Roles;
  Roles.StackRead = StackRead || (!StackWrite && ExplicitStack);
  // PUSH/CALL read explicit memory before their implicit stack write. POP
  // reads its implicit stack before an independently addressed destination.
  if (StackWrite)
    Roles.StackRead = ExplicitStack;
  Roles.StackWrite = StackWrite || ExplicitStack;
  Roles.SseMemory = Roles.Sse && ExplicitMemory != 0;
  Roles.Supported = true;
  return Roles;
}
} // namespace neverd::analysis::detail
#endif // NEVERD_ANALYSIS_ARCH_X86_64_X64CONTINUATIONPAGED_H

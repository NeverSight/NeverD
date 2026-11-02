//===- X86MpxExecution.cpp - Execute qualified MPX instructions ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind byte-owned operand roles to the shared MPX state and memory owner.
//===----------------------------------------------------------------------===//

#include "neverd/lift/X86/X86MpxExecution.h"

#include "neverd/lift/X86/X86MpxEncoding.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/support/X86/X86Addressing.h"

#include "llvm/Support/ErrorHandling.h"

namespace neverd {
namespace {

std::optional<uint64_t> general(const X86MpxExecutionInputs &Inputs,
                                x86_reg Register, uint64_t NextIP) {
  if (Register == X86_REG_INVALID)
    return 0;
  if (Register == X86_REG_RIP)
    return NextIP;
  // Only the byte reader's authenticated native-width GPR operands reach here.
  return Inputs.GPR[mapCapstoneReg(Register).Offset / x86reg::GeneralRegStride];
}

std::optional<uint64_t> effectiveAddress(const X86MpxExecutionInputs &Inputs,
                                         const x86_op_mem &Memory,
                                         uint64_t NextIP) {
  const auto Base = general(Inputs, Memory.base, NextIP);
  const auto Index = general(Inputs, Memory.index, NextIP);
  if (!Base || !Index)
    return std::nullopt;
  return *Base + *Index * Memory.scale + uint64_t(Memory.disp);
}

X86SegmentRegister segment(const x86_op_mem &Memory) {
  switch (Memory.segment) {
#define X86_SEGMENT_REGISTER(ID, NUMBER, NAME)                                 \
  case X86_REG_##ID:                                                           \
    return X86SegmentRegister::ID;
#include "neverd/ir/intrinsics/X86/X86SegmentRegisters.def"
  default:
    break;
  }
  return isStackDefaultBase(Memory.base) ? X86SegmentRegister::SS
                                         : X86SegmentRegister::DS;
}

X86MpxResult execute(const X86MpxEncoding &E, X86MpxMode Mode, va_t Address,
                     X86MpxContext &Context,
                     const X86MpxExecutionInputs &Inputs,
                     X86MpxMemoryProvider &Memory) {
  if (E.Id == X86_INS_NOP)
    return X86MpxResult::Completed;
  const auto &X = E.Detail;
  const bool Store = X.operands[0].type == X86_OP_MEM;
  const unsigned Bound = X.operands[Store ? 1 : 0].reg - X86_REG_BND0;
  const auto Ready = Context.operationStatus(Mode, Bound);
  if (Ready != X86MpxResult::Completed)
    return Ready;
  const auto &Operand = X.operands[Store ? 0 : 1];
  if (E.Id == X86_INS_BNDMOV && Operand.type == X86_OP_REG)
    return Context.move(Mode, Bound, Operand.reg - X86_REG_BND0);

  const auto NextIP = Address + E.Size;
  if (E.Id == X86_INS_BNDMOV) {
    const auto Offset = effectiveAddress(Inputs, Operand.mem, NextIP);
    const auto Segment = segment(Operand.mem);
    const bool DefaultStack = isStackDefaultBase(Operand.mem.base);
    return Store ? Context.store(Mode, Bound, Offset, X.prefix[0] == 0xf0,
                                 Memory, Segment, DefaultStack)
                 : Context.load(Mode, Bound, Offset, Memory, Segment,
                                DefaultStack);
  }
  if (E.Id == X86_INS_BNDMK)
    return Context.make(Mode, Bound, general(Inputs, Operand.mem.base, NextIP),
                        effectiveAddress(Inputs, Operand.mem, NextIP));
  if (E.Id == X86_INS_BNDCL || E.Id == X86_INS_BNDCU || E.Id == X86_INS_BNDCN) {
    const auto Value = Operand.type == X86_OP_REG
                           ? general(Inputs, Operand.reg, NextIP)
                           : effectiveAddress(Inputs, Operand.mem, NextIP);
    const auto Kind = E.Id == X86_INS_BNDCL   ? X86MpxCheck::Lower
                      : E.Id == X86_INS_BNDCU ? X86MpxCheck::UpperComplement
                                              : X86MpxCheck::UpperRaw;
    return Context.check(Mode, Kind, Bound, Value);
  }
  if (E.Id != X86_INS_BNDLDX && E.Id != X86_INS_BNDSTX)
    llvm_unreachable("unexpected qualified MPX instruction");

  // MIB is not an ordinary effective address: absent base discards disp, and
  // the unscaled index is the pointer-comparison value, not address arithmetic.
  const auto &MIB = Operand.mem;
  const auto Base = general(Inputs, MIB.base, NextIP);
  const auto Pointer = general(Inputs, MIB.index, NextIP);
  if (!Base || !Pointer)
    return X86MpxResult::UnknownOperand;
  const auto Segment = segment(MIB);
  uint64_t SegmentBase = 0;
  if (Mode == X86MpxMode::Bits32 || Segment == X86SegmentRegister::FS ||
      Segment == X86SegmentRegister::GS) {
    const auto Supplied = Inputs.SegmentBases[unsigned(Segment)];
    if (!Supplied)
      return X86MpxResult::UnknownContext;
    SegmentBase = *Supplied;
  }
  const uint64_t Offset =
      MIB.base == X86_REG_INVALID ? 0 : *Base + uint64_t(MIB.disp);
  // Translation deliberately performs no memory or canonical-address probe.
  const uint64_t Linear = Offset + SegmentBase;
  return Store ? Context.storeTable(Mode, Bound, Linear, *Pointer, Memory)
               : Context.loadTable(Mode, Bound, Linear, *Pointer, Memory);
}

} // namespace

X86MpxExecutionResult
executeX86MpxInstruction(llvm::ArrayRef<uint8_t> Bytes, Arch Target,
                         va_t Address, X86MpxContext &Context,
                         const X86MpxExecutionInputs &Inputs,
                         X86MpxMemoryProvider &Memory) {
  const auto Encoding = decodeX86MpxEncoding(Bytes, Target);
  if (!Encoding)
    return {};
  return {execute(*Encoding,
                  Target == Arch::X64 ? X86MpxMode::Bits64 : X86MpxMode::Bits32,
                  Address, Context, Inputs, Memory),
          Encoding->Size};
}

} // namespace neverd

//===- SymExec.cpp - Symbolic execution of NeverD IR ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The operator semantics.  One switch, and it is the whole of what a symbolic
/// engine for six architectures needs, because the lifters already turned six
/// instruction sets into this one.
///
/// Two rules run through all of it.
///
/// An operation happens at the width of its operands and its result is then
/// fitted to the width of its destination.  Doing the arithmetic in a machine
/// word and masking afterwards agrees for addition and disagrees for a signed
/// comparison, a division or a shift, so the width is never widened for
/// convenience.
///
/// An operation the engine cannot model exactly writes a fresh input to its
/// destination rather than stopping.  A call, a square root, a vector
/// intrinsic: the value becomes an unknown with a name and execution
/// continues.  The explorer records that abstraction so callers can
/// distinguish a complete walk from an exact one.
///
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymExec.h"

#include "neverd/ir/X86/X87GuardedValue.h"
#include "neverd/ir/X86/X87Stack.h"
#include "neverd/ir/X86/X87Value.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/LowRawEnvironment.h"
#include "neverd/ir/low/X86/X87PartialRemainder.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/symbolic/SymIntegerIntrinsics.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"

#include <array>
#include <cassert>
#include <stdexcept>

namespace neverd::symbolic {

namespace {

constexpr uint32_t kByteBits = 8;
constexpr uint16_t kX87TopMask = 0x3800;

/// Widest operand the bit-count models are expanded over.  Each of them costs
/// a node per bit, so letting a 256-bit word through would turn one operation
/// into several hundred nodes and charge every later pass for it.  Above this
/// the operation is named instead, which is the same answer the engine gives
/// anything else it declines to write out.
constexpr uint32_t kMaxBitCountWidth = 64;

/// Width of an operand in bits, or zero when it does not declare one.
uint32_t widthOf(const NdVar &V) { return uint32_t(V.Size) * kByteBits; }

SymSpace spaceOf(const NdVar &V) {
  return V.isTemp() ? SymSpace::Temporary : SymSpace::Register;
}

bool isAtomicWrite(NdOp Opcode) {
  return Opcode == NdOp::ATOMIC_XCHG || Opcode == NdOp::ATOMIC_ADD ||
         Opcode == NdOp::ATOMIC_CMPXCHG;
}

} // namespace

bool supportsAtomicWritePreservation(const LowOp &Op) {
  const bool Compare = Op.Opcode == NdOp::ATOMIC_CMPXCHG;
  if (!isAtomicWrite(Op.Opcode) || Op.NumInputs != (Compare ? 3u : 2u) ||
      Op.MemoryOrdering != NdMemoryOrdering::SequentiallyConsistent ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  const auto Scalar = [](const NdVar &V) {
    return V.Size && (V.isConst() || V.isReg() || V.isTemp()) &&
           (V.isConst() || V.Offset <= UINT64_MAX - (V.Size - 1));
  };
  const auto Bytes = Op.Output.Size;
  return (Bytes == 1 || Bytes == 2 || Bytes == 4 || Bytes == 8) &&
         (Op.Output.isReg() || Op.Output.isTemp()) && Scalar(Op.Output) &&
         Op.Inputs[0].Size == 8 && Scalar(Op.Inputs[0]) &&
         Op.Inputs[1].Size == Bytes && Scalar(Op.Inputs[1]) &&
         (!Compare || (Op.Inputs[2].Size == Bytes && Scalar(Op.Inputs[2])));
}

bool supportsCmpxchgPreservation(const LowOp &Op) {
  return Op.Opcode == NdOp::ATOMIC_CMPXCHG &&
         supportsAtomicWritePreservation(Op);
}

//===----------------------------------------------------------------------===//
// Reading and writing operands
//===----------------------------------------------------------------------===//

SymRef SymExec::read(const NdVar &V) {
  const uint32_t Width = widthOf(V) ? widthOf(V) : 64;
  if (V.isConst())
    return Ctx.mkConst(Width, V.Offset);
  if (V.isRam())
    return State.load(Ctx.mkConst(64, V.Offset), V.Size ? V.Size : uint16_t(8));
  return State.read(spaceOf(V), V.Offset, V.Size ? V.Size : uint16_t(8));
}

SymRef SymExec::operandValue(const NdVar &V) { return read(V); }

SymRef SymExec::memoryAddressValue(const LowOp &Op) {
  const auto Memory = lowMemoryOperands(Op);
  if (!Memory.Complete)
    return {};
  if (Op.MemoryAddressSpace == NdMemoryAddressSpace::Default)
    return read(*Memory.Address);
  const auto Base = State.x64MemoryAddressSpaceBase(Op.MemoryAddressSpace);
  if (!Base)
    return {};
  // The legacy address-space token, when present, is metadata, not an address.
  const bool Load = Op.Opcode == NdOp::LOAD;
  const bool Store = Op.Opcode == NdOp::STORE;
  const unsigned Arity = Load ? 1 : Op.Opcode == NdOp::ATOMIC_CMPXCHG ? 3 : 2;
  const bool HasToken = (Load || Store) && Op.NumInputs == Arity + 1;
  if ((!HasToken && Op.NumInputs != Arity) ||
      (HasToken && !Op.Inputs[0].isConst()))
    return {};
  const auto Scalar = [](const NdVar &V) {
    return V.Size && (V.isConst() || V.isReg() || V.isTemp()) &&
           (V.isConst() || V.Offset <= UINT64_MAX - (V.Size - 1));
  };
  if (!Scalar(*Memory.Address) || Memory.Address->Size > 8 ||
      (Store ? Op.Output.Size != 0
             : !(Op.Output.isReg() || Op.Output.isTemp()) ||
                   !Scalar(Op.Output)) ||
      (Memory.StoredValue && !Scalar(*Memory.StoredValue)) ||
      (Memory.ExpectedValue && !Scalar(*Memory.ExpectedValue)))
    return {};
  const auto Offset = read(*Memory.Address);
  if (!Offset)
    return {};
  return Ctx.mkAdd(Base, Ctx.mkZExtOrTrunc(Offset, 64));
}

SymRef SymExec::fit(SymRef Value, uint32_t Bits) {
  return Bits == 0 ? Value : Ctx.mkZExtOrTrunc(Value, Bits);
}

void SymExec::writeResult(const NdVar &Output, SymRef Value) {
  const uint32_t Width = widthOf(Output);
  if (Width == 0)
    return;
  SymRef Fitted = fit(Value, Width);
  if (Output.isRam()) {
    State.store(Ctx.mkConst(64, Output.Offset), Fitted);
    return;
  }
  if (Output.isConst())
    return;
  State.write(spaceOf(Output), Output.Offset, Fitted);
}

StepResult SymExec::unmodelled(const LowOp &Op) {
  ++Unmodelled;
  ++OpaqueOperations;
  // Opaque system instructions can change FS/GS without a LowIR output.
  State.clearX64MemoryAddressSpaceBases();
  if (widthOf(Op.Output) != 0)
    writeResult(Op.Output, State.freshInput("undef", widthOf(Op.Output)));
  return StepResult::Unmodelled;
}

StepResult SymExec::stepX87Intrinsic(const LowOp &Op) {
  auto unknownX87State = [&]() {
    StepResult Result = unmodelled(Op);
    State.write(SymSpace::Register, x86reg::FPU_SW,
                State.freshInput("x87_status", 16));
    State.write(SymSpace::Register, x86reg::FPU_CW,
                State.freshInput("x87_control", 16));
    for (unsigned Index = 0; Index != x86reg::FPUStackDepth; ++Index)
      State.write(SymSpace::Register, x86reg::stReg(Index),
                  State.freshInput("x87_stack", 80));
    return Result;
  };
  if (Op.NumInputs == 0 || !Op.Inputs[0].isConst() ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return unknownX87State();

  const Intrinsic Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  if (x87ValueInfo(Id) &&
      !x87ValueShapeIsValid(x87ValueLowShape(Op, Arch::Unknown)))
    return unknownX87State();
  if (Id == Intrinsic::X87Fninit) {
    if (Op.NumInputs != 1 || Op.Output.Size != 0)
      return unknownX87State();
    State.write(SymSpace::Register, x86reg::FPU_SW, Ctx.mkZero(16));
    State.write(SymSpace::Register, x86reg::FPU_CW, Ctx.mkConst(16, 0x037f));
    for (unsigned Index = 0; Index != x86reg::FPUStackDepth; ++Index)
      State.write(SymSpace::Register, x86reg::stReg(Index),
                  State.freshInput("x87_empty", 80));
    // Tags are not represented by SymState. Keep the reset's known status and
    // control word, but report the unmodelled empty-stack state.
    ++Unmodelled;
    ++OpaqueOperations;
    return StepResult::Unmodelled;
  }
  if (isX87GuardedValueIntrinsic(Id)) {
    if (!x87GuardedValueShapeIsValid(
            x87GuardedValueLowShape(Op, Arch::Unknown)))
      return unknownX87State();
    const auto Result = unmodelled(Op);
    State.write(SymSpace::Register, x86reg::FPU_SW,
                State.freshInput("x87_guarded_status", 16));
    return Result;
  }
  if (Id == Intrinsic::X87Wait) {
    if (Op.NumInputs != 1 || Op.Output.Size != 0)
      return unknownX87State();
    const auto Status =
        Ctx.asConst(State.read(SymSpace::Register, x86reg::FPU_SW, 2));
    const auto Control =
        Ctx.asConst(State.read(SymSpace::Register, x86reg::FPU_CW, 2));
    if (!Status || !Control || (Status->getZExtValue() & 0x80) != 0 ||
        (Status->getZExtValue() & ~Control->getZExtValue() & 0x3f) != 0)
      return unmodelled(Op);
    return StepResult::Continue;
  }
  if (Id == Intrinsic::X87Fnclex) {
    if (Op.NumInputs != 1 || Op.Output.Size != 0)
      return unknownX87State();
    SymRef Status = State.read(SymSpace::Register, x86reg::FPU_SW, 2);
    State.write(SymSpace::Register, x86reg::FPU_SW,
                Ctx.mkAnd(Status, Ctx.mkConst(16, ~uint16_t(0x80ff))));
    return StepResult::Continue;
  }
  if (const auto Stack = x87StackEffect(Id); Stack && Stack->FreesOperand) {
    if (Op.NumInputs != 3 || Op.Output.Size != 0 || !Op.Inputs[1].isReg() ||
        Op.Inputs[1].Size != x86reg::FPURegSize ||
        Op.Inputs[1].Offset < x86reg::ST0 ||
        Op.Inputs[1].Offset > x86reg::ST7 ||
        (Op.Inputs[1].Offset - x86reg::ST0) % x86reg::FPURegStride != 0 ||
        !Op.Inputs[2].isConst() || Op.Inputs[2].Size != 1 ||
        Op.Inputs[2].Offset >= x86reg::FPUStackDepth)
      return unknownX87State();
    // FFREE empties the tag for this slot and leaves its payload unusable.
    // Intel also leaves every x87 condition code undefined.
    State.write(SymSpace::Register, Op.Inputs[1].Offset,
                State.freshInput("x87_empty", 80));
    if (Stack->Pops) {
      const unsigned OldTop = (x86reg::stRegIndex(Op.Inputs[1].Offset) +
                               x86reg::FPUStackDepth - Op.Inputs[2].Offset) &
                              7;
      State.write(SymSpace::Register, x86reg::stReg(OldTop),
                  State.freshInput("x87_popped", 80));
    }
    SymRef Status = State.read(SymSpace::Register, x86reg::FPU_SW, 2);
    const uint16_t ChangedMask =
        X87PartialRemainderConditionMask | (Stack->Pops ? kX87TopMask : 0);
    SymRef Preserved = Ctx.mkAnd(Status, Ctx.mkConst(16, ~ChangedMask));
    SymRef UnknownCodes = Ctx.mkAnd(State.freshInput("x87_ffree_codes", 16),
                                    Ctx.mkConst(16, ChangedMask));
    State.write(SymSpace::Register, x86reg::FPU_SW,
                Ctx.mkOr(Preserved, UnknownCodes));
    ++Unmodelled;
    ++OpaqueOperations;
    return StepResult::Unmodelled;
  }
  if (x87StackEffect(Id)) {
    if (Op.NumInputs != 1 || Op.Output.Size != 0)
      return unknownX87State();
    SymRef Status = State.read(SymSpace::Register, x86reg::FPU_SW, 2);
    constexpr uint16_t ChangedMask =
        kX87TopMask | X87PartialRemainderConditionMask;
    SymRef Cleared = Ctx.mkAnd(Status, Ctx.mkConst(16, ~ChangedMask));
    SymRef UnknownStatus = Ctx.mkAnd(State.freshInput("x87_fincstp_status", 16),
                                     Ctx.mkConst(16, ChangedMask));
    State.write(SymSpace::Register, x86reg::FPU_SW,
                Ctx.mkOr(Cleared, UnknownStatus));
    // The condition codes are undefined, and LowIR's physical ST mapping does
    // not keep the status-word TOP bits current across prior pushes and pops.
    // Neither field may be read back as a guessed constant.
    ++Unmodelled;
    ++OpaqueOperations;
    return StepResult::Unmodelled;
  }
  if (Id == Intrinsic::X87ReadStatus) {
    writeResult(Op.Output, State.read(SymSpace::Register, x86reg::FPU_SW, 2));
    return StepResult::Continue;
  }
  if (Id != Intrinsic::X87Fprem && Id != Intrinsic::X87Fprem1) {
    // X87ReadStatus now observes the tracked status word. Any x87 operation
    // whose status effect is still opaque must invalidate that word, or a
    // preceding known C2 value could survive an intervening status update.
    if (Id == Intrinsic::X87Op || Id == Intrinsic::Fxrstor ||
        Id == Intrinsic::Fxrstor64Mem || Id == Intrinsic::Xrstor ||
        Id == Intrinsic::Xrstors || Id == Intrinsic::Xrstor64 ||
        Id == Intrinsic::Xrstors64 ||
        (Id >= Intrinsic::X87Fsin && Id <= Intrinsic::X87ReadStatus) ||
        (Id >= Intrinsic::X87Fldenv && Id <= Intrinsic::X87Fnsave))
      return unknownX87State();
    return unmodelled(Op);
  }

  auto unknownRemainder = [&]() {
    StepResult Result = unmodelled(Op);
    // An unsupported remainder may still change the x87 condition codes and
    // exception status. A later FNSTSW must never see a stale known value.
    State.write(SymSpace::Register, x86reg::FPU_SW,
                State.freshInput("x87_status", 16));
    return Result;
  };
  if (State.byteOrder() != llvm::endianness::little)
    return unknownRemainder();

  auto concreteBytes =
      [&](const NdVar &Operand) -> std::optional<std::array<uint8_t, 10>> {
    if (!Operand.isReg() && !Operand.isTemp())
      return std::nullopt;
    const auto Value = Ctx.asConst(read(Operand));
    if (!Value || Value->getBitWidth() != 80)
      return std::nullopt;
    std::array<uint8_t, 10> Bytes{};
    for (unsigned Index = 0; Index != Bytes.size(); ++Index)
      Bytes[Index] =
          static_cast<uint8_t>(Value->extractBitsAsZExtValue(8, Index * 8));
    return Bytes;
  };
  const auto Dividend = concreteBytes(Op.Inputs[1]);
  const auto Divisor = concreteBytes(Op.Inputs[2]);
  if (!Dividend || !Divisor)
    return unknownRemainder();
  const auto Result = evaluateX87PartialRemainder(*Dividend, *Divisor,
                                                  Id == Intrinsic::X87Fprem1);
  if (!Result)
    return unknownRemainder();

  StepResult Step = StepResult::Continue;
  if (Result->Complete) {
    llvm::APInt ResultBits(80, 0);
    for (unsigned Index = 0; Index != Result->Value.size(); ++Index)
      ResultBits |= llvm::APInt(80, Result->Value[Index]) << (Index * 8);
    writeResult(Op.Output, Ctx.mkConst(ResultBits));
  } else {
    // Intel permits an implementation-selected reduction of 32 to 63 bits.
    // The chosen partial remainder is not an invariant of the x87 ISA.
    Step = unmodelled(Op);
  }

  SymRef OldStatus = State.read(SymSpace::Register, x86reg::FPU_SW, 2);
  SymRef Preserved = Ctx.mkAnd(
      OldStatus, Ctx.mkConst(16, ~uint16_t(X87PartialRemainderConditionMask |
                                           kX87TopMask)));
  const uint16_t KnownMask = Result->knownConditionMask();
  const uint16_t UnknownMask = X87PartialRemainderConditionMask & ~KnownMask;
  SymRef Codes = Ctx.mkConst(16, Result->ConditionCodes & KnownMask);
  if (UnknownMask != 0)
    Codes =
        Ctx.mkOr(Codes, Ctx.mkAnd(State.freshInput("x87_partial_quotient", 16),
                                  Ctx.mkConst(16, UnknownMask)));
  // LowIR keeps physical ST slots but does not update SW.TOP on FLD/FSTP.
  // FNSTSW after those stack operations must not inherit FNINIT's TOP=0.
  SymRef UnknownTop =
      Ctx.mkAnd(State.freshInput("x87_top", 16), Ctx.mkConst(16, kX87TopMask));
  State.write(SymSpace::Register, x86reg::FPU_SW,
              Ctx.mkOr(Ctx.mkOr(Preserved, Codes), UnknownTop));
  return Step;
}

//===----------------------------------------------------------------------===//
// Arithmetic and bitwise
//===----------------------------------------------------------------------===//

StepResult SymExec::stepBinary(const LowOp &Op) {
  if (Op.NumInputs < 2)
    return unmodelled(Op);

  SymRef A = read(Op.Inputs[0]);
  const uint32_t Width = Ctx.width(A);
  SymRef B = read(Op.Inputs[1]);
  // A shift count keeps its declared width: truncating a wider count can turn
  // an out-of-range shift into an in-range one.  The expression builders own
  // the exact shift semantics for both narrower and wider counts.
  if (Op.Opcode != NdOp::INT_LEFT && Op.Opcode != NdOp::INT_RIGHT &&
      Op.Opcode != NdOp::INT_ASHR)
    B = fit(B, Width);

  SymRef Result;
  switch (Op.Opcode) {
  case NdOp::INT_ADD:
    Result = Ctx.mkAdd(A, B);
    break;
  case NdOp::INT_SUB:
    Result = Ctx.mkSub(A, B);
    break;
  case NdOp::INT_MULT:
    Result = Ctx.mkMul(A, B);
    break;
  case NdOp::INT_AND:
    Result = Ctx.mkAnd(A, B);
    break;
  case NdOp::INT_OR:
    Result = Ctx.mkOr(A, B);
    break;
  case NdOp::INT_XOR:
    Result = Ctx.mkXor(A, B);
    break;
  case NdOp::INT_LEFT:
    Result = Ctx.mkShl(A, B);
    break;
  case NdOp::INT_RIGHT:
    Result = Ctx.mkLShr(A, B);
    break;
  case NdOp::INT_ASHR:
    Result = Ctx.mkAShr(A, B);
    break;
  case NdOp::INT_DIV:
    Result = Ctx.mkUDiv(A, B);
    break;
  case NdOp::INT_SDIV:
    Result = Ctx.mkSDiv(A, B);
    break;
  case NdOp::INT_REM:
    Result = Ctx.mkURem(A, B);
    break;
  case NdOp::INT_SREM:
    Result = Ctx.mkSRem(A, B);
    break;
  default:
    return unmodelled(Op);
  }

  writeResult(Op.Output, Result);
  return StepResult::Continue;
}

StepResult SymExec::stepCompare(const LowOp &Op) {
  if (Op.NumInputs < 2)
    return unmodelled(Op);

  SymRef A = read(Op.Inputs[0]);
  SymRef B = fit(read(Op.Inputs[1]), Ctx.width(A));

  SymRef Bit;
  switch (Op.Opcode) {
  case NdOp::INT_EQUAL:
    Bit = Ctx.mkEq(A, B);
    break;
  case NdOp::INT_NOTEQUAL:
    Bit = Ctx.mkNe(A, B);
    break;
  case NdOp::INT_LESS:
    Bit = Ctx.mkUlt(A, B);
    break;
  case NdOp::INT_LESSEQUAL:
    Bit = Ctx.mkUle(A, B);
    break;
  case NdOp::INT_SLESS:
    Bit = Ctx.mkSlt(A, B);
    break;
  case NdOp::INT_SLESSEQUAL:
    Bit = Ctx.mkSle(A, B);
    break;
  case NdOp::INT_CARRY:
    // The sum wrapped exactly when it came out below what went in.
    Bit = Ctx.mkUlt(Ctx.mkAdd(A, B), A);
    break;
  case NdOp::INT_SOVF: {
    // A signed sum overflows when both operands differ in sign from the
    // result, which is what testing the sign of the two differences together
    // says in one expression.
    SymRef Sum = Ctx.mkAdd(A, B);
    Bit = Ctx.mkSlt(Ctx.mkAnd(Ctx.mkXor(A, Sum), Ctx.mkXor(B, Sum)),
                    Ctx.mkZero(Ctx.width(A)));
    break;
  }
  case NdOp::INT_SBOR: {
    // A signed difference overflows when the operands differ in sign and the
    // result differs in sign from the first.
    SymRef Diff = Ctx.mkSub(A, B);
    Bit = Ctx.mkSlt(Ctx.mkAnd(Ctx.mkXor(A, B), Ctx.mkXor(A, Diff)),
                    Ctx.mkZero(Ctx.width(A)));
    break;
  }
  default:
    return unmodelled(Op);
  }

  writeResult(Op.Output, Bit);
  return StepResult::Continue;
}

StepResult SymExec::stepUnary(const LowOp &Op) {
  if (Op.NumInputs < 1)
    return unmodelled(Op);

  SymRef A = read(Op.Inputs[0]);
  const uint32_t OutWidth = widthOf(Op.Output);

  switch (Op.Opcode) {
  case NdOp::COPY:
    writeResult(Op.Output, A);
    return StepResult::Continue;

  case NdOp::INT_ZEXT:
    writeResult(Op.Output, A);
    return StepResult::Continue;

  case NdOp::INT_SEXT:
    // Sign extension has to happen before the fit, which zero-extends.
    writeResult(Op.Output,
                OutWidth > Ctx.width(A) ? Ctx.mkSExt(A, OutWidth) : A);
    return StepResult::Continue;

  // Complement under both of its spellings; the arithmetic negation is the
  // separate one.
  case NdOp::INT_NEGATE:
  case NdOp::INT_NOT:
    writeResult(Op.Output, Ctx.mkNot(A));
    return StepResult::Continue;

  case NdOp::INT_NEG2:
    writeResult(Op.Output, Ctx.mkNeg(A));
    return StepResult::Continue;

  case NdOp::SUBBYTES: {
    // The second operand counts bytes from the low end of the value.
    std::optional<llvm::APInt> Offset =
        Op.NumInputs >= 2 ? Ctx.asConst(read(Op.Inputs[1]))
                          : std::optional<llvm::APInt>(llvm::APInt(64, 0));
    if (!Offset || Offset->getActiveBits() > 64 || OutWidth == 0)
      return unmodelled(Op);
    const uint64_t ByteOffset = Offset->getZExtValue();
    if (ByteOffset > Ctx.width(A) / kByteBits)
      return unmodelled(Op);
    const uint64_t Low = ByteOffset * kByteBits;
    if (Low > Ctx.width(A) || OutWidth > Ctx.width(A) - Low)
      return unmodelled(Op);
    writeResult(Op.Output, Ctx.mkExtract(A, uint32_t(Low), OutWidth));
    return StepResult::Continue;
  }

  case NdOp::CONCAT: {
    if (Op.NumInputs < 2)
      return unmodelled(Op);
    writeResult(Op.Output, Ctx.mkConcat(A, read(Op.Inputs[1])));
    return StepResult::Continue;
  }

  default:
    return unmodelled(Op);
  }
}

StepResult SymExec::stepBoolean(const LowOp &Op) {
  if (Op.NumInputs < 1)
    return unmodelled(Op);

  // A lifted boolean is a whole byte holding zero or one, so it has to be
  // reduced to a bit before the logic and widened again afterwards.
  auto asBit = [&](const NdVar &V) {
    SymRef Value = read(V);
    return Ctx.width(Value) == 1
               ? Value
               : Ctx.mkNe(Value, Ctx.mkZero(Ctx.width(Value)));
  };

  SymRef Bit;
  if (Op.Opcode == NdOp::BOOL_NOT) {
    Bit = Ctx.mkNot(asBit(Op.Inputs[0]));
  } else {
    if (Op.NumInputs < 2)
      return unmodelled(Op);
    SymRef A = asBit(Op.Inputs[0]);
    SymRef B = asBit(Op.Inputs[1]);
    switch (Op.Opcode) {
    case NdOp::BOOL_AND:
      Bit = Ctx.mkAnd(A, B);
      break;
    case NdOp::BOOL_OR:
      Bit = Ctx.mkOr(A, B);
      break;
    case NdOp::BOOL_XOR:
      Bit = Ctx.mkXor(A, B);
      break;
    default:
      return unmodelled(Op);
    }
  }

  writeResult(Op.Output, Bit);
  return StepResult::Continue;
}

//===----------------------------------------------------------------------===//
// Bit counts and bit fields
//===----------------------------------------------------------------------===//

// Counting bits, and moving a field of them.
//
// Every one of these is exact, and each is here because the concrete emulator
// computes it.  An opcode one engine executes and the other replaces with an
// unknown is a disagreement that shows up as a lost jump table or as a value
// that looks independent of something it depends on, somewhere else entirely.
//
// The counts are taken at the width the operand declares, in keeping with the
// rest of this file: the leading zeros of a byte are not the leading zeros of
// the machine word it was loaded into.
StepResult SymExec::stepBits(const LowOp &Op) {
  if (Op.NumInputs < 1)
    return unmodelled(Op);

  SymRef A = read(Op.Inputs[0]);
  const uint32_t Width = Ctx.width(A);

  // A constant operand of a bit-field operation, as a bit index.  A variable
  // one has no model: the field it names is a different mask per assignment
  // and the expression language has no way to write that.
  auto bitIndex = [&](const NdVar &V) -> std::optional<uint32_t> {
    std::optional<llvm::APInt> Value = Ctx.asConst(read(V));
    if (!Value || Value->getActiveBits() > 32)
      return std::nullopt;
    return uint32_t(Value->getZExtValue());
  };

  switch (Op.Opcode) {
  case NdOp::POPCOUNT: {
    if (Width > kMaxBitCountWidth)
      return unmodelled(Op);
    // A bit at a time, because the expression language has no population
    // count and nothing shorter is exact.  The sum cannot overflow the width
    // it is taken at: a W-bit word holds at most W ones, and W < 2^W.
    llvm::SmallVector<SymRef, 64> Bits;
    Bits.reserve(Width);
    for (uint32_t I = 0; I < Width; ++I)
      Bits.push_back(Ctx.mkZExtOrTrunc(Ctx.mkExtract(A, I, 1), Width));
    writeResult(Op.Output, Ctx.mkAdd(Bits));
    return StepResult::Continue;
  }

  case NdOp::LZCOUNT: {
    if (Width > kMaxBitCountWidth)
      return unmodelled(Op);
    // Built from the bottom bit up so that the test of the most significant
    // one ends up outermost, which is what makes the highest set bit the one
    // that decides.  A word with no bits set falls through to the width.
    SymRef Result = Ctx.mkConst(Width, Width);
    for (uint32_t I = 0; I < Width; ++I)
      Result = Ctx.mkIte(Ctx.mkExtract(A, I, 1),
                         Ctx.mkConst(Width, Width - 1 - I), Result);
    writeResult(Op.Output, Result);
    return StepResult::Continue;
  }

  case NdOp::INSERT: {
    // Base, value, position and length, the last two in bits.
    if (Op.NumInputs < 4)
      return unmodelled(Op);
    std::optional<uint32_t> Low = bitIndex(Op.Inputs[2]);
    std::optional<uint32_t> Bits = bitIndex(Op.Inputs[3]);
    if (!Low || !Bits || *Bits == 0 || *Low >= Width || *Bits > Width - *Low)
      return unmodelled(Op);
    const llvm::APInt Mask = llvm::APInt::getBitsSet(Width, *Low, *Low + *Bits);
    SymRef Field = fit(read(Op.Inputs[1]), Width);
    writeResult(Op.Output,
                Ctx.mkOr(Ctx.mkAnd(A, Ctx.mkConst(~Mask)),
                         Ctx.mkAnd(Ctx.mkShl(Field, Ctx.mkConst(Width, *Low)),
                                   Ctx.mkConst(Mask))));
    return StepResult::Continue;
  }

  case NdOp::EXTRACT: {
    // Base, position and length, the last two in bits.
    if (Op.NumInputs < 3)
      return unmodelled(Op);
    std::optional<uint32_t> Low = bitIndex(Op.Inputs[1]);
    std::optional<uint32_t> Bits = bitIndex(Op.Inputs[2]);
    if (!Low || !Bits || *Bits == 0 || *Low >= Width || *Bits > Width - *Low)
      return unmodelled(Op);
    writeResult(Op.Output, Ctx.mkExtract(A, *Low, *Bits));
    return StepResult::Continue;
  }

  default:
    return unmodelled(Op);
  }
}

StepResult SymExec::stepMemory(const LowOp &Op,
                               SymStorePreservation *Preservation,
                               SymConditionalStore *Conditional) {
  const auto Refuse = [&]() {
    if (Op.Opcode == NdOp::STORE || isAtomicWrite(Op.Opcode)) {
      State.clobberMemory();
      ++MemoryHavocs;
    }
    return unmodelled(Op);
  };
  if ((Preservation || Conditional) && isAtomicWrite(Op.Opcode) &&
      !supportsAtomicWritePreservation(Op))
    return Refuse();
  const LowMemoryOperandView Memory = lowMemoryOperands(Op);
  const SymRef Addr = memoryAddressValue(Op);
  if (!Addr)
    return Refuse();
  if (const auto Absolute = Ctx.asConst(Addr);
      Absolute && Absolute->getBitWidth() <= 64 &&
      Absolute->getZExtValue() > UINT64_MAX - (Memory.AccessSize - 1))
    return Refuse();
  const auto Commit = [&](SymRef Value) {
    bool Absolute;
    if (Conditional) {
      Absolute = State.storeConditional(Addr, Value, *Conditional);
      if (Conditional->Status != SymConditionalStoreStatus::Applied)
        return Refuse();
    } else {
      Absolute = Preservation ? State.store(Addr, Value, *Preservation)
                              : State.store(Addr, Value);
      if (Preservation &&
          Preservation->Status != SymStorePreservationStatus::Applied)
        return Refuse();
    }
    if (!Absolute) {
      ++Unmodelled;
      ++MemoryHavocs;
    }
    return StepResult::Continue;
  };
  if (Op.Opcode == NdOp::LOAD) {
    const auto Value = State.load(Addr, Memory.AccessSize);
    if (!Value)
      return Refuse();
    writeResult(Op.Output, Value);
    return StepResult::Continue;
  }

  if (Op.Opcode == NdOp::ATOMIC_XCHG || Op.Opcode == NdOp::ATOMIC_ADD) {
    SymRef Old = State.load(Addr, Memory.AccessSize);
    if (!Old)
      return Refuse();
    SymRef Value = fit(read(*Memory.StoredValue), widthOf(Op.Output));
    SymRef NewValue =
        Op.Opcode == NdOp::ATOMIC_ADD ? Ctx.mkAdd(Old, Value) : Value;
    if (!Preservation && !Conditional)
      writeResult(Op.Output, Old);
    const auto Committed = Commit(NewValue);
    if (Committed != StepResult::Continue)
      return Committed;
    if (Preservation || Conditional)
      writeResult(Op.Output, Old);
    return StepResult::Continue;
  }

  if (Op.Opcode == NdOp::ATOMIC_CMPXCHG) {
    SymRef Old = State.load(Addr, Memory.AccessSize);
    if (!Old)
      return Refuse();
    SymRef Expected = fit(read(*Memory.ExpectedValue), widthOf(Op.Output));
    SymRef Desired = fit(read(*Memory.StoredValue), widthOf(Op.Output));
    // Preserve the legacy no-descriptor ordering, including RAM outputs.
    // Explicit write policies accept only scalar outputs and publish Old
    // after the write phase has actually succeeded.
    if (!Preservation && !Conditional)
      writeResult(Op.Output, Old);
    const auto Committed =
        Commit(Ctx.mkIte(Ctx.mkEq(Old, Expected), Desired, Old));
    if (Committed != StepResult::Continue)
      return Committed;
    if (Preservation || Conditional)
      writeResult(Op.Output, Old);
    return StepResult::Continue;
  }

  return Commit(read(*Memory.StoredValue));
}

StepResult SymExec::stepControl(const LowOp &Op,
                                const SymCallEffect *CallEffect) {
  switch (Op.Opcode) {
  case NdOp::BRANCH:
    Target = Op.NumInputs >= 1 ? read(Op.Inputs[0]) : Ctx.mkConst(64, 0);
    return StepResult::Branch;

  case NdOp::COND_BR: {
    if (Op.NumInputs < 2)
      return unmodelled(Op);
    // The destination comes first and the predicate second, as the lifters
    // emit them.
    Target = read(Op.Inputs[0]);
    SymRef Predicate = read(Op.Inputs[1]);
    Condition = Ctx.width(Predicate) == 1
                    ? Predicate
                    : Ctx.mkNe(Predicate, Ctx.mkZero(Ctx.width(Predicate)));
    return StepResult::CondBranch;
  }

  case NdOp::INDIR_BR:
    Target = Op.NumInputs >= 1 ? read(Op.Inputs[0]) : Ctx.mkConst(64, 0);
    return StepResult::IndirectBranch;

  case NdOp::RETURN:
    // Some ISAs make the architectural return address explicit in LowIR.
    // SymExplore uses instruction-boundary metadata to distinguish that
    // control target from a legacy RETURN operand carrying a semantic value.
    Target = Op.NumInputs >= 1 ? read(Op.Inputs[0]) : SymRef();
    return StepResult::Return;

  case NdOp::CALL:
  case NdOp::INDIR_CALL:
    if (CallEffect)
      for (const SymCallRegisterWrite &Write : CallEffect->Registers)
        if (!State.canWriteRegister(Write.Offset, Write.Value))
          throw std::invalid_argument(
              "call summary contains invalid register write");
    // Stepping over a call rather than stopping at one: what it returns is an
    // unknown, and what it leaves in the registers it is allowed to overwrite
    // is another.  Both are named so execution can continue, but without a
    // callee summary this operation is necessarily an approximation.
    // Capture the call destination before caller-saved registers are clobbered.
    // SymExplore consumes it when instruction-boundary metadata proves that the
    // call does not return.
    if (CallEffect && CallEffect->MemoryAfter) {
      // Read the target from the pre-call state, including memory operands.
      // Admission is transactional: even an untouched target input must not
      // materialize in the original state if the memory contract is invalid.
      SymState Draft(State);
      SymExec Reader(Ctx, Draft);
      SymRef Destination =
          Op.NumInputs >= 1 ? Reader.operandValue(Op.Inputs[0]) : SymRef();
      if (!Draft.restoreMemoryKnowledge(*CallEffect->MemoryAfter))
        throw std::invalid_argument(
            "call summary contains invalid memory knowledge");
      State = std::move(Draft);
      Target = Destination;
    } else {
      Target = Op.NumInputs >= 1 ? read(Op.Inputs[0]) : SymRef();
    }
    State.clearX64MemoryAddressSpaceBases();
    if (CallEffect) {
      State.clobberRegistersExcept(CallPreserved);
      if (!CallEffect->MemoryAfter &&
          CallEffect->Memory == SymCallMemoryEffect::Havoc)
        State.clobberMemory();
      for (const SymCallMemoryWrite &Write : CallEffect->Writes) {
        if (!Write.Address.isValid() || !Write.Value.isValid()) {
          // A malformed explicit write cannot preserve memory soundly.  Keep
          // the summary fail-closed even when its provider made a mistake.
          State.clobberMemory();
          ++MemoryHavocs;
          continue;
        }
        if (!State.store(Write.Address, Write.Value))
          ++MemoryHavocs;
      }
      for (const SymCallRegisterWrite &Write : CallEffect->Registers)
        State.write(SymSpace::Register, Write.Offset, Write.Value);
      if (widthOf(Op.Output) != 0) {
        SymRef Return = CallEffect->ReturnValue;
        if (!Return.isValid())
          Return = State.freshInput("call_summary", widthOf(Op.Output));
        writeResult(Op.Output, Return);
      }
      for (SymRef Constraint : CallEffect->Constraints)
        if (Constraint.isValid())
          assume(Constraint);
      return StepResult::Continue;
    }
    ++Unmodelled;
    ++CallHavocs;
    State.clobberRegistersExcept(CallPreserved);
    // With no function summary, the callee may write through any pointer it
    // can reach.  Keeping a pre-call memory byte would turn that uncertainty
    // into a false constant.
    State.clobberMemory();
    if (widthOf(Op.Output) != 0)
      writeResult(Op.Output, State.freshInput("call", widthOf(Op.Output)));
    return StepResult::Continue;

  default:
    return unmodelled(Op);
  }
}

//===----------------------------------------------------------------------===//
// Dispatch
//===----------------------------------------------------------------------===//

StepResult SymExec::step(const LowOp &Op) { return step(Op, nullptr); }

StepResult SymExec::step(const LowOp &Op, const SymCallEffect *CallEffect) {
  return step(Op, CallEffect, nullptr);
}

StepResult SymExec::step(const LowOp &Op, const SymCallEffect *CallEffect,
                         SymStorePreservation *Preservation) {
  return stepImpl(Op, CallEffect, Preservation, nullptr);
}

StepResult SymExec::stepWithConditionalStore(const LowOp &Op,
                                             SymConditionalStore &Control,
                                             const SymCallEffect *CallEffect) {
  Control.Status = SymConditionalStoreStatus::Invalid;
  Control.WorkUsed = 0;
  return stepImpl(Op, CallEffect, nullptr, &Control);
}

StepResult SymExec::stepImpl(const LowOp &Op, const SymCallEffect *CallEffect,
                             SymStorePreservation *Preservation,
                             SymConditionalStore *Conditional) {
  // Unmodelled is a continuing value abstraction. A required environment
  // cannot be replaced by an unknown result, especially without an output.
  if (isLowRawEnvironmentRequirement(Op))
    throw std::invalid_argument(
        "raw environment requirement needs a receipt-aware symbolic consumer");
  if (Preservation) {
    Preservation->Status = SymStorePreservationStatus::Invalid;
    Preservation->WorkUsed = 0;
    if (Op.Opcode != NdOp::STORE && !isAtomicWrite(Op.Opcode))
      return step(Op, CallEffect);
  }
  if (Conditional && Op.Opcode != NdOp::STORE && !isAtomicWrite(Op.Opcode))
    return step(Op, CallEffect);
  switch (Op.Opcode) {
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_MULT:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
  case NdOp::INT_LEFT:
  case NdOp::INT_RIGHT:
  case NdOp::INT_ASHR:
  case NdOp::INT_DIV:
  case NdOp::INT_SDIV:
  case NdOp::INT_REM:
  case NdOp::INT_SREM:
    return stepBinary(Op);

  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESS:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::INT_CARRY:
  case NdOp::INT_SOVF:
  case NdOp::INT_SBOR:
    return stepCompare(Op);

  case NdOp::COPY:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
  case NdOp::INT_NEGATE:
  case NdOp::INT_NOT:
  case NdOp::INT_NEG2:
  case NdOp::SUBBYTES:
  case NdOp::CONCAT:
    return stepUnary(Op);

  case NdOp::BOOL_NOT:
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
    return stepBoolean(Op);

  case NdOp::POPCOUNT:
  case NdOp::LZCOUNT:
  case NdOp::INSERT:
  case NdOp::EXTRACT:
    return stepBits(Op);

  case NdOp::SELECT: {
    if (Op.NumInputs < 3)
      return unmodelled(Op);
    SymRef Cond = read(Op.Inputs[0]);
    SymRef IfTrue = read(Op.Inputs[1]);
    SymRef IfFalse = fit(read(Op.Inputs[2]), Ctx.width(IfTrue));
    SymRef Bit = Ctx.width(Cond) == 1
                     ? Cond
                     : Ctx.mkNe(Cond, Ctx.mkZero(Ctx.width(Cond)));
    writeResult(Op.Output, Ctx.mkIte(Bit, IfTrue, IfFalse));
    return StepResult::Continue;
  }

  case NdOp::LOAD:
  case NdOp::STORE:
  case NdOp::ATOMIC_XCHG:
  case NdOp::ATOMIC_ADD:
  case NdOp::ATOMIC_CMPXCHG:
    return stepMemory(Op, Preservation, Conditional);

  case NdOp::BRANCH:
  case NdOp::COND_BR:
  case NdOp::INDIR_BR:
  case NdOp::RETURN:
  case NdOp::CALL:
  case NdOp::INDIR_CALL:
    return stepControl(Op, CallEffect);

  case NdOp::NOP:
    return StepResult::Continue;

  case NdOp::INTRINSIC:
    if (auto Value = evaluateIntegerIntrinsic(
            Ctx, Op, [&](const NdVar &Input) { return read(Input); })) {
      writeResult(Op.Output, *Value);
      return StepResult::Continue;
    }
    return stepX87Intrinsic(Op);

  default:
    // Everything floating-point, and whatever a lifter routed through an
    // intrinsic — a vector operation, a system instruction.  Each has a value
    // this expression language cannot express at all, rather than one it would
    // merely be expensive to write out, so each gets a name instead.  The
    // concrete emulator reaches the same position from the other side: it
    // invalidates the destination of an intrinsic rather than folding a stale
    // value into it.
    return unmodelled(Op);
  }
}

size_t SymExec::run(llvm::ArrayRef<LowOp> Ops) {
  size_t Executed = 0;
  for (const LowOp &Op : Ops) {
    ++Executed;
    StepResult Result = step(Op);
    if (Result != StepResult::Continue && Result != StepResult::Unmodelled)
      break;
  }
  return Executed;
}

//===----------------------------------------------------------------------===//
// Path conditions
//===----------------------------------------------------------------------===//

void SymExec::assume(SymRef Cond) {
  assert(Ctx.width(Cond) == 1 && "a path condition is one bit");
  Constraints.push_back(Cond);
}

void SymExec::assumeBranch(bool Taken) {
  if (!Condition.isValid())
    return;
  assume(Taken ? Condition : Ctx.mkNot(Condition));
}

SymRef SymExec::pathPredicate() const {
  if (Constraints.empty())
    return Ctx.mkTrue();
  return Ctx.mkAnd(Constraints);
}

} // namespace neverd::symbolic

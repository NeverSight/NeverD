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
/// An operation the engine does not model writes a fresh input to its
/// destination rather than stopping.  A call, a square root, a population
/// count: the value becomes an unknown with a name, execution continues, and
/// whatever comes out further along remains exactly true — it simply mentions
/// the unknown.
///
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymExec.h"

#include "llvm/ADT/SmallVector.h"

#include <cassert>

namespace neverd::symbolic {

namespace {

constexpr uint32_t kByteBits = 8;

/// Width of an operand in bits, or zero when it does not declare one.
uint32_t widthOf(const NdVar &V) { return uint32_t(V.Size) * kByteBits; }

SymSpace spaceOf(const NdVar &V) {
  return V.isTemp() ? SymSpace::Temporary : SymSpace::Register;
}

} // namespace

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
  if (widthOf(Op.Output) != 0)
    writeResult(Op.Output, State.freshInput("undef", widthOf(Op.Output)));
  return StepResult::Unmodelled;
}

//===----------------------------------------------------------------------===//
// Arithmetic and bitwise
//===----------------------------------------------------------------------===//

StepResult SymExec::stepBinary(const LowOp &Op) {
  if (Op.NumInputs < 2)
    return unmodelled(Op);

  SymRef A = read(Op.Inputs[0]);
  const uint32_t Width = Ctx.width(A);
  // The second operand is brought to the first's width rather than the other
  // way round.  For a shift that is exactly right — the amount is often
  // narrower than the value and means the same number either way — and for the
  // rest the two already agree.
  SymRef B = fit(read(Op.Inputs[1]), Width);

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

StepResult SymExec::stepMemory(const LowOp &Op) {
  if (Op.Opcode == NdOp::LOAD) {
    if (Op.NumInputs < 1 || widthOf(Op.Output) == 0)
      return unmodelled(Op);
    // A load names its address in the last operand it has: a leading space
    // identifier is present in some lifts and absent in others.
    const NdVar &AddrVar = Op.Inputs[Op.NumInputs >= 2 ? 1 : 0];
    writeResult(Op.Output, State.load(read(AddrVar), Op.Output.Size));
    return StepResult::Continue;
  }

  if (Op.NumInputs < 2)
    return unmodelled(Op);
  const bool HasSpace = Op.NumInputs >= 3;
  SymRef Addr = read(Op.Inputs[HasSpace ? 1 : 0]);
  SymRef Value = read(Op.Inputs[HasSpace ? 2 : 1]);
  State.store(Addr, Value);
  return StepResult::Continue;
}

StepResult SymExec::stepControl(const LowOp &Op) {
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
    return StepResult::Return;

  case NdOp::CALL:
  case NdOp::INDIR_CALL:
    // Stepping over a call rather than stopping at one: what it returns is an
    // unknown, and what it leaves in the registers it is allowed to overwrite
    // is another.  Both are named, so the code after the call is still
    // executed exactly.
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

StepResult SymExec::step(const LowOp &Op) {
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
    return stepMemory(Op);

  case NdOp::BRANCH:
  case NdOp::COND_BR:
  case NdOp::INDIR_BR:
  case NdOp::RETURN:
  case NdOp::CALL:
  case NdOp::INDIR_CALL:
    return stepControl(Op);

  case NdOp::NOP:
    return StepResult::Continue;

  default:
    // Everything floating-point, the bit counts, the insert and extract forms,
    // and whatever a lifter routed through an intrinsic.  Each has a value the
    // engine cannot express, so each gets one it can name.
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

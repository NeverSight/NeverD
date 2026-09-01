//===- SymConcrete.cpp - Exact concrete execution through SymExec ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymConcrete.h"

#include <limits>
#include <set>

namespace neverd::symbolic {

struct SymExecConcreteShadow::Impl {
  std::unique_ptr<SymContext> Context;
  std::unique_ptr<SymState> State;
  std::unique_ptr<SymExec> Executor;
  std::set<uint64_t> SeededBytes;
  bool Trusted = false;
  bool Started = false;

  void rebuild(llvm::endianness ByteOrder) {
    Executor.reset();
    State.reset();
    Context = std::make_unique<SymContext>();
    State = std::make_unique<SymState>(*Context, ByteOrder);
    Executor = std::make_unique<SymExec>(*Context, *State);
    SeededBytes.clear();
    Trusted = true;
    Started = false;
  }

  std::optional<uint64_t> concreteValue(const NdVar &V) {
    SymRef Expression = Executor->operandValue(V);
    if (!Expression.isValid() || Context->width(Expression) > 64)
      return std::nullopt;
    std::optional<llvm::APInt> Constant = Context->asConst(Expression);
    if (!Constant || Constant->getActiveBits() > 64)
      return std::nullopt;
    return Constant->getZExtValue();
  }

  bool fail() {
    Trusted = false;
    return false;
  }
};

SymExecConcreteShadow::SymExecConcreteShadow() : P(std::make_unique<Impl>()) {}

SymExecConcreteShadow::~SymExecConcreteShadow() = default;

bool SymExecConcreteShadow::reset(llvm::endianness ByteOrder) {
  P->rebuild(ByteOrder);
  return true;
}

bool SymExecConcreteShadow::setRegister(uint64_t Offset, uint16_t Bytes,
                                        uint64_t Value) {
  if (!P->Trusted || P->Started || Bytes == 0 || Bytes > sizeof(uint64_t) ||
      (Bytes < sizeof(uint64_t) && Value >> (unsigned(Bytes) * 8) != 0) ||
      Offset > std::numeric_limits<uint64_t>::max() - (Bytes - 1))
    return P->fail();

  for (uint16_t I = 0; I < Bytes; ++I)
    if (P->SeededBytes.count(Offset + I) != 0)
      return P->fail();
  for (uint16_t I = 0; I < Bytes; ++I)
    P->SeededBytes.insert(Offset + I);

  P->State->write(SymSpace::Register, Offset,
                  P->Context->mkConst(uint32_t(Bytes) * 8, Value));
  return true;
}

bool SymExecConcreteShadow::step(const LowOp &Op) {
  if (!P->Trusted)
    return false;
  P->Started = true;

  // A concrete run must know every value the operation consumes, even when
  // symbolic simplification could prove that one of them does not affect the
  // output.  This is what keeps a missing seed from becoming an implicit zero.
  for (uint8_t I = 0; I < Op.NumInputs; ++I)
    if (!P->concreteValue(Op.Inputs[I]))
      return P->fail();

  const StepResult Result = P->Executor->step(Op);
  if (Result != StepResult::Continue || P->Executor->unmodelledCount() != 0 ||
      P->Executor->opaqueOperationCount() != 0 ||
      P->Executor->callHavocCount() != 0 ||
      P->Executor->memoryHavocCount() != 0)
    return P->fail();

  if (Op.Output.Size != 0 && !P->concreteValue(Op.Output))
    return P->fail();
  return true;
}

std::optional<uint64_t> SymExecConcreteShadow::value(const NdVar &V) {
  if (!P->Trusted)
    return std::nullopt;
  P->Started = true;
  std::optional<uint64_t> Result = P->concreteValue(V);
  if (!Result)
    P->Trusted = false;
  return Result;
}

} // namespace neverd::symbolic

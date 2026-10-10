//===- LLVMInterpreterModelInternal.h - Scalar model builder ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LLVM_INTERPRETER_MODEL_INTERNAL_H
#define NEVERD_LLVM_INTERPRETER_MODEL_INTERNAL_H

#include "neverd/analysis/LLVMInterpreterModel.h"
#include "neverd/analysis/LLVMScalarFunctionModel.h"

#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/ModRef.h"

#include <map>
#include <set>
#include <string>

namespace neverd::analysis::llvm_model {

struct Failure {
  std::string Message;
  bool Budget = false;
};
[[noreturn]] inline void fail(const llvm::Twine &Message) {
  throw Failure{Message.str()};
}
inline NdVar rvar(uint64_t Offset, unsigned Bytes = 8) {
  return NdVar::reg(Offset, Bytes);
}
inline NdVar num(uint64_t Value, unsigned Bytes = 8) {
  return NdVar::scalar(Value, Bytes);
}
inline LowOp op(NdOp Code, NdVar Output, std::initializer_list<NdVar> Inputs) {
  LowOp O;
  O.Opcode = Code;
  O.Output = Output;
  for (auto V : Inputs)
    O.addInput(V);
  return O;
}

/// One recipe for a violated integer range, shared by the LowIR importer and
/// LLVM projections that must materialize a disappearing return attribute.
struct RangeObligation {
  enum Kind { None, Always, Bounds } Test = None;
  uint64_t Lower = 0, Upper = 0;
  bool Wrapped = false;
};
inline RangeObligation rangeObligation(const llvm::ConstantRange &Range) {
  if (Range.isFullSet())
    return {};
  if (Range.isEmptySet())
    return {RangeObligation::Always};
  return {RangeObligation::Bounds, Range.getLower().getZExtValue(),
          Range.getUpper().getZExtValue(),
          Range.getLower().uge(Range.getUpper())};
}

class Builder {
  const llvm::Function &F;
  const LLVMInterpreterModelLimits &Limits;
  std::vector<LLVMScalarArgument> *ScalarArguments;
  InterpreterMachineStateModel Result;
  std::map<const llvm::Value *, NdVar> Values;
  std::map<const llvm::BasicBlock *, int> Blocks;
  std::map<std::pair<const llvm::BasicBlock *, const llvm::BasicBlock *>, int>
      Edges;
  std::vector<std::pair<const llvm::BasicBlock *, const llvm::BasicBlock *>>
      EdgeOrder;
  std::map<const llvm::SwitchInst *, std::vector<int>> SwitchBlocks;
  std::map<const llvm::Value *, int64_t> StateOffsets;
  std::map<const llvm::Value *, std::pair<NdVar, NdVar>> Aggregates;
  uint64_t NextRegister = uint64_t{1} << 40;
  uint64_t NextTemporary = 0;
  uint64_t Work = 0, InputItems = 0, Operations = 0;
  const unsigned StateBytes;
  static constexpr uint64_t DefinednessOffset =
      LLVMInterpreterDefinednessOffset;

  void charge(uint64_t &Counter, uint64_t Amount, uint64_t Limit);
  void work(uint64_t Amount = 1) { charge(Work, Amount, Limits.MaxWork); }
  void input(uint64_t Amount = 1) {
    charge(InputItems, Amount, Limits.MaxInputItems);
    work(Amount);
  }
  NdVar local(unsigned Bytes);
  NdVar fresh(unsigned Bytes);
  int block();
  void emit(LowBlock &Block, LowOp Operation);
  void preflight();
  void validateContract();
  void validateInitialization();
  void pointerProjections();
  bool blockLocal(const llvm::Instruction &I);
  NdVar stateSlot(const llvm::Value *Pointer, unsigned Bytes, uint64_t Align);
  void requireGuestAlignment(LowBlock &Out, NdVar Address, uint64_t Align);
  void requireEqual(LowBlock &Out, NdVar A, NdVar B);
  void requireRange(LowBlock &Out, NdVar Value,
                    const llvm::ConstantRange &Range);
  unsigned bytes(llvm::Type *Type);
  NdVar value(const llvm::Value *Value);
  int target(const llvm::BasicBlock *From, const llvm::BasicBlock *To);
  va_t address(int Id) { return Result.Function.Blocks.at(Id).StartAddr; }
  void createGraph();
  void emitEdges();
  bool emitMemory(LowBlock &Out, const llvm::Instruction &I);
  bool emitScalar(LowBlock &Out, const llvm::Instruction &I);
  bool emitControl(LowBlock &Out, const llvm::Instruction &I);
  void seal();

public:
  Builder(const llvm::Function &Function,
          const LLVMInterpreterModelLimits &Limits, unsigned StateBytes,
          std::vector<LLVMScalarArgument> *ScalarArguments = nullptr)
      : F(Function), Limits(Limits), ScalarArguments(ScalarArguments),
        StateBytes(StateBytes) {}
  InterpreterMachineStateModel build();
  /// Valid only after successful build(). Identities describe this exact input
  /// function; consumers cannot use them as guest-memory or alias facts.
  const std::map<const llvm::Value *, int64_t> &statePointerOffsets() const {
    return StateOffsets;
  }
};
} // namespace neverd::analysis::llvm_model
#endif

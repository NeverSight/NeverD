//===- PortableX87Validation.cpp - Portable CFG and SSA admission ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prove explicit scalar definitions and ordinary direct-control-flow edges.
//===----------------------------------------------------------------------===//

#include "PortableX87Validation.h"

#include "neverd/ir/med/MedScalarShapes.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Errc.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <vector>

namespace neverd::portable_x87 {
namespace {
llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, Message);
}
using Key = std::tuple<MedVar::VarKind, int, int>;
Key key(const MedVar &Value) { return {Value.Kind, Value.Id, Value.SSAVer}; }
struct Definition {
  int Block;
  int Position;
  uint16_t Bytes;
};
bool scalar(const MedVar &Value) {
  return Value.Size != 0 && Value.Size <= sizeof(uint64_t) &&
         (Value.TheArch == Arch::X64 || Value.TheArch == Arch::Unknown) &&
         (Value.Kind == MedVar::Temp || Value.Kind == MedVar::Param ||
          Value.Kind == MedVar::Const);
}
bool integerShape(const MedOp &Op) {
  const int Count = medScalarInputCount(Op.Opcode);
  if (Count >= 0 &&
      (Op.NumInputs != Count || !medScalarInputWidthsCompatible(Op, 8) ||
       !medScalarByteProjectionIsValid(Op)))
    return false;
  const auto Unary = [&] {
    return Op.Output.Size && Op.Output.Size == Op.Inputs[0].Size;
  };
  const auto Binary = [&] {
    return Op.Output.Size && Op.Output.Size == Op.Inputs[0].Size &&
           Op.Output.Size == Op.Inputs[1].Size;
  };
  switch (Op.Opcode) {
  case NdOp::NOP:
    return Op.NumInputs == 0 && Op.Output.Size == 0;
  case NdOp::COPY:
  case NdOp::INT_NEGATE:
  case NdOp::INT_NOT:
    return Unary();
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_MULT:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
    return Binary();
  case NdOp::INT_LEFT:
  case NdOp::INT_RIGHT:
  case NdOp::INT_ASHR:
    return Op.NumInputs == 2 && Op.Output.Size &&
           Op.Output.Size == Op.Inputs[0].Size && Op.Inputs[1].isConst() &&
           Op.Inputs[1].ConstVal < Op.Output.Size * 8U;
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    return Op.NumInputs == 1 && Op.Inputs[0].Size < Op.Output.Size;
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESS:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::INT_CARRY:
  case NdOp::INT_SOVF:
  case NdOp::INT_SBOR:
    return Op.NumInputs == 2 && Op.Output.Size == 1 &&
           Op.Inputs[0].Size == Op.Inputs[1].Size;
  case NdOp::BOOL_NOT:
    return Unary() && Op.Output.Size == 1;
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
    return Binary() && Op.Output.Size == 1;
  case NdOp::CONCAT:
  case NdOp::SUBBYTES:
    return Op.Output.Size != 0;
  case NdOp::SELECT:
    return Op.NumInputs == 3 && Op.Inputs[0].Size == 1 && Op.Output.Size &&
           Op.Inputs[1].Size == Op.Output.Size &&
           Op.Inputs[2].Size == Op.Output.Size;
  case NdOp::BRANCH:
    return Op.NumInputs == 1 && Op.Output.Size == 0 && Op.Inputs[0].isConst() &&
           Op.Inputs[0].Size == 8;
  case NdOp::COND_BR:
    return Op.NumInputs == 2 && Op.Output.Size == 0 && Op.Inputs[0].isConst() &&
           Op.Inputs[0].Size == 8;
  case NdOp::RETURN:
    return Op.NumInputs == 1 && Op.Output.Size == 0;
  case NdOp::STORE:
    return Op.NumInputs == 2 && Op.Output.Size == 0 && Op.Inputs[0].Size == 8 &&
           Op.Inputs[1].Size == 8;
  case NdOp::INTRINSIC:
    // The context and portable helper owners validate their full shapes.
    return true;
  default:
    return false;
  }
}
} // namespace

bool isIntegerOperation(NdOp Opcode) {
  switch (Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_MULT:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
  case NdOp::INT_LEFT:
  case NdOp::INT_RIGHT:
  case NdOp::INT_ASHR:
  case NdOp::INT_NEGATE:
  case NdOp::INT_NOT:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESS:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::INT_CARRY:
  case NdOp::INT_SOVF:
  case NdOp::INT_SBOR:
  case NdOp::BOOL_NOT:
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
  case NdOp::CONCAT:
  case NdOp::SUBBYTES:
  case NdOp::SELECT:
  case NdOp::NOP:
  case NdOp::BRANCH:
  case NdOp::COND_BR:
  case NdOp::RETURN:
    return true;
  default:
    return false;
  }
}

llvm::Error validateControlAndValues(const MedFunc &Function) {
  if (Function.Blocks.empty() || !Function.Blocks.front().Preds.empty())
    return invalid("portable x87 requires an ordinary, predecessor-free entry");
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Function> Graph(llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
      llvm::GlobalValue::PrivateLinkage));
  std::vector<llvm::BasicBlock *> Blocks;
  for (const auto &Block : Function.Blocks) {
    if (Block.Id != static_cast<int>(Blocks.size()) ||
        !Block.ExceptionalSuccs.empty() || !Block.ExceptionalPreds.empty() ||
        Block.Succs.size() > 2 || Block.Ops.empty())
      return invalid("portable x87 has an invalid block or exceptional edge");
    Blocks.push_back(llvm::BasicBlock::Create(Context, "", Graph.get()));
  }
  std::vector<std::set<int>> Predecessors(Blocks.size());
  for (const auto &Block : Function.Blocks) {
    std::set<int> Unique;
    for (int Successor : Block.Succs) {
      if (Successor < 0 || static_cast<size_t>(Successor) >= Blocks.size() ||
          !Unique.insert(Successor).second)
        return invalid("portable x87 has a malformed successor");
      Predecessors[Successor].insert(Block.Id);
    }
    const auto Terminal = Block.Ops.back().Opcode;
    if (Block.Succs.empty()) {
      if (Terminal != NdOp::RETURN)
        return invalid("portable x87 leaf has no explicit return");
      llvm::ReturnInst::Create(Context, Blocks[Block.Id]);
    } else if (Block.Succs.size() == 1) {
      if (Terminal == NdOp::RETURN || Terminal == NdOp::COND_BR)
        return invalid("portable x87 terminator disagrees with its edges");
      llvm::UncondBrInst::Create(Blocks[Block.Succs[0]], Blocks[Block.Id]);
    } else {
      if (Terminal != NdOp::COND_BR)
        return invalid("portable x87 branch has no explicit condition");
      llvm::CondBrInst::Create(llvm::ConstantInt::getFalse(Context),
                               Blocks[Block.Succs[0]], Blocks[Block.Succs[1]],
                               Blocks[Block.Id]);
    }
  }
  llvm::DominatorTree Dominators(*Graph);
  std::map<Key, Definition> Definitions;
  for (const auto &Parameter : Function.Params) {
    if (!scalar(Parameter) || Parameter.Kind != MedVar::Param ||
        Parameter.Id < 0 ||
        !Definitions.emplace(key(Parameter), Definition{-1, -1, Parameter.Size})
             .second)
      return invalid("portable x87 has an invalid or duplicate parameter");
  }
  const auto Define = [&](const MedVar &Value, int Block, int Position) {
    return scalar(Value) && Value.Kind == MedVar::Temp && Value.Id >= 0 &&
           Definitions
               .emplace(key(Value), Definition{Block, Position, Value.Size})
               .second;
  };
  for (const auto &Block : Function.Blocks) {
    if (!Dominators.isReachableFromEntry(Blocks[Block.Id]) ||
        std::set<int>(Block.Preds.begin(), Block.Preds.end()) !=
            Predecessors[Block.Id] ||
        Block.Preds.size() != Predecessors[Block.Id].size())
      return invalid("portable x87 has unreachable or inconsistent CFG edges");
    for (const auto &Phi : Block.Phis)
      if (Phi.ExceptionalEntry || !Define(Phi.Output, Block.Id, -1))
        return invalid("portable x87 has an invalid PHI definition");
    for (size_t I = 0; I != Block.Ops.size(); ++I) {
      const auto &Op = Block.Ops[I];
      if (Op.Dead || Op.NumInputs > Op.Inputs.size() ||
          !Op.IntrinsicOutputs.empty() ||
          Op.MemoryOrdering != NdMemoryOrdering::None ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          Op.RegistrationRoot != MedOp::RegistrationRootKind::None ||
          Op.HasSourceErrorResult ||
          (I + 1 != Block.Ops.size() &&
           (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
            Op.Opcode == NdOp::RETURN)))
        return invalid("portable x87 has an invalid operation boundary");
      if (!integerShape(Op))
        return invalid(
            "portable x87 has an unsupported scalar shape or shift range");
      if (Op.Output.Size && !Define(Op.Output, Block.Id, static_cast<int>(I)))
        return invalid(
            "portable x87 scalar has multiple or invalid definitions");
    }
  }
  const auto Use = [&](const MedVar &Value, int Block, int Position) {
    if (!scalar(Value))
      return false;
    if (Value.isConst())
      return true;
    const auto Found = Definitions.find(key(Value));
    if (Found == Definitions.end() || Found->second.Bytes != Value.Size)
      return false;
    const auto &Def = Found->second;
    return Def.Block < 0 ||
           (Def.Block == Block
                ? Def.Position < Position
                : Dominators.dominates(Blocks[Def.Block], Blocks[Block]));
  };
  for (const auto &Block : Function.Blocks) {
    for (const auto &Phi : Block.Phis) {
      std::set<int> Incoming;
      for (const auto &[Predecessor, Value] : Phi.Args) {
        if (!Predecessors[Block.Id].count(Predecessor) ||
            !Incoming.insert(Predecessor).second ||
            Value.Size != Phi.Output.Size ||
            !Use(Value, Predecessor,
                 static_cast<int>(Function.Blocks[Predecessor].Ops.size())))
          return invalid("portable x87 PHI has an undefined incoming value");
      }
      if (Incoming != Predecessors[Block.Id])
        return invalid("portable x87 PHI omits an incoming edge");
    }
    for (size_t I = 0; I != Block.Ops.size(); ++I)
      for (unsigned J = 0; J != Block.Ops[I].NumInputs; ++J)
        if (!Use(Block.Ops[I].Inputs[J], Block.Id, static_cast<int>(I)))
          return invalid(
              "portable x87 reads an undefined or non-dominating value");
  }
  return llvm::Error::success();
}

} // namespace neverd::portable_x87

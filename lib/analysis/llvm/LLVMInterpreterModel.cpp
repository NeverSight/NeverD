//===- LLVMInterpreterModel.cpp - Shared scalar source model --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LLVMInterpreterModelInternal.h"

#include "neverd/analysis/Limits.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Support/Errc.h"

#include <limits>
#include <system_error>

namespace neverd::analysis::llvm_model {

void Builder::charge(uint64_t &Counter, uint64_t Amount, uint64_t Limit) {
  if (ScalarOptions && ScalarOptions->StopRequested) {
    if (Amount >= UntilStopPoll) {
      if (ScalarOptions->StopRequested())
        throw Failure{"scalar model construction cancelled", false, true};
      UntilStopPoll = limits::analysis::kLLVMScalarStopPollWork;
    } else {
      UntilStopPoll -= Amount;
    }
  }
  if (Amount > Limit || Counter > Limit - Amount)
    throw Failure{"LLVM machine-state model budget exhausted", true};
  Counter += Amount;
}
NdVar Builder::local(unsigned Bytes) {
  work();
  const unsigned Stride = std::max(8U, Bytes);
  if (Stride > (uint64_t{1} << 38) - NextTemporary)
    fail("LLVM temporary namespace exhausted");
  NdVar V = NdVar::tmp(NextTemporary, Bytes);
  NextTemporary += Stride;
  return V;
}
NdVar Builder::fresh(unsigned Bytes) {
  work();
  const unsigned Stride = std::max(8U, Bytes);
  if (Stride > (uint64_t{1} << 41) - NextRegister)
    fail("LLVM register namespace exhausted");
  NdVar V = rvar(NextRegister, Bytes);
  NextRegister += Stride;
  return V;
}
int Builder::block() {
  work();
  auto &Blocks = Result.Function.Blocks;
  if (Blocks.size() >= Limits.MaxBlocks ||
      Blocks.size() >= static_cast<size_t>(std::numeric_limits<int>::max()))
    throw Failure{"LLVM machine-state block budget exhausted", true};
  LowBlock B;
  B.Id = static_cast<int>(Blocks.size());
  B.StartAddr = 0x1000 + Blocks.size() * 0x100;
  B.EndAddr = B.StartAddr + 1;
  Blocks.push_back(std::move(B));
  return Blocks.back().Id;
}
void Builder::emit(LowBlock &Block, LowOp Operation) {
  charge(Operations, 1, Limits.MaxOperations);
  work();
  if (Block.Ops.size() >= static_cast<size_t>(std::numeric_limits<int>::max()))
    throw Failure{"LLVM instruction operation budget exhausted", true};
  Block.Ops.push_back(Operation);
}

void Builder::preflight() {
  if (!F.getParent() || F.isDeclaration() || F.isVarArg())
    fail("LLVM model requires a nonvariadic definition in a module");
  if (ScalarArguments) {
    input(F.arg_size());
    if (!F.getReturnType()->isIntegerTy())
      fail("scalar model requires an integer return");
    bytes(F.getReturnType());
    for (const auto &Arg : F.args()) {
      if (!Arg.getType()->isIntegerTy() ||
          ((!ScalarOptions || ScalarOptions->InputDomain ==
                                  LLVMScalarInputDomain::RequireNoUndef) &&
           !Arg.hasAttribute(llvm::Attribute::NoUndef)))
        fail("scalar model requires noundef integer arguments");
      bytes(Arg.getType());
    }
  } else {
    if (F.arg_size() != 1 || !F.getArg(0)->getType()->isPointerTy() ||
        !F.getReturnType()->isIntegerTy(64))
      fail("LLVM model requires one state pointer and an i64 status result");
    const auto &DL = F.getParent()->getDataLayout();
    if (F.getParent()->getDataLayoutStr().empty() || !DL.isLittleEndian() ||
        DL.getPointerSizeInBits(0) != 64 || DL.getIndexSizeInBits(0) != 64 ||
        DL.isNonIntegralAddressSpace(0) ||
        F.getArg(0)->getType()->getPointerAddressSpace() != 0)
      fail("unsupported LLVM machine-state data layout");
  }
  input(F.size());
  input(F.getParent()->getDataLayoutStr().size());
  for (auto Set : F.getAttributes())
    for (auto A : Set) {
      input();
      // Charge range-list payloads before LLVM verification or diagnostics
      // can traverse/print them, including attributes on invalid positions.
      if (A.isConstantRangeListAttribute())
        input(A.getValueAsConstantRangeList().size());
      if (A.isStringAttribute()) {
        input(A.getKindAsString().size());
        input(A.getValueAsString().size());
      }
    }
  std::set<const llvm::Metadata *> Seen;
  std::vector<const llvm::Metadata *> Pending;
  auto Metadata = [&](const auto &Object) {
    llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 4> Attachments;
    Object.getAllMetadata(Attachments);
    input(Attachments.size());
    for (auto [Kind, Node] : Attachments) {
      (void)Kind;
      Pending.push_back(Node);
    }
  };
  Metadata(F);
  for (const auto &B : F) {
    if (B.empty() || !B.back().isTerminator())
      fail("LLVM block has no terminator");
    input(B.size());
    for (const auto &I : B) {
      input(I.getNumOperands());
      // An assume's inaccessible-memory marker is not a scalar memory
      // access. emitScalar still validates its complete call contract and
      // records the condition as an obligation, never as an input fact.
      if (ScalarArguments &&
          ((I.mayReadOrWriteMemory() && !llvm::isa<llvm::AssumeInst>(I)) ||
           I.getType()->isPointerTy() || llvm::isa<llvm::AllocaInst>(I)))
        fail("memory and pointer effects are unsupported in scalar models");
      for (const auto *V : I.operand_values()) {
        if (llvm::isa<llvm::Constant>(V) &&
            !llvm::isa<llvm::ConstantInt, llvm::Function>(V))
          fail("unsupported LLVM constant, undef or poison operand");
        if (llvm::isa<llvm::ConstantInt>(V))
          bytes(V->getType());
      }
      Metadata(I);
    }
  }
  while (!Pending.empty()) {
    auto *M = Pending.back();
    Pending.pop_back();
    work();
    if (!M || !Seen.insert(M).second)
      continue;
    input();
    if (auto *String = llvm::dyn_cast<llvm::MDString>(M))
      input(String->getString().size());
    if (auto *Node = llvm::dyn_cast<llvm::MDNode>(M)) {
      input(Node->getNumOperands());
      for (const auto &Operand : Node->operands())
        Pending.push_back(Operand.get());
    }
  }
  if (llvm::verifyFunction(F))
    fail("invalid LLVM function");
}
bool Builder::blockLocal(const llvm::Instruction &I) {
  if (llvm::isa<llvm::PHINode>(I))
    return false;
  for (auto *U : I.users()) {
    work();
    auto *Use = llvm::dyn_cast<llvm::Instruction>(U);
    if (!Use || Use->getParent() != I.getParent() ||
        llvm::isa<llvm::PHINode, llvm::SwitchInst>(Use))
      return false;
  }
  return true;
}
unsigned Builder::bytes(llvm::Type *T) {
  if (T->isPointerTy() && T->getPointerAddressSpace() == 0)
    return 8;
  if (!T->isIntegerTy())
    fail("noninteger type unsupported");
  unsigned N = T->getIntegerBitWidth();
  if (N != 1 && N != 8 && N != 16 && N != 32 && N != 64)
    fail("integer width unsupported");
  return (N + 7) / 8;
}
NdVar Builder::value(const llvm::Value *V) {
  if (llvm::isa<llvm::UndefValue, llvm::PoisonValue>(V))
    fail("undef or poison unsupported");
  if (auto *K = llvm::dyn_cast<llvm::ConstantInt>(V)) {
    auto Bytes = bytes(K->getType());
    return num(K->getZExtValue(), Bytes);
  }
  auto I = Values.find(V);
  if (I == Values.end())
    fail("unbound LLVM value");
  return I->second;
}
int Builder::target(const llvm::BasicBlock *From, const llvm::BasicBlock *To) {
  auto I = Edges.find({From, To});
  return I == Edges.end() ? Blocks.at(To) : I->second;
}

void Builder::createGraph() {
  if (ScalarArguments)
    for (const auto &Arg : F.args()) {
      auto Slot = fresh(bytes(Arg.getType()));
      Values[&Arg] = Slot;
      ScalarArguments->push_back({Slot, Arg.getType()->getIntegerBitWidth()});
    }
  for (const auto &B : F) {
    Blocks[&B] = block();
    for (const auto &I : B) {
      work();
      if (I.getType()->isVoidTy() || StateOffsets.count(&I))
        continue;
      auto *Call = llvm::dyn_cast<llvm::CallInst>(&I);
      auto ID = Call && Call->getCalledFunction()
                    ? Call->getCalledFunction()->getIntrinsicID()
                    : llvm::Intrinsic::not_intrinsic;
      if (I.getType()->isStructTy() &&
          (ID == llvm::Intrinsic::sadd_with_overflow ||
           ID == llvm::Intrinsic::ssub_with_overflow)) {
        auto Bytes = bytes(Call->getArgOperand(0)->getType());
        Aggregates[&I] = blockLocal(I) ? std::make_pair(local(Bytes), local(1))
                                       : std::make_pair(fresh(Bytes), fresh(1));
      } else
        Values[&I] = blockLocal(I) ? local(bytes(I.getType()))
                                   : fresh(bytes(I.getType()));
    }
  }
  for (const auto &B : F) {
    if (auto *Switch = llvm::dyn_cast<llvm::SwitchInst>(B.getTerminator())) {
      auto &Dispatch = SwitchBlocks[Switch];
      Dispatch.push_back(Blocks.at(&B));
      for (unsigned I = 1; I < Switch->getNumCases(); ++I)
        Dispatch.push_back(block());
    }
  }
  Result.Function.Entry = address(Blocks.at(&F.front()));
  Result.Function.Name =
      ScalarArguments ? "llvm_scalar_function" : "llvm_machine_state";
  for (const auto &B : F) {
    const auto *T = B.getTerminator();
    for (unsigned I = 0; I != T->getNumSuccessors(); ++I) {
      work();
      const auto *To = T->getSuccessor(I);
      if (To->phis().empty())
        continue;
      auto Key = std::make_pair(&B, To);
      if (!Edges.count(Key)) {
        Edges[Key] = block();
        EdgeOrder.push_back(Key);
      }
    }
  }
}

void Builder::emitEdges() {
  for (const auto &Key : EdgeOrder) {
    auto &B = Result.Function.Blocks[Edges.at(Key)];
    std::vector<std::pair<NdVar, NdVar>> Copies;
    for (const auto &Phi : Key.second->phis()) {
      auto Temp = local(bytes(Phi.getType()));
      emit(B, op(NdOp::COPY, Temp,
                 {value(Phi.getIncomingValueForBlock(Key.first))}));
      Copies.push_back({value(&Phi), Temp});
    }
    // Capture every incoming value before writing any destination PHI.
    for (auto [Dest, Temp] : Copies)
      emit(B, op(NdOp::COPY, Dest, {Temp}));
    B.Succs = {Blocks.at(Key.second)};
    emit(B, op(NdOp::BRANCH, {}, {num(address(B.Succs[0]))}));
  }
}

void Builder::seal() {
  for (auto &B : Result.Function.Blocks) {
    if (B.Ops.empty())
      fail("empty generated LLVM model block");
    LowInstructionBoundary IB;
    IB.Address = B.StartAddr;
    IB.Size = 1;
    IB.OpCount = B.Ops.size();
    for (size_t N = 0; N != B.Ops.size(); ++N) {
      auto &O = B.Ops[N];
      O.Addr = IB.Address;
      O.Seq = static_cast<int>(N);
      if (O.Opcode == NdOp::BRANCH || O.Opcode == NdOp::COND_BR) {
        IB.Control = LowInstructionControl::Branch;
        IB.ControlFlags = LowInstructionControlFlag::Branch;
        IB.Immediate = O.Inputs[0].Offset;
        if (O.Opcode == NdOp::COND_BR)
          IB.ControlFlags |= LowInstructionControlFlag::Conditional;
      } else if (O.Opcode == NdOp::RETURN) {
        IB.Control = LowInstructionControl::Return;
        IB.ControlFlags = LowInstructionControlFlag::Return;
      }
    }
    B.InstructionBoundaries = {IB};
    LowInstructionUndefinedEffects E;
    E.Coverage = LowUndefinedCoverage::Complete;
    E.OpCount = B.Ops.size();
    E.OperationDigest = lowUndefinedOperationDigest(B.Ops);
    Result.Instructions.push_back({B.Id, IB, std::move(E)});
    for (int Successor : B.Succs)
      Result.Function.Blocks[Successor].Preds.push_back(B.Id);
  }
  if (auto Error = validateLowInstructionBoundaries(
          Result.Function, LowInstructionBoundaryRequirement::Required))
    fail(llvm::toString(std::move(Error)));
}

InterpreterMachineStateModel Builder::build() {
  preflight();
  if (!ScalarArguments)
    pointerProjections();
  validateContract();
  if (!ScalarArguments)
    validateInitialization();
  createGraph();
  emitEdges();
  for (const auto &B : F) {
    auto &Out = Result.Function.Blocks[Blocks.at(&B)];
    for (const auto &I : B) {
      work();
      if (llvm::isa<llvm::PHINode>(I) || StateOffsets.count(&I))
        continue;
      if (!(ScalarArguments ? false : emitMemory(Out, I)) &&
          !emitScalar(Out, I) && !emitControl(Out, I))
        fail(llvm::Twine("unsupported LLVM instruction: ") + I.getOpcodeName());
    }
  }
  seal();
  return std::move(Result);
}
} // namespace neverd::analysis::llvm_model

namespace neverd::analysis {
llvm::Expected<LLVMScalarFunctionModel>
modelLLVMScalarFunction(const llvm::Function &Function,
                        const LLVMInterpreterModelLimits &Limits,
                        const LLVMScalarModelOptions &Options) {
  try {
    if (Options.InputDomain != LLVMScalarInputDomain::RequireNoUndef &&
        Options.InputDomain != LLVMScalarInputDomain::DefinedBitvectors)
      llvm_model::fail("unsupported scalar input domain");
    if (Options.StopRequested && Options.StopRequested())
      throw llvm_model::Failure{"scalar model construction cancelled", false,
                                true};
    LLVMScalarFunctionModel Model;
    Model.InputDomain = Options.InputDomain;
    Model.Graph =
        llvm_model::Builder(Function, Limits, 0, &Model.Arguments, &Options)
            .build();
    Model.ResultBits = Function.getReturnType()->getIntegerBitWidth();
    if (Options.StopRequested && Options.StopRequested())
      throw llvm_model::Failure{"scalar model construction cancelled", false,
                                true};
    return Model;
  } catch (const llvm_model::Failure &E) {
    return llvm::createStringError(E.Cancelled ? std::errc::operation_canceled
                                   : E.Budget  ? std::errc::result_out_of_range
                                               : std::errc::invalid_argument,
                                   "%s", E.Message.c_str());
  }
}
} // namespace neverd::analysis

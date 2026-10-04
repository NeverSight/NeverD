//===- LLVMScalarEquivalence.cpp - Complete finite control partitions -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/analysis/LLVMScalarEquivalence.h"

#include "neverd/symbolic/SymExec.h"
#include "neverd/symbolic/SymKnownBits.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/Errc.h"

#include <algorithm>
#include <set>

namespace neverd::analysis {
namespace {
namespace sym = symbolic;
using Ref = sym::SymRef;
using Bit = LLVMScalarControlBit;
using Status = LLVMScalarEquivalenceStatus;

struct Failure {
  Status Code;
  const char *Message;
};

struct Budget {
  uint64_t Remaining;
  void spend(uint64_t N = 1) {
    if (N > Remaining) {
      Remaining = 0;
      throw Failure{Status::BudgetExceeded,
                    "scalar proof work budget exhausted"};
    }
    Remaining -= N;
  }
};

struct Evaluation {
  Ref Value;
  std::set<Bit> Needed;
};

class Executor {
  sym::SymContext &C;
  Budget &Work;
  const LLVMScalarEquivalenceLimits &Limits;
  llvm::DenseMap<uint32_t, unsigned> Variables;
  llvm::DenseMap<uint32_t, Ref> Folded;
  sym::SymKnownBits BitFacts;
  unsigned InputVariables;

  std::optional<llvm::APInt> constantWindow(Ref R, unsigned Low,
                                            unsigned Width) {
    Work.spend();
    const unsigned Available = static_cast<unsigned>(
        std::min<uint64_t>(Work.Remaining, sym::SymKnownBits::MaxQueryWork));
    unsigned Remaining = Available;
    auto K = C.constantWindow(R, Low, Width, Remaining);
    if (!K && Width && Low <= C.width(R) && Width <= C.width(R) - Low) {
      if (auto Facts = BitFacts.query(R, Remaining)) {
        auto Slice = Facts->extractBits(Width, Low);
        if (Slice.isConstant())
          K = Slice.One;
      }
    }
    Work.spend(Available - Remaining);
    // The final value comparison has no later traversal to detect exhaustion.
    if (!K && !Work.Remaining)
      Work.spend();
    return K;
  }

  // Every unhandled expression conservatively demands all operand bits.
  // A successful empty result proves constancy. Early variable discovery is
  // useful only to decline a constant fold, never to refine a control domain.
  std::set<Bit> dependencies(Ref Root, bool StopAtVariable = false) {
    std::set<Bit> Result;
    std::set<std::pair<uint32_t, unsigned>> Seen;
    llvm::SmallVector<std::pair<Ref, unsigned>, 64> Pending;
    auto Add = [&](Ref R, unsigned B) {
      if (B < C.width(R)) {
        Work.spend();
        Pending.push_back({R, B});
      }
    };
    auto All = [&](Ref R) {
      for (unsigned B = 0; B < C.width(R); ++B)
        Add(R, B);
    };
    All(Root);
    while (!Pending.empty()) {
      auto [R, B] = Pending.pop_back_val();
      Work.spend();
      if (!Seen.insert({R.index(), B}).second)
        continue;
      if (constantWindow(R, B, 1))
        continue;
      const auto Ops = C.operands(R);
      switch (C.op(R)) {
      case sym::SymOp::Const:
        break;
      case sym::SymOp::Var: {
        auto I = Variables.find(C.varId(R));
        if (I == Variables.end())
          throw Failure{Status::Unsupported, "unexpected symbolic input"};
        Result.insert({I->second, B});
        if (StopAtVariable)
          return Result;
        if (Result.size() > Limits.MaxControlBits)
          throw Failure{Status::BudgetExceeded, "control-bit budget exhausted"};
        break;
      }
      case sym::SymOp::Not:
      case sym::SymOp::And:
      case sym::SymOp::Or:
      case sym::SymOp::Xor:
        for (auto O : Ops)
          Add(O, B);
        break;
      case sym::SymOp::Add:
      case sym::SymOp::Mul:
        for (auto O : Ops)
          for (unsigned N = 0; N <= B; ++N)
            Add(O, N);
        break;
      case sym::SymOp::Extract:
        Add(Ops[0], B + C.node(R).Aux);
        break;
      case sym::SymOp::ZExt:
        Add(Ops[0], B);
        break;
      case sym::SymOp::SExt:
        Add(Ops[0], std::min(B, C.width(Ops[0]) - 1));
        break;
      case sym::SymOp::Concat: {
        unsigned Low = 0;
        for (auto O : llvm::reverse(Ops)) {
          if (B >= Low && B < Low + C.width(O))
            Add(O, B - Low);
          Low += C.width(O);
        }
        break;
      }
      case sym::SymOp::Shl:
      case sym::SymOp::LShr:
      case sym::SymOp::AShr:
      case sym::SymOp::Rol:
      case sym::SymOp::Ror: {
        auto K = C.asConst(Ops[1]);
        const unsigned W = C.width(Ops[0]);
        if (!K || K->uge(W)) {
          for (auto O : Ops)
            All(O);
          break;
        }
        const unsigned N = K->getZExtValue();
        if (C.op(R) == sym::SymOp::Shl) {
          if (B >= N)
            Add(Ops[0], B - N);
        } else if (C.op(R) == sym::SymOp::LShr)
          Add(Ops[0], B + N);
        else if (C.op(R) == sym::SymOp::AShr)
          Add(Ops[0], std::min(W - 1, B + N));
        else if (C.op(R) == sym::SymOp::Rol)
          Add(Ops[0], (B + W - N) % W);
        else
          Add(Ops[0], (B + N) % W);
        break;
      }
      case sym::SymOp::Ite:
        All(Ops[0]);
        Add(Ops[1], B);
        Add(Ops[2], B);
        break;
      default:
        for (auto O : Ops)
          All(O);
        break;
      }
    }
    return Result;
  }

  Ref foldUncached(Ref R) {
    if (auto K = constantWindow(R, 0, C.width(R)))
      return C.mkConst(*K);
    if (!dependencies(R, true).empty())
      return R;
    // Evaluation follows a complete no-variable-dependence proof. It does
    // not specialize or sample a runtime input. The whole context is bounded
    // and charged before the shared DAG evaluator traverses this expression.
    Work.spend(C.numNodes() + C.numVars());
    llvm::SmallVector<llvm::APInt, 8> Values;
    for (unsigned N = 0; N < C.numVars(); ++N)
      Values.push_back(llvm::APInt::getAllOnes(C.width(C.varRef(N))));
    return C.mkConst(C.eval(R, Values));
  }

  Ref fold(Ref R) {
    if (!R || C.isConst(R))
      return R;
    Work.spend();
    auto I = Folded.find(R.index());
    if (I != Folded.end())
      return I->second;
    // Expressions and their input partition are immutable in this context.
    // Repeated model copies may share a query result, including conservative
    // nonconstant results; another partition always starts a fresh context.
    auto Value = foldUncached(R);
    Folded[R.index()] = Value;
    return Value;
  }

  void checkNodes() {
    if (C.numNodes() > Limits.MaxSymbolicNodes)
      throw Failure{Status::BudgetExceeded, "symbolic-node budget exhausted"};
    if (C.numVars() != InputVariables)
      throw Failure{Status::Unsupported, "model read an uninitialized input"};
  }

public:
  bool sameValue(Ref A, Ref B) {
    if (A == B)
      return true;
    auto Equality = C.mkEq(A, B);
    checkNodes();
    auto K = constantWindow(Equality, 0, 1);
    return K && K->isOne();
  }

  Executor(sym::SymContext &Context, Budget &Budget,
           const LLVMScalarEquivalenceLimits &Limits,
           llvm::ArrayRef<Ref> Inputs)
      : C(Context), Work(Budget), Limits(Limits), BitFacts(Context),
        InputVariables(Context.numVars()) {
    for (unsigned N = 0; N < Inputs.size(); ++N)
      Variables[C.varId(Inputs[N])] = N;
  }

  Evaluation run(const LLVMScalarFunctionModel &Model,
                 llvm::ArrayRef<Ref> Arguments) {
    sym::SymState State(C);
    sym::SymExec Exec(C, State);
    for (unsigned N = 0; N < Arguments.size(); ++N) {
      Work.spend();
      const auto &Arg = Model.Arguments[N];
      State.write(sym::SymSpace::Register, Arg.Storage.Offset,
                  C.mkZExt(Arguments[N], Arg.Storage.Size * 8));
    }
    State.write(sym::SymSpace::Register, LLVMInterpreterDefinednessOffset,
                C.mkZero(8));
    const auto &F = Model.Graph.Function;
    const LowBlock *Block = nullptr;
    for (const auto &B : F.Blocks) {
      Work.spend();
      if (B.StartAddr == F.Entry)
        Block = &B;
    }
    if (!Block)
      throw Failure{Status::Unsupported, "model has no entry block"};
    for (uint64_t Visits = 0; Visits < Limits.MaxBlockVisits; ++Visits) {
      const LowBlock *Next = nullptr;
      for (const auto &O : Block->Ops) {
        Work.spend();
        checkNodes();
        auto Step = Exec.step(O);
        checkNodes();
        if (Step == sym::StepResult::Unmodelled || Exec.unmodelledCount())
          throw Failure{Status::Unsupported, "unmodeled scalar operation"};
        if (Step == sym::StepResult::Continue) {
          if (O.Output.isReg() || O.Output.isTemp()) {
            auto V = fold(Exec.operandValue(O.Output));
            State.write(O.Output.isReg() ? sym::SymSpace::Register
                                         : sym::SymSpace::Temporary,
                        O.Output.Offset, V);
          }
          continue;
        }
        if (Step == sym::StepResult::Return) {
          auto Definedness = fold(State.read(
              sym::SymSpace::Register, LLVMInterpreterDefinednessOffset, 1));
          auto K = C.asConst(Definedness);
          if (!K)
            return {{}, dependencies(Definedness)};
          if (!K->isZero())
            throw Failure{Status::Unproved,
                          "executed source operation is not defined"};
          auto Value = fold(Exec.branchTarget());
          checkNodes();
          return {Value, {}};
        }
        unsigned Edge = 0;
        if (Step == sym::StepResult::CondBranch) {
          auto Condition = fold(Exec.branchCondition());
          auto K = C.asConst(Condition);
          if (!K)
            return {{}, dependencies(Condition)};
          Edge = K->isZero() ? 1 : 0;
        } else if (Step != sym::StepResult::Branch)
          throw Failure{Status::Unsupported,
                        "unexpected model control transfer"};
        if (Edge >= Block->Succs.size() || Block->Succs[Edge] < 0 ||
            static_cast<size_t>(Block->Succs[Edge]) >= F.Blocks.size())
          throw Failure{Status::Unsupported, "invalid model successor"};
        Next = &F.Blocks[Block->Succs[Edge]];
        break;
      }
      if (!Next)
        throw Failure{Status::Unsupported, "model block has no transfer"};
      Block = Next;
    }
    throw Failure{Status::BudgetExceeded, "scalar path visit budget exhausted"};
  }
};

} // namespace

LLVMScalarEquivalenceResult
checkLLVMScalarEquivalence(const llvm::Function &Original,
                           const llvm::Function &Candidate,
                           const LLVMScalarEquivalenceLimits &Limits) {
  LLVMScalarEquivalenceResult Result;
  Budget Work{Limits.MaxWork};
  std::set<Bit> Domain;
  auto Model = [&](const llvm::Function &F) {
    auto M = modelLLVMScalarFunction(F, Limits.Model);
    if (!M) {
      llvm::handleAllErrors(M.takeError(), [&](const llvm::ErrorInfoBase &E) {
        Result.Diagnostic = E.message();
        Result.Status =
            E.convertToErrorCode() == llvm::errc::result_out_of_range
                ? Status::BudgetExceeded
                : Status::Unsupported;
      });
      return std::optional<LLVMScalarFunctionModel>();
    }
    return std::optional<LLVMScalarFunctionModel>(std::move(*M));
  };
  auto Left = Model(Original);
  if (!Left)
    return Result;
  auto Right = Model(Candidate);
  if (!Right)
    return Result;
  if (Left->ResultBits != Right->ResultBits ||
      Left->Arguments.size() != Right->Arguments.size() ||
      !llvm::equal(Left->Arguments, Right->Arguments,
                   [](auto A, auto B) { return A.Bits == B.Bits; })) {
    Result.Status = Status::Unsupported;
    Result.Diagnostic = "scalar signatures differ";
    return Result;
  }
  try {
    for (;;) {
      Work.spend();
      if (Domain.size() > Limits.MaxControlBits || Domain.size() >= 63 ||
          (uint64_t{1} << Domain.size()) > Limits.MaxPartitions)
        throw Failure{Status::BudgetExceeded,
                      "scalar partition budget exhausted"};
      std::vector<Bit> Bits(Domain.begin(), Domain.end());
      const uint64_t Count = uint64_t{1} << Bits.size();
      Result.CompletedPartitions = 0;
      bool Refine = false;
      for (uint64_t Case = 0; Case < Count; ++Case) {
        Work.spend();
        ++Result.Attempts;
        sym::SymContext Context;
        llvm::SmallVector<Ref, 8> Inputs, Args;
        for (unsigned A = 0; A < Left->Arguments.size(); ++A) {
          Work.spend(1 + Bits.size());
          const unsigned W = Left->Arguments[A].Bits;
          llvm::APInt Mask(W, 0), Value(W, 0);
          for (unsigned K = 0; K < Bits.size(); ++K)
            if (Bits[K].Argument == A) {
              Mask.setBit(Bits[K].Bit);
              if (Case & (uint64_t{1} << K))
                Value.setBit(Bits[K].Bit);
            }
          auto Input = Context.mkFreshVar(W, "input");
          Inputs.push_back(Input);
          Args.push_back(
              Context.mkOr(Context.mkAnd(Input, Context.mkConst(~Mask)),
                           Context.mkConst(Value)));
        }
        Executor Exec(Context, Work, Limits, Inputs);
        auto L = Exec.run(*Left, Args);
        auto R = L.Needed.empty() ? Exec.run(*Right, Args) : Evaluation{};
        auto &Needed = L.Needed.empty() ? R.Needed : L.Needed;
        if (!Needed.empty()) {
          const auto OldSize = Domain.size();
          Work.spend(Needed.size());
          Domain.insert(Needed.begin(), Needed.end());
          if (OldSize == Domain.size())
            throw Failure{Status::Unproved,
                          "control partition did not resolve"};
          Refine = true;
          break;
        }
        Work.spend();
        if (!L.Value || !R.Value || !Exec.sameValue(L.Value, R.Value))
          throw Failure{Status::Unproved,
                        "symbolic returns differ or remain unproved"};
        ++Result.CompletedPartitions;
      }
      if (!Refine) {
        Result.Status = Status::Proved;
        break;
      }
    }
  } catch (const Failure &E) {
    Result.Status = E.Code;
    Result.Diagnostic = E.Message;
  }
  Result.Work = Limits.MaxWork - Work.Remaining;
  Result.ControlBits.assign(Domain.begin(), Domain.end());
  return Result;
}

} // namespace neverd::analysis

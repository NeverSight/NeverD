//===- LLVMScalarEquivalence.cpp - Complete finite control partitions -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/analysis/LLVMScalarEquivalence.h"

#include "neverd/analysis/Limits.h"
#include "neverd/symbolic/SymExec.h"
#include "neverd/symbolic/SymKnownBits.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/Errc.h"

#include <algorithm>
#include <set>
#include <system_error>

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
  const LLVMScalarModelOptions &Options;
  bool Exceeded = false;
  uint64_t UntilStopPoll = 0;
  void poll() {
    if (Options.StopRequested && Options.StopRequested())
      throw Failure{Status::Cancelled, "scalar proof cancelled"};
    UntilStopPoll = limits::analysis::kLLVMScalarStopPollWork;
  }
  void spend(uint64_t N = 1) {
    if (Options.StopRequested) {
      if (N >= UntilStopPoll)
        poll();
      else
        UntilStopPoll -= N;
    }
    if (N > Remaining) {
      Remaining = 0;
      Exceeded = true;
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
  llvm::DenseMap<uint32_t, Ref> Decisions;
  // Query these facts only after visiting a decision's children. A shallow
  // query may cache sound but incomplete facts near its depth/work boundary;
  // those facts must not hide the stronger bottom-up normalization here.
  sym::SymKnownBits DecisionFacts;
  unsigned InputVariables;

  std::optional<llvm::APInt> constantWindow(Ref R, unsigned Low, unsigned Width,
                                            sym::SymKnownBits &Facts) {
    Work.spend();
    const unsigned Available = static_cast<unsigned>(
        std::min<uint64_t>(Work.Remaining, sym::SymKnownBits::MaxQueryWork));
    unsigned Remaining = Available;
    auto K = C.constantWindow(R, Low, Width, Remaining);
    bool Complete = bool(K);
    if (!K && Width && Low <= C.width(R) && Width <= C.width(R) - Low) {
      if (auto Known = Facts.query(R, Remaining)) {
        Complete = true;
        auto Slice = Known->extractBits(Width, Low);
        if (Slice.isConstant())
          K = Slice.One;
      }
    }
    Work.spend(Available - Remaining);
    // An incomplete final query has no later traversal to detect exhaustion.
    // Completed unknown facts, however, are an ordinary refusal even when
    // they consume the last available unit.
    if (!Complete && !Work.Remaining &&
        Available < sym::SymKnownBits::MaxQueryWork)
      Work.spend();
    return K;
  }

  std::optional<llvm::APInt> constantWindow(Ref R, unsigned Low,
                                            unsigned Width) {
    return constantWindow(R, Low, Width, BitFacts);
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

  Ref evaluateIndependent(Ref R) {
    // Evaluation follows a complete no-variable-dependence proof. It does
    // not specialize or sample a runtime input. The whole context is bounded
    // and charged before the shared DAG evaluator traverses this expression.
    Work.spend(C.numNodes() + C.numVars());
    llvm::SmallVector<llvm::APInt, 8> Values;
    for (unsigned N = 0; N < C.numVars(); ++N)
      Values.push_back(llvm::APInt::getAllOnes(C.width(C.varRef(N))));
    return C.mkConst(C.eval(R, Values));
  }

  Ref foldUncached(Ref R) {
    if (auto K = constantWindow(R, 0, C.width(R)))
      return C.mkConst(*K);
    if (!dependencies(R, true).empty())
      return R;
    return evaluateIndependent(R);
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

  Ref normalizeValue(Ref Root) {
    llvm::SmallVector<std::pair<Ref, bool>, 64> Pending;
    Work.spend();
    Pending.push_back({Root, false});
    while (!Pending.empty()) {
      Work.spend();
      auto [R, Ready] = Pending.pop_back_val();
      if (Decisions.contains(R.index()))
        continue;
      if (C.isConst(R) || C.op(R) == sym::SymOp::Var) {
        Decisions[R.index()] = R;
        continue;
      }
      if (!Ready) {
        Work.spend(1 + C.operands(R).size());
        Pending.push_back({R, true});
        for (auto O : C.operands(R))
          Pending.push_back({O, false});
        continue;
      }
      llvm::SmallVector<Ref, 4> Operands;
      bool Changed = false;
      for (auto O : C.operands(R)) {
        Work.spend();
        Ref V = Decisions.at(O.index());
        Operands.push_back(V);
        Changed |= V != O;
      }
      Ref V = R;
      if (Changed) {
        // Copy the operands before rebuilding: interning may grow both of
        // the context's node/operand vectors. Each original DAG node is
        // normalized once, without expanding shared terms into trees.
        Work.spend(1 + Operands.size());
        V = C.rebuild(R, Operands);
        checkNodes();
      }
      if (auto K = constantWindow(V, 0, C.width(V), DecisionFacts)) {
        V = C.mkConst(*K);
        checkNodes();
      }
      Decisions[R.index()] = V;
    }
    return Decisions.at(Root.index());
  }

  Evaluation decision(Ref R) {
    auto Value = fold(R);
    if (!C.isConst(Value))
      Value = normalizeValue(Value);
    checkNodes();
    if (C.isConst(Value))
      return {Value, {}};
    auto Needed = dependencies(Value);
    if (!Needed.empty())
      return {{}, std::move(Needed)};
    // The complete bit-dependency walk can prove independence even when the
    // initial bounded whole-value query could not. An empty result is a
    // constant decision, never an absent return or permission to skip it.
    Value = evaluateIndependent(Value);
    checkNodes();
    return {Value, {}};
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
    if (K)
      return K->isOne();
    // Try exact identity and the bounded equality query before normalizing
    // data. Rebuild only the demanded return DAGs; every source operation
    // and definedness/control obligation has already executed below.
    auto Left = normalizeValue(A), Right = normalizeValue(B);
    if (Left == Right)
      return true;
    auto NormalizedEquality = C.mkEq(Left, Right);
    checkNodes();
    K = constantWindow(NormalizedEquality, 0, 1);
    return K && K->isOne();
  }

  Executor(sym::SymContext &Context, Budget &Budget,
           const LLVMScalarEquivalenceLimits &Limits,
           llvm::ArrayRef<Ref> Inputs)
      : C(Context), Work(Budget), Limits(Limits), BitFacts(Context),
        DecisionFacts(Context), InputVariables(Context.numVars()) {
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
        // Keep symbolic intermediate values intact. Eagerly trying to fold
        // every write repeatedly walks growing data DAGs, including values
        // that cannot affect an observation. Control, definedness and final
        // equality normalize their demanded values within this same budget.
        if (Step == sym::StepResult::Continue)
          continue;
        if (Step == sym::StepResult::Return) {
          auto Definedness = decision(State.read(
              sym::SymSpace::Register, LLVMInterpreterDefinednessOffset, 1));
          if (!Definedness.Value)
            return Definedness;
          auto K = C.asConst(Definedness.Value);
          if (!K->isZero())
            throw Failure{Status::Unproved,
                          "executed source operation is not defined"};
          auto Value = Exec.branchTarget();
          checkNodes();
          return {Value, {}};
        }
        unsigned Edge = 0;
        if (Step == sym::StepResult::CondBranch) {
          auto Condition = decision(Exec.branchCondition());
          if (!Condition.Value)
            return Condition;
          auto K = C.asConst(Condition.Value);
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
                           const LLVMScalarEquivalenceLimits &Limits,
                           const LLVMScalarModelOptions &Options) {
  LLVMScalarEquivalenceResult Result;
  Result.InputDomain = Options.InputDomain;
  Budget Work{Limits.MaxWork, Options};
  std::set<Bit> Domain;
  auto Model = [&](const llvm::Function &F) {
    auto M = modelLLVMScalarFunction(F, Limits.Model, Options);
    if (!M) {
      llvm::handleAllErrors(M.takeError(), [&](const llvm::ErrorInfoBase &E) {
        Result.Diagnostic = E.message();
        Result.Status =
            E.convertToErrorCode() == std::errc::operation_canceled
                ? Status::Cancelled
            : E.convertToErrorCode() == llvm::errc::result_out_of_range
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
  // A self-query still proves admission, definedness and termination over
  // the complete control domain. Only its duplicate model/execution is
  // redundant. Identity is local to this call, never a name or cached result.
  std::optional<LLVMScalarFunctionModel> Right;
  if (&Original != &Candidate) {
    Right = Model(Candidate);
    if (!Right)
      return Result;
  }
  if (Right &&
      (Left->ResultBits != Right->ResultBits ||
       Left->Arguments.size() != Right->Arguments.size() ||
       !llvm::equal(Left->Arguments, Right->Arguments,
                    [](auto A, auto B) { return A.Bits == B.Bits; }))) {
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
        Evaluation R;
        if (L.Needed.empty())
          R = Right ? Exec.run(*Right, Args) : Evaluation{L.Value, {}};
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
        Work.poll();
        Result.Status = Status::Proved;
        break;
      }
    }
  } catch (const Failure &E) {
    Result.Status = E.Code;
    Result.Diagnostic = E.Message;
  }
  Result.Work = Limits.MaxWork - Work.Remaining;
  Result.WorkLimitExceeded = Work.Exceeded;
  Result.ControlBits.assign(Domain.begin(), Domain.end());
  return Result;
}

} // namespace neverd::analysis

//===- Z3Solver.cpp - Optional native Z3 bitvector queries ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/solver/Z3Solver.h"

#include "llvm/ADT/SmallString.h"

#include <optional>
#include <utility>
#include <vector>

#if defined(NEVERD_HAS_Z3)
#include <z3++.h>
#endif

namespace neverd::solver {

using symbolic::SymContext;
using symbolic::SymOp;
using symbolic::SymRef;

namespace {

bool validRef(const SymContext &Ctx, SymRef R) {
  return R.isValid() && R.index() < Ctx.numNodes() && Ctx.width(R) != 0;
}

} // namespace

class Z3Solver::Impl {
public:
  SymContext &Ctx;
  Z3SolverOptions Options;
  BitVectorModel Model;
  std::vector<SymRef> Failed;
  std::string Reason;
  std::optional<SatResult> Error;

  bool fail(SatResult Result, std::string Message = {}) {
    Error = Result;
    Reason = std::move(Message);
    Model.clear();
    Failed.clear();
    return false;
  }

  void clearAnswer() {
    Model.clear();
    Failed.clear();
    if (!Error)
      Reason.clear();
  }

#if defined(NEVERD_HAS_Z3)
  // Expressions and solver must be destroyed before their owning context.
  z3::context ZCtx;
  z3::solver Solver;
  z3::expr_vector LastAssumptions;
  std::vector<std::optional<z3::expr>> Nodes;
  std::vector<std::pair<uint32_t, uint32_t>> Variables;

  Impl(SymContext &Ctx, const Z3SolverOptions &Options)
      : Ctx(Ctx), Options(Options), Solver(ZCtx, "QF_BV"),
        LastAssumptions(ZCtx) {
    z3::params Params(ZCtx);
    Params.set("timeout", Options.TimeoutMs);
    Params.set("rlimit", Options.ResourceLimit);
    Solver.set(Params);
  }

  bool validateNode(SymRef R) {
    if (!validRef(Ctx, R))
      return fail(SatResult::Invalid);
    const auto &N = Ctx.node(R);
    // A SymContext appends a node after its children. Check this invariant
    // before dereferencing an operand, and reject cycles or foreign handles.
    for (SymRef Child : Ctx.operands(R))
      if (!validRef(Ctx, Child) || Child.index() >= R.index())
        return fail(SatResult::Invalid);

    auto sameWidth = [&]() {
      for (SymRef Child : Ctx.operands(R))
        if (Ctx.width(Child) != N.Width)
          return false;
      return true;
    };
    auto operandWidth = [&](unsigned I) {
      return Ctx.width(Ctx.operand(R, I));
    };
    bool Valid = false;
    switch (N.Op) {
    case SymOp::Const:
      Valid = N.NumOperands == 0;
      break;
    case SymOp::Var:
      Valid = N.NumOperands == 0 && N.Aux < Ctx.numVars() &&
              Ctx.varInfo(static_cast<uint32_t>(N.Aux)).Width == N.Width;
      break;
    case SymOp::Add:
    case SymOp::Mul:
    case SymOp::And:
    case SymOp::Or:
    case SymOp::Xor:
      Valid = N.NumOperands != 0 && sameWidth();
      break;
    case SymOp::Not:
      Valid = N.NumOperands == 1 && sameWidth();
      break;
    case SymOp::UDiv:
    case SymOp::SDiv:
    case SymOp::URem:
    case SymOp::SRem:
      Valid = N.NumOperands == 2 && sameWidth();
      break;
    case SymOp::Shl:
    case SymOp::LShr:
    case SymOp::AShr:
    case SymOp::Rol:
    case SymOp::Ror:
      Valid = N.NumOperands == 2 && operandWidth(0) == N.Width;
      break;
    case SymOp::Extract:
      Valid = N.NumOperands == 1 && N.Aux <= operandWidth(0) &&
              N.Width <= operandWidth(0) - N.Aux;
      break;
    case SymOp::Concat: {
      uint64_t Width = 0;
      for (SymRef Child : Ctx.operands(R))
        Width += Ctx.width(Child);
      Valid = N.NumOperands != 0 && Width == N.Width;
      break;
    }
    case SymOp::ZExt:
    case SymOp::SExt:
      Valid = N.NumOperands == 1 && operandWidth(0) <= N.Width;
      break;
    case SymOp::Ite:
      Valid = N.NumOperands == 3 && operandWidth(0) == 1 &&
              operandWidth(1) == N.Width && operandWidth(2) == N.Width;
      break;
    case SymOp::Eq:
    case SymOp::Ult:
    case SymOp::Ule:
    case SymOp::Slt:
    case SymOp::Sle:
      Valid = N.NumOperands == 2 && N.Width == 1 &&
              operandWidth(0) == operandWidth(1);
      break;
    }
    return Valid || fail(SatResult::Invalid);
  }

  z3::expr predicate(const z3::expr &Value) {
    return Value != ZCtx.bv_val(0, Value.get_sort().bv_size());
  }

  z3::expr bit(const z3::expr &Predicate) {
    return z3::ite(Predicate, ZCtx.bv_val(1, 1), ZCtx.bv_val(0, 1));
  }

  z3::expr shiftAmount(const z3::expr &Amount, uint32_t Width, bool Rotate) {
    const unsigned AmountWidth = Amount.get_sort().bv_size();
    if (AmountWidth < Width)
      return z3::zext(Amount, Width - AmountWidth);
    if (AmountWidth == Width)
      return Amount;
    if (Rotate) {
      // Reduction must precede truncation: at width 3, rotating by 256 is
      // rotating by 1, whereas truncating that amount first would yield zero.
      return z3::urem(Amount, ZCtx.bv_val(Width, AmountWidth))
          .extract(Width - 1, 0);
    }
    const z3::expr High = Amount.extract(AmountWidth - 1, Width);
    return z3::ite(predicate(High), ZCtx.bv_val(Width, Width),
                   Amount.extract(Width - 1, 0));
  }

  z3::expr translateNode(SymRef R) {
    const auto &N = Ctx.node(R);
    auto arg = [&](unsigned I) -> const z3::expr & {
      return *Nodes[Ctx.operand(R, I).index()];
    };
    switch (N.Op) {
    case SymOp::Const: {
      llvm::SmallString<80> Decimal;
      Ctx.constValue(R).toString(Decimal, 10, false);
      return ZCtx.bv_val(Decimal.c_str(), N.Width);
    }
    case SymOp::Var: {
      const uint32_t Id = Ctx.varId(R);
      Variables.emplace_back(Id, R.index());
      const std::string Name = "v" + std::to_string(Id);
      return ZCtx.bv_const(Name.c_str(), N.Width);
    }
    case SymOp::Mul: {
      unsigned Zeros = 0;
      if (Ctx.isConst(Ctx.operand(R, 0))) {
        const auto Trailing = Ctx.constValue(Ctx.operand(R, 0)).countr_zero();
        if (Trailing < N.Width)
          Zeros = Trailing;
      }
      // In Z/(2^W), a coefficient divisible by 2^K observes only W-K bits
      // of each other factor. Restore the complete word after the product.
      const unsigned Observed = N.Width - Zeros;
      std::optional<z3::expr> Value;
      if (Zeros)
        Value = arg(0).extract(N.Width - 1, Zeros);
      for (unsigned I = Zeros ? 1 : 0; I < N.NumOperands;) {
        unsigned End = I + 1;
        while (End < N.NumOperands && Ctx.operand(R, End) == Ctx.operand(R, I))
          ++End;
        // Equal handles denote the same complete word. Share squared powers
        // instead of constructing a linear chain of repeated multiplications.
        z3::expr Factor = Zeros ? arg(I).extract(Observed - 1, 0) : arg(I);
        z3::expr Power = Factor;
        for (unsigned Remaining = End - I - 1; Remaining; Remaining >>= 1) {
          if (Remaining & 1)
            Factor = Factor * Power;
          if (Remaining > 1)
            Power = Power * Power;
        }
        Value = Value ? *Value * Factor : Factor;
        I = End;
      }
      return Zeros ? z3::concat(*Value, ZCtx.bv_val(0, Zeros)) : *Value;
    }
    case SymOp::Add:
    case SymOp::And:
    case SymOp::Or:
    case SymOp::Xor:
    case SymOp::Concat: {
      z3::expr Value = arg(0);
      for (unsigned I = 1; I < N.NumOperands; ++I) {
        switch (N.Op) {
        case SymOp::Add:
          Value = Value + arg(I);
          break;
        case SymOp::And:
          Value = Value & arg(I);
          break;
        case SymOp::Or:
          Value = Value | arg(I);
          break;
        case SymOp::Xor:
          Value = Value ^ arg(I);
          break;
        case SymOp::Concat:
          Value = z3::concat(Value, arg(I));
          break;
        default:
          break;
        }
      }
      return Value;
    }
    case SymOp::Not:
      return ~arg(0);
    case SymOp::UDiv:
      return z3::udiv(arg(0), arg(1));
    case SymOp::SDiv:
      return arg(0) / arg(1);
    case SymOp::URem:
      return z3::urem(arg(0), arg(1));
    case SymOp::SRem:
      return z3::srem(arg(0), arg(1));
    case SymOp::Shl:
      return z3::shl(arg(0), shiftAmount(arg(1), N.Width, false));
    case SymOp::LShr:
      return z3::lshr(arg(0), shiftAmount(arg(1), N.Width, false));
    case SymOp::AShr:
      return z3::ashr(arg(0), shiftAmount(arg(1), N.Width, false));
    case SymOp::Rol: {
      z3::expr Amount = shiftAmount(arg(1), N.Width, true);
      return z3::to_expr(ZCtx, Z3_mk_ext_rotate_left(ZCtx, arg(0), Amount));
    }
    case SymOp::Ror: {
      z3::expr Amount = shiftAmount(arg(1), N.Width, true);
      return z3::to_expr(ZCtx, Z3_mk_ext_rotate_right(ZCtx, arg(0), Amount));
    }
    case SymOp::Extract:
      return arg(0).extract(static_cast<unsigned>(N.Aux + N.Width - 1),
                            static_cast<unsigned>(N.Aux));
    case SymOp::ZExt:
      return z3::zext(arg(0), N.Width - arg(0).get_sort().bv_size());
    case SymOp::SExt:
      return z3::sext(arg(0), N.Width - arg(0).get_sort().bv_size());
    case SymOp::Ite:
      return z3::ite(predicate(arg(0)), arg(1), arg(2));
    case SymOp::Eq:
      return bit(arg(0) == arg(1));
    case SymOp::Ult:
      return bit(z3::ult(arg(0), arg(1)));
    case SymOp::Ule:
      return bit(z3::ule(arg(0), arg(1)));
    case SymOp::Slt:
      return bit(z3::slt(arg(0), arg(1)));
    case SymOp::Sle:
      return bit(z3::sle(arg(0), arg(1)));
    }
    // validateNode rejects unknown operators before translation.
    throw z3::exception("invalid symbolic operator");
  }

  bool translate(SymRef Root) {
    if (Error)
      return false;
    if (!validRef(Ctx, Root))
      return fail(SatResult::Invalid);
    Nodes.resize(Ctx.numNodes());
    std::vector<std::pair<SymRef, bool>> Work = {{Root, false}};
    while (!Work.empty()) {
      const auto [R, Expanded] = Work.back();
      Work.pop_back();
      if (Nodes[R.index()])
        continue;
      if (Expanded) {
        Nodes[R.index()] = translateNode(R);
        continue;
      }
      if (!validateNode(R))
        return false;
      Work.emplace_back(R, true);
      for (SymRef Child : Ctx.operands(R))
        if (!Nodes[Child.index()])
          Work.emplace_back(Child, false);
    }
    return true;
  }

  bool assertion(SymRef A, std::optional<SymRef> B, bool Positive) {
    clearAnswer();
    LastAssumptions = z3::expr_vector(ZCtx);
    try {
      if (!translate(A))
        return false;
      z3::expr Pred = predicate(*Nodes[A.index()]);
      if (B) {
        if (!translate(*B))
          return false;
        if (Ctx.width(A) != Ctx.width(*B))
          return fail(SatResult::Invalid);
        Pred = *Nodes[A.index()] == *Nodes[B->index()];
      }
      Solver.add(Positive ? Pred : !Pred);
      return true;
    } catch (const z3::exception &E) {
      return fail(SatResult::Unknown, E.msg());
    }
  }

  SatResult check(llvm::ArrayRef<SymRef> Assumptions,
                  std::optional<std::pair<SymRef, SymRef>> Distinct = {}) {
    clearAnswer();
    LastAssumptions = z3::expr_vector(ZCtx);
    if (Error)
      return *Error;
    try {
      for (SymRef A : Assumptions) {
        if (!translate(A))
          return *Error;
        LastAssumptions.push_back(predicate(*Nodes[A.index()]));
      }
      if (Distinct) {
        const auto [A, B] = *Distinct;
        if (!translate(A) || !translate(B))
          return *Error;
        if (Ctx.width(A) != Ctx.width(B)) {
          fail(SatResult::Invalid);
          return SatResult::Invalid;
        }
        LastAssumptions.push_back(*Nodes[A.index()] != *Nodes[B.index()]);
      }
      const z3::check_result Result = Solver.check(LastAssumptions);
      if (Result == z3::unknown) {
        Reason = Solver.reason_unknown();
        return SatResult::Unknown;
      }
      if (Result == z3::unsat) {
        const z3::expr_vector Core = Solver.unsat_core();
        for (size_t I = 0; I < Assumptions.size(); ++I)
          for (const z3::expr &C : Core)
            if (z3::eq(C, LastAssumptions[I])) {
              Failed.push_back(Assumptions[I]);
              break;
            }
        return SatResult::Unsat;
      }
      if (Options.BuildModel) {
        const z3::model Assignment = Solver.get_model();
        for (const auto &[Id, Index] : Variables) {
          std::string Decimal;
          if (!Assignment.eval(*Nodes[Index], true).is_numeral(Decimal)) {
            fail(SatResult::Unknown,
                 "Z3 returned a non-numeral bitvector model");
            return SatResult::Unknown;
          }
          Model.set(Id, llvm::APInt(Ctx.varInfo(Id).Width, Decimal, 10));
        }
      }
      return SatResult::Sat;
    } catch (const z3::exception &E) {
      fail(SatResult::Unknown, E.msg());
      return SatResult::Unknown;
    }
  }

  std::string dumpSMT2() {
    if (Error)
      return {};
    try {
      z3::solver Snapshot(ZCtx);
      Snapshot.add(Solver.assertions());
      Snapshot.add(LastAssumptions);
      return Snapshot.to_smt2();
    } catch (const z3::exception &) {
      return {};
    }
  }
#else
  Impl(SymContext &Ctx, const Z3SolverOptions &Options)
      : Ctx(Ctx), Options(Options), Reason("Z3 backend is unavailable") {}

  bool assertion(SymRef A, std::optional<SymRef> B, bool) {
    if (!validRef(Ctx, A) ||
        (B && (!validRef(Ctx, *B) || Ctx.width(A) != Ctx.width(*B))))
      return fail(SatResult::Invalid);
    if (!Error)
      fail(SatResult::Unknown, "Z3 backend is unavailable");
    return false;
  }

  SatResult check(llvm::ArrayRef<SymRef> Assumptions,
                  std::optional<std::pair<SymRef, SymRef>> Distinct = {}) {
    for (SymRef A : Assumptions)
      if (!validRef(Ctx, A)) {
        fail(SatResult::Invalid);
        return SatResult::Invalid;
      }
    if (Distinct) {
      const auto [A, B] = *Distinct;
      if (!validRef(Ctx, A) || !validRef(Ctx, B) ||
          Ctx.width(A) != Ctx.width(B)) {
        fail(SatResult::Invalid);
        return SatResult::Invalid;
      }
    }
    return Error.value_or(SatResult::Unknown);
  }

  std::string dumpSMT2() { return {}; }
#endif
};

Z3Solver::Z3Solver(SymContext &Ctx, const Z3SolverOptions &Options)
    : P(std::make_unique<Impl>(Ctx, Options)) {}

Z3Solver::~Z3Solver() = default;

bool Z3Solver::available() {
#if defined(NEVERD_HAS_Z3)
  return true;
#else
  return false;
#endif
}

std::string Z3Solver::version() {
#if defined(NEVERD_HAS_Z3)
  return Z3_get_full_version();
#else
  return {};
#endif
}

bool Z3Solver::assertTrue(SymRef Pred) {
  return P->assertion(Pred, std::nullopt, true);
}

bool Z3Solver::assertFalse(SymRef Pred) {
  return P->assertion(Pred, std::nullopt, false);
}

bool Z3Solver::assertEqual(SymRef A, SymRef B) {
  return P->assertion(A, B, true);
}

bool Z3Solver::assertDistinct(SymRef A, SymRef B) {
  return P->assertion(A, B, false);
}

SatResult Z3Solver::check() { return check({}); }

SatResult Z3Solver::check(llvm::ArrayRef<SymRef> Assumptions) {
  return P->check(Assumptions);
}

SatResult Z3Solver::checkDistinct(SymRef A, SymRef B) {
  return P->check({}, std::make_pair(A, B));
}

const BitVectorModel &Z3Solver::model() const { return P->Model; }

llvm::ArrayRef<SymRef> Z3Solver::failedAssumptions() const { return P->Failed; }

bool Z3Solver::ok() const { return available() && !P->Error; }

const std::string &Z3Solver::reasonUnknown() const { return P->Reason; }

std::string Z3Solver::dumpSMT2() const { return P->dumpSMT2(); }

SatResult z3CheckSat(SymContext &Ctx, SymRef Pred, BitVectorModel *Model,
                     const Z3SolverOptions &Options) {
  if (Model)
    Model->clear();
  Z3Solver Solver(Ctx, Options);
  Solver.assertTrue(Pred);
  const SatResult Result = Solver.check();
  if (Result == SatResult::Sat && Model)
    *Model = Solver.model();
  return Result;
}

EquivResult z3CheckEqual(SymContext &Ctx, SymRef A, SymRef B,
                         BitVectorModel *Counterexample,
                         const Z3SolverOptions &Options) {
  if (Counterexample)
    Counterexample->clear();
  Z3Solver Solver(Ctx, Options);
  Solver.assertDistinct(A, B);
  switch (Solver.check()) {
  case SatResult::Sat:
    if (Counterexample)
      *Counterexample = Solver.model();
    return EquivResult::Different;
  case SatResult::Unsat:
    return EquivResult::Equal;
  case SatResult::Unknown:
    return EquivResult::Unknown;
  case SatResult::Invalid:
    return EquivResult::Invalid;
  }
  return EquivResult::Invalid;
}

} // namespace neverd::solver

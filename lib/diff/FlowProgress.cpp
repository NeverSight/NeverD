//===- FlowProgress.cpp - Bounded progress proofs for scalar SSA ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A defined iteration advances a nowrap counter in a finite domain.
/// Exhaustion produces poison consumed directly by the exit branch (UB),
/// which satisfies LLVM willreturn. No fixed choice for undef is assumed.
//===----------------------------------------------------------------------===//

#include "FlowProgress.h"

#include "FlowInvariantBounds.h"
#include "FlowParameterViews.h"
#include "LLVMFlowSemantics.h"

#include "neverd/diff/Control.h"
#include "neverd/diff/Limits.h"
#include "neverd/diff/Program.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Operator.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace neverd::diff {
llvm::StringRef flowCounterProofName(FlowCounterProof Proof) {
  switch (Proof) {
  case FlowCounterProof::None:
    return "none";
  case FlowCounterProof::NoWrap:
    return "nowrap";
  case FlowCounterProof::Modular:
    return "modular";
  case FlowCounterProof::ParameterModular:
    return "parameter_modular";
  case FlowCounterProof::InvariantModular:
    return "invariant_modular";
  }
  return "invalid";
}
llvm::StringRef flowProgressName(FlowProgressKind Kind) {
  switch (Kind) {
  case FlowProgressKind::Unproven:
    return "unproven";
  case FlowProgressKind::Acyclic:
    return "acyclic";
  case FlowProgressKind::CounterLoops:
    return "counter_loops";
  }
  return "invalid";
}
namespace {
enum CounterDomain : unsigned {
  SignedIncreasing = 1,
  SignedDecreasing = 2,
  UnsignedIncreasing = 4,
  UnsignedDecreasing = 8,
  AllCounterDomains = SignedIncreasing | SignedDecreasing | UnsignedIncreasing |
                      UnsignedDecreasing
};
struct CounterStep {
  FlowLoopProgress Loop;
  unsigned Domains = 0;
  unsigned Width = 0;
  uint64_t Delta = 0;
};

std::optional<CounterStep>
counterUpdate(const DataFlowGraph &G, uint32_t Block, uint32_t Header,
              uint32_t Update, uint32_t Compare, llvm::StringRef Contract,
              llvm::function_ref<void(uint64_t)> Charge,
              bool CheckIncoming = true, bool Modular = false) {
  const auto &U = G.Nodes[Update];
  Charge(1 + U.Semantics.size());
  // The update is local to the header or its represented continuation block.
  // Legacy guards consume it before the backedge. A v4 guard may consume the
  // current counter first: new poison then crosses one backedge through the
  // exact header PHI and prevents another defined guarded continuation.
  if ((U.Block != Block && U.Block != Header) ||
      (U.Opcode != "add" && U.Opcode != "sub") || U.Inputs.size() != 2)
    return {};
  const auto *Phi = &U.Inputs[0], *Step = &U.Inputs[1];
  if (U.Opcode == "add" && Phi->Type == FlowOperand::Kind::Constant)
    std::swap(Phi, Step);
  if (Phi->Type != FlowOperand::Kind::Node ||
      Step->Type != FlowOperand::Kind::Constant)
    return {};
  // Exact suffixes, not a search through arbitrary source string attributes.
  unsigned Width = 0, Flags = 0;
  llvm::StringRef Type(Step->Semantics), Suffix(U.Semantics);
  if (!Type.consume_front("i") || Type.getAsInteger(10, Width) || !Width ||
      Width > std::numeric_limits<uint64_t>::digits ||
      !Suffix.consume_front(Contract) ||
      !Suffix.consume_front(";result=" + Step->Semantics + ";flags=") ||
      Suffix.getAsInteger(10, Flags) || Suffix != std::to_string(Flags))
    return {};
  constexpr unsigned NSW = llvm::OverflowingBinaryOperator::NoSignedWrap;
  constexpr unsigned NUW = llvm::OverflowingBinaryOperator::NoUnsignedWrap;
  // Every nonzero fixed step exhausts its finite nowrap domain, even when
  // the exit value is skipped. No signed conversion or iteration count is
  // needed, including the i64 minimum signed step and maximum unsigned step.
  if ((Flags & ~(NSW | NUW)) ||
      (!Modular && (!Step->Value || !(Flags & (NSW | NUW)))))
    return {};
  const auto Counter = Phi->Node;
  const auto &P = G.Nodes[Counter];
  Charge(1 + P.Semantics.size());
  if (P.Block != Header || !P.Phi || P.Opcode != "phi" || P.Inputs.size() < 2 ||
      P.Semantics != Contract.str() + ";result=" + Step->Semantics + ";flags=0")
    return {};
  unsigned Domains = 0;
  if (Step->Value && (Flags & NSW)) {
    const bool Negative = (Step->Value >> (Width - 1)) & 1;
    Domains |=
        (U.Opcode == "sub") == Negative ? SignedIncreasing : SignedDecreasing;
  }
  if (Step->Value && (Flags & NUW))
    Domains |= U.Opcode == "sub" ? UnsignedDecreasing : UnsignedIncreasing;
  CounterStep Result{{Block, Counter, Update, Compare}, Domains};
  if (Modular) {
    Result.Width = Width;
    const auto Mask = std::numeric_limits<uint64_t>::max() >>
                      (std::numeric_limits<uint64_t>::digits - Width);
    Result.Delta =
        (U.Opcode == "sub" ? uint64_t(0) - Step->Value : Step->Value) & Mask;
  }
  // v4 checks the complete header PHI once for a whole group, including
  // every physical backedge. Published modes retain their original scan.
  if (!CheckIncoming)
    return Result;
  unsigned SelfInputs = 0;
  for (const auto &Input : P.Inputs) {
    Charge(1);
    if (Input.Predecessor == NoIndex)
      return {};
    if (Input.Predecessor == Block) {
      if (Input.Type != FlowOperand::Kind::Node || Input.Node != Update)
        return {};
      ++SelfInputs;
    }
  }
  if (SelfInputs != 1)
    return {};
  return Result;
}

std::optional<uint32_t>
guardCompare(const DataFlowGraph &G, uint32_t Block, uint32_t Target,
             llvm::StringRef Contract,
             llvm::function_ref<void(uint64_t)> Charge) {
  const auto &B = G.Blocks[Block];
  if (B.Successors.size() != 2 ||
      (B.Successors[0] == Target) == (B.Successors[1] == Target))
    return {};
  const auto &Branch = G.Nodes[B.Terminator];
  Charge(1 + Branch.Semantics.size());
  if (Branch.Opcode != "br" || Branch.Inputs.size() != 1 ||
      Branch.Inputs[0].Type != FlowOperand::Kind::Node ||
      Branch.Semantics != Contract.str() + ";result=i0;flags=0")
    return {};
  const auto Compare = Branch.Inputs[0].Node;
  const auto &Cmp = G.Nodes[Compare];
  Charge(1 + Cmp.Semantics.size());
  llvm::StringRef Suffix(Cmp.Semantics);
  unsigned Predicate = 0;
  if (Cmp.Block != Block || Cmp.Opcode != "icmp" || Cmp.Inputs.size() != 2 ||
      !Suffix.consume_front(Contract) ||
      !Suffix.consume_front(";result=i1;flags=0;predicate=") ||
      Suffix.getAsInteger(10, Predicate) ||
      Suffix != std::to_string(Predicate) ||
      Predicate < llvm::CmpInst::FIRST_ICMP_PREDICATE ||
      Predicate > llvm::CmpInst::LAST_ICMP_PREDICATE)
    return {};
  return Compare;
}

std::optional<FlowLoopProgress>
counterLoop(const DataFlowGraph &G, uint32_t Block, uint32_t Header,
            llvm::StringRef Contract,
            llvm::function_ref<void(uint64_t)> Charge) {
  const auto Compare = guardCompare(G, Block, Header, Contract, Charge);
  if (!Compare)
    return {};
  const auto &Cmp = G.Nodes[*Compare];
  // Any direct scalar comparison propagates poison from either operand to
  // the branch. Progress does not depend on its predicate or exit polarity.
  for (const auto &Input : Cmp.Inputs) {
    Charge(1);
    if (Input.Type == FlowOperand::Kind::Node)
      if (auto L = counterUpdate(G, Block, Header, Input.Node, *Compare,
                                 Contract, Charge))
        return L->Loop;
  }
  return {};
}

struct FlowComponents {
  std::vector<uint32_t> Id, Header, Latch, Backedges;
  std::vector<uint8_t> MultipleEntries;
};

/// Iterative Tarjan traversal: each block/edge is visited a bounded number of
/// times, including unreachable components. Compute the unique header and its
/// internal predecessor inventory before scanning any counter PHI. Otherwise
/// many rejected latches could repeatedly scan a large externally seeded PHI.
FlowComponents components(const DataFlowGraph &G,
                          llvm::function_ref<void(uint64_t)> Charge,
                          llvm::ArrayRef<uint32_t> Cut = {}) {
  const auto Size = G.Blocks.size();
  Charge(10 * uint64_t(Size));
  FlowComponents C{std::vector<uint32_t>(Size, NoIndex),
                   std::vector<uint32_t>(Size, NoIndex),
                   std::vector<uint32_t>(Size, NoIndex),
                   std::vector<uint32_t>(Size), std::vector<uint8_t>(Size)};
  std::vector<uint32_t> Order(Size, NoIndex), Low(Size), Pending;
  std::vector<uint8_t> OnStack(Size);
  struct Frame {
    uint32_t Block;
    size_t Next;
  };
  std::vector<Frame> Stack;
  uint32_t Next = 0, Count = 0;
  const auto Visit = [&](uint32_t Block) {
    Charge(1);
    Order[Block] = Low[Block] = Next++;
    Pending.push_back(Block);
    OnStack[Block] = true;
    Stack.push_back({Block, 0});
  };
  for (uint32_t Root = 0; Root < Size; ++Root) {
    Charge(1);
    if (Order[Root] != NoIndex)
      continue;
    Visit(Root);
    while (!Stack.empty()) {
      Charge(1);
      auto &Top = Stack.back();
      const auto Block = Top.Block;
      if (Top.Next < G.Blocks[Block].Successors.size()) {
        const auto To = G.Blocks[Block].Successors[Top.Next++];
        if (!Cut.empty() && Cut[Block] == To)
          continue;
        if (Order[To] == NoIndex)
          Visit(To);
        else if (OnStack[To])
          Low[Block] = std::min(Low[Block], Order[To]);
        continue;
      }
      if (Low[Block] == Order[Block]) {
        uint32_t Member;
        do {
          Charge(1);
          Member = Pending.back();
          Pending.pop_back();
          OnStack[Member] = false;
          C.Id[Member] = Count;
        } while (Member != Block);
        ++Count;
      }
      Stack.pop_back();
      if (!Stack.empty()) {
        const auto Parent = Stack.back().Block;
        Low[Parent] = std::min(Low[Parent], Low[Block]);
      }
    }
  }
  const auto Entry = [&](uint32_t Block) {
    auto &Header = C.Header[C.Id[Block]];
    if (Header != NoIndex && Header != Block)
      C.MultipleEntries[C.Id[Block]] = true;
    Header = Block;
  };
  if (G.EntryBlock != NoIndex)
    Entry(G.EntryBlock);
  // Keep original entries, including cut edges. A residual component must
  // never acquire a proof by hiding an entry from an earlier round.
  for (uint32_t From = 0; From < Size; ++From) {
    Charge(1);
    for (const auto To : G.Blocks[From].Successors) {
      Charge(1);
      if (C.Id[From] != C.Id[To])
        Entry(To);
    }
  }
  for (uint32_t From = 0; From < Size; ++From) {
    Charge(1);
    const auto Id = C.Id[From];
    for (const auto To : G.Blocks[From].Successors) {
      Charge(1);
      if (!Cut.empty() && Cut[From] == To)
        continue;
      if (C.Id[To] == Id && To == C.Header[Id]) {
        C.Latch[Id] = From;
        ++C.Backedges[Id];
      }
    }
  }
  return C;
}

/// Compare (counter + A) with (counter + B). For X = counter + A and nonzero
/// D = B - A, X < X + D holds exactly before the wrap at -D. Signed comparison
/// rotates both operands by the sign bit. All arithmetic uses the source width.
llvm::ConstantRange sameCounterRegion(llvm::CmpInst::Predicate Pred,
                                      llvm::APInt A, const llvm::APInt &B) {
  const auto Width = A.getBitWidth();
  const auto D = B - A;
  if (D.isZero())
    return llvm::CmpInst::isTrueWhenEqual(Pred)
               ? llvm::ConstantRange::getFull(Width)
               : llvm::ConstantRange::getEmpty(Width);
  if (Pred == llvm::CmpInst::ICMP_EQ)
    return llvm::ConstantRange::getEmpty(Width);
  if (Pred == llvm::CmpInst::ICMP_NE)
    return llvm::ConstantRange::getFull(Width);
  if (llvm::CmpInst::isSigned(Pred))
    A += llvm::APInt::getSignMask(Width);
  const bool Less =
      Pred == llvm::CmpInst::ICMP_ULT || Pred == llvm::CmpInst::ICMP_ULE ||
      Pred == llvm::CmpInst::ICMP_SLT || Pred == llvm::CmpInst::ICMP_SLE;
  return llvm::ConstantRange::makeExactICmpRegion(
             Less ? llvm::CmpInst::ICMP_ULT : llvm::CmpInst::ICMP_UGE, -D)
      .subtract(A);
}

/// The shared nonzero delta visits exactly one residue class modulo its gcd
/// with 2^Width. Every residue class must contain a value excluded by ALL
/// continuation guards. Individual ranges may conservatively include poison
/// cases; their union therefore never invents a common exit.
/// This does not freeze undef: a defined branch must continue for every
/// contributing undef choice, including a coherent initial value following
/// the fixed orbit. At its common exit the branch exits or becomes undefined.
/// Two undef observations may vary independently; the coherent same-counter
/// choice is included, without assuming those uses are correlated in LLVM.
/// Exact recurrence forwarding and final residual acyclicity are essential.
bool modularExitCoverage(llvm::ArrayRef<llvm::ConstantRange> Continued,
                         unsigned Width, uint64_t Delta,
                         llvm::function_ref<void(uint64_t)> Charge) {
  if (!Delta)
    return false;
  struct Interval {
    llvm::APInt Begin, End;
  };
  // The extra bit represents the half-open endpoint 2^64 without overflow.
  const auto Limit = llvm::APInt::getOneBitSet(Width + 1, Width);
  const auto Period =
      llvm::APInt::getOneBitSet(Width + 1, std::countr_zero(Delta));
  const llvm::APInt Zero(Width + 1, 0);
  std::vector<Interval> Intervals, Residues;
  for (const auto &Range : Continued) {
    Charge(1);
    if (Range.isEmptySet())
      continue;
    if (Range.isFullSet())
      return false;
    auto Begin = Range.getLower().zext(Width + 1);
    auto End = Range.getUpper().zext(Width + 1);
    if (Begin.ult(End))
      Intervals.push_back({std::move(Begin), std::move(End)});
    else {
      Intervals.push_back({std::move(Begin), Limit});
      if (!End.isZero())
        Intervals.push_back({Zero, std::move(End)});
    }
  }
  const auto Sort = [&](std::vector<Interval> &Values) {
    std::sort(Values.begin(), Values.end(),
              [&](const Interval &A, const Interval &B) {
                Charge(1);
                return A.Begin.ult(B.Begin);
              });
  };
  Sort(Intervals);
  const auto ProjectExit = [&](const llvm::APInt &Begin,
                               const llvm::APInt &End) {
    Charge(1);
    const auto Length = End - Begin;
    if (Length.uge(Period))
      return true;
    if (Length.isZero())
      return false;
    auto Low = Begin & (Period - 1);
    auto High = Low + Length;
    if (High.ule(Period))
      Residues.push_back({std::move(Low), std::move(High)});
    else {
      Residues.push_back({std::move(Low), Period});
      Residues.push_back({Zero, High - Period});
    }
    return false;
  };
  auto Covered = Zero;
  for (const auto &I : Intervals) {
    Charge(1);
    if (I.Begin.ugt(Covered) && ProjectExit(Covered, I.Begin))
      return true;
    if (I.End.ugt(Covered))
      Covered = I.End;
  }
  if (ProjectExit(Covered, Limit))
    return true;
  Sort(Residues);
  Covered = Zero;
  for (const auto &I : Residues) {
    Charge(1);
    if (I.Begin.ugt(Covered))
      return false;
    if (I.End.ugt(Covered))
      Covered = I.End;
  }
  return Covered == Period;
}

/// v4-v7 group all paths that advance one header counter. Original predecessor
/// inventories and scratch are shared across candidates/components. A funnel
/// has one unconditional physical backedge and several consuming guards; its
/// witnesses belong to those guards, not duplicate rows for the same latch.
class BranchProof {
  struct Guard {
    uint32_t Block, Compare, Target;
  };
  struct Backedge {
    uint32_t Latch;
    size_t Begin, End;
    bool Funnel;
  };
  const DataFlowGraph &G;
  llvm::StringRef Contract;
  llvm::function_ref<void(uint64_t)> Charge;
  const bool Modular, ParameterBound, InvariantBound;
  std::optional<FlowInvariantBounds> Invariants;
  std::vector<std::vector<uint32_t>> Predecessors;
  std::vector<uint32_t> HeaderValues, ArmValues, Owners;
  std::vector<uint8_t> Seen;
  std::vector<Backedge> Backedges;
  std::vector<Guard> Guards;
  std::vector<FlowLoopProgress> Trial;

  bool indexPhi(uint32_t Node, uint32_t Block, llvm::StringRef Semantics,
                std::vector<uint32_t> &Values) {
    const auto &P = G.Nodes[Node];
    Charge(1 + P.Semantics.size());
    if (P.Block != Block || !P.Phi || P.Opcode != "phi" ||
        P.Semantics != Semantics ||
        P.Inputs.size() != Predecessors[Block].size())
      return false;
    for (const auto From : Predecessors[Block]) {
      Charge(1);
      Owners[From] = Node;
      Seen[From] = false;
    }
    for (const auto &Input : P.Inputs) {
      Charge(1);
      if (Input.Predecessor >= G.Blocks.size() ||
          Owners[Input.Predecessor] != Node || Seen[Input.Predecessor])
        return false;
      Seen[Input.Predecessor] = true;
      Values[Input.Predecessor] =
          Input.Type == FlowOperand::Kind::Node ? Input.Node : NoIndex;
    }
    // Structural graph validation alone does not require complete PHI arms.
    // Check every original edge, including duplicates and already cut edges.
    for (const auto From : Predecessors[Block]) {
      Charge(1);
      if (!Seen[From])
        return false;
    }
    return true;
  }

  bool addGuard(uint32_t Block, uint32_t Target, const FlowComponents &C,
                uint32_t Id) {
    Charge(1 + G.Blocks[Block].Successors.size());
    if (C.Id[Block] != Id)
      return false;
    for (const auto To : G.Blocks[Block].Successors)
      if (To != Target && C.Id[To] == Id)
        return false;
    const auto Compare = guardCompare(G, Block, Target, Contract, Charge);
    if (!Compare)
      return false;
    Guards.push_back({Block, *Compare, Target});
    return true;
  }

  bool inventory(uint32_t Header, const FlowComponents &C,
                 llvm::ArrayRef<uint32_t> Cut) {
    Backedges.clear();
    Guards.clear();
    const auto Id = C.Id[Header];
    for (const auto Latch : Predecessors[Header]) {
      Charge(1);
      if (C.Id[Latch] != Id || Cut[Latch] == Header)
        continue;
      const auto &B = G.Blocks[Latch];
      const auto Begin = Guards.size();
      const bool Funnel = B.Successors.size() == 1;
      if (Funnel) {
        const auto &Branch = G.Nodes[B.Terminator];
        Charge(1 + Branch.Semantics.size());
        if (Branch.Opcode != "br" || !Branch.Inputs.empty() ||
            Branch.Semantics != Contract.str() + ";result=i0;flags=0" ||
            Predecessors[Latch].empty())
          return false;
        for (const auto From : Predecessors[Latch])
          if (!addGuard(From, Latch, C, Id))
            return false;
      } else if (!addGuard(Latch, Header, C, Id)) {
        return false;
      }
      Backedges.push_back({Latch, Begin, Guards.size(), Funnel});
    }
    return !Backedges.empty() && Backedges.size() == C.Backedges[Id];
  }

  bool consumes(const Guard &Guard, uint32_t Counter, uint32_t Update) {
    for (const auto &Input : G.Nodes[Guard.Compare].Inputs) {
      Charge(1);
      if (Input.Type == FlowOperand::Kind::Node &&
          (Input.Node == Counter ||
           (Input.Node == Update &&
            (G.Nodes[Update].Block == Guard.Block ||
             G.Nodes[Update].Block == G.Nodes[Counter].Block))))
        return true;
    }
    return false;
  }

  bool candidate(uint32_t Counter, uint32_t Header) {
    Trial.clear();
    if (!indexPhi(Counter, Header, G.Nodes[Counter].Semantics, HeaderValues))
      return false;
    unsigned Domains = AllCounterDomains;
    for (const auto &Edge : Backedges) {
      Charge(1);
      const auto Incoming = HeaderValues[Edge.Latch];
      if (Incoming == NoIndex)
        return false;
      const bool Selected = Edge.Funnel && G.Nodes[Incoming].Phi &&
                            G.Nodes[Incoming].Block == Edge.Latch;
      if (Selected && !indexPhi(Incoming, Edge.Latch,
                                G.Nodes[Counter].Semantics, ArmValues))
        return false;
      std::optional<CounterStep> Common;
      if (!Selected) {
        Common = counterUpdate(G, Edge.Latch, Header, Incoming, NoIndex,
                               Contract, Charge, false);
        if (!Common || Common->Loop.Counter != Counter)
          return false;
        Domains &= Common->Domains;
      }
      for (size_t I = Edge.Begin; I < Edge.End; ++I) {
        const auto &Guard = Guards[I];
        Charge(1);
        const auto Update = Selected ? ArmValues[Guard.Block] : Incoming;
        if (Update == NoIndex)
          return false;
        if (Selected) {
          const auto Step =
              counterUpdate(G, Guard.Block, Header, Update, Guard.Compare,
                            Contract, Charge, false);
          if (!Step || Step->Loop.Counter != Counter)
            return false;
          Domains &= Step->Domains;
        }
        if (!Domains || !consumes(Guard, Counter, Update))
          return false;
        Trial.push_back(
            {Guard.Block, Counter, Update, Guard.Compare, Header, Edge.Latch});
      }
    }
    return true;
  }

  struct Observation {
    unsigned Operand;
    uint64_t Offset;
  };
  std::optional<uint64_t> observationOffset(const FlowOperand &Input,
                                            const Guard &Guard,
                                            uint32_t Counter, uint32_t Header) {
    Charge(1);
    if (Input.Type != FlowOperand::Kind::Node)
      return {};
    if (Input.Node == Counter)
      return 0;
    const auto Step =
        counterUpdate(G, Guard.Block, Header, Input.Node, Guard.Compare,
                      Contract, Charge, false, true);
    if (Step && Step->Loop.Counter == Counter)
      return Step->Delta;
    return {};
  }
  std::optional<Observation> observation(const Guard &Guard, uint32_t Counter,
                                         uint32_t Header) {
    const auto &Inputs = G.Nodes[Guard.Compare].Inputs;
    for (unsigned I = 0; I < Inputs.size(); ++I)
      if (const auto Offset =
              observationOffset(Inputs[I], Guard, Counter, Header))
        return Observation{I, *Offset};
    return {};
  }

  struct ParameterObservation {
    uint64_t Parameter, Offset;
  };
  std::optional<ParameterObservation>
  parameterObservation(const FlowOperand &Input, unsigned Width) {
    const auto Parameter = [&](const FlowOperand &Value) {
      Charge(1 + Value.Semantics.size());
      return Value.Type == FlowOperand::Kind::Parameter &&
             Value.Value < G.ParameterViews.size() &&
             G.ParameterViews[Value.Value].ViewBits == Width &&
             Value.Semantics ==
                 parameterViewSemantics(G.ParameterViews[Value.Value]);
    };
    if (Parameter(Input))
      return ParameterObservation{Input.Value, 0};
    if (Input.Type != FlowOperand::Kind::Node)
      return {};
    const auto &N = G.Nodes[Input.Node];
    Charge(1 + N.Semantics.size());
    if ((N.Opcode != "add" && N.Opcode != "sub") || N.Inputs.size() != 2)
      return {};
    const auto *Base = &N.Inputs[0], *Offset = &N.Inputs[1];
    if (N.Opcode == "add" && Base->Type == FlowOperand::Kind::Constant)
      std::swap(Base, Offset);
    const auto Type = "i" + std::to_string(Width);
    Charge(1 + Offset->Semantics.size());
    if (!Parameter(*Base) || Offset->Type != FlowOperand::Kind::Constant ||
        Offset->Semantics != Type)
      return {};
    llvm::StringRef Suffix(N.Semantics);
    unsigned Flags = 0;
    constexpr unsigned WrapFlags =
        llvm::OverflowingBinaryOperator::NoSignedWrap |
        llvm::OverflowingBinaryOperator::NoUnsignedWrap;
    if (!Suffix.consume_front(Contract) ||
        !Suffix.consume_front(";result=" + Type + ";flags=") ||
        Suffix.getAsInteger(10, Flags) || Suffix != std::to_string(Flags) ||
        (Flags & ~WrapFlags))
      return {};
    // Pure arithmetic on the same parameter has a coherent constant value
    // throughout the recurrence, wherever evaluated. A freeze/PHI/select is
    // deliberately not traversed: it could pick a new value each iteration.
    // Ignoring overflow flags only adds concrete continuation possibilities;
    // actual poison flows through the comparison into the direct branch.
    const auto Bits = llvm::APInt(Width, Offset->Value);
    return ParameterObservation{
        Base->Value, (N.Opcode == "sub" ? -Bits : Bits).getZExtValue()};
  }

  bool modularCandidate(uint32_t Counter, uint32_t Header,
                        bool InvariantFallback = false) {
    Trial.clear();
    if (!indexPhi(Counter, Header, G.Nodes[Counter].Semantics, HeaderValues))
      return false;
    unsigned Domains = AllCounterDomains, Width = 0;
    uint64_t Delta = 0;
    bool Uniform = true;
    std::vector<Observation> Observations;
    const auto Accumulate = [&](const CounterStep &Step) {
      Domains &= Step.Domains;
      if (!Width) {
        Width = Step.Width;
        Delta = Step.Delta;
      } else {
        Uniform &= Width == Step.Width && Delta == Step.Delta;
      }
    };
    for (const auto &Edge : Backedges) {
      Charge(1);
      const auto Incoming = HeaderValues[Edge.Latch];
      if (Incoming == NoIndex)
        return false;
      const bool Selected = Edge.Funnel && G.Nodes[Incoming].Phi &&
                            G.Nodes[Incoming].Block == Edge.Latch;
      if (Selected && !indexPhi(Incoming, Edge.Latch,
                                G.Nodes[Counter].Semantics, ArmValues))
        return false;
      if (!Selected) {
        const auto Step = counterUpdate(G, Edge.Latch, Header, Incoming,
                                        NoIndex, Contract, Charge, false, true);
        if (!Step || Step->Loop.Counter != Counter)
          return false;
        Accumulate(*Step);
      }
      for (size_t I = Edge.Begin; I < Edge.End; ++I) {
        Charge(1);
        const auto &Guard = Guards[I];
        const auto Update = Selected ? ArmValues[Guard.Block] : Incoming;
        if (Update == NoIndex)
          return false;
        if (Selected) {
          const auto Step =
              counterUpdate(G, Guard.Block, Header, Update, Guard.Compare,
                            Contract, Charge, false, true);
          if (!Step || Step->Loop.Counter != Counter)
            return false;
          Accumulate(*Step);
        }
        const auto Observed = observation(Guard, Counter, Header);
        if (!Observed)
          return false;
        Observations.push_back(*Observed);
        Trial.push_back({Guard.Block, Counter, Update, Guard.Compare, Header,
                         Edge.Latch, FlowCounterProof::NoWrap});
      }
    }
    // NoWrap needs strict poison propagation, but no bound or equal-step
    // restrictions. Only modular fallback requires concrete counter regions.
    if (Domains)
      return true;
    if (!Uniform || !Delta)
      return false;
    std::vector<llvm::ConstantRange> Continued;
    std::optional<uint64_t> Parameter;
    std::optional<uint32_t> Basis;
    for (size_t I = 0; I < Guards.size(); ++I) {
      const auto &Guard = Guards[I];
      const auto &Observed = Observations[I];
      const auto &Cmp = G.Nodes[Guard.Compare];
      const auto &Bound = Cmp.Inputs[1 - Observed.Operand];
      Charge(1 + Cmp.Semantics.size() + Bound.Semantics.size());
      llvm::StringRef Suffix(Cmp.Semantics);
      unsigned Predicate = 0;
      if (!Suffix.consume_front(Contract) ||
          !Suffix.consume_front(";result=i1;flags=0;predicate=") ||
          Suffix.getAsInteger(10, Predicate))
        return false;
      auto Pred = llvm::CmpInst::Predicate(Predicate);
      if (Observed.Operand)
        Pred = llvm::CmpInst::getSwappedPredicate(Pred);
      if (G.Blocks[Guard.Block].Successors[0] != Guard.Target)
        Pred = llvm::CmpInst::getInversePredicate(Pred);
      if (InvariantFallback) {
        if (!llvm::CmpInst::isEquality(Pred))
          return false;
        if (!Invariants)
          Invariants.emplace(G, Contract, Charge);
        const auto Relative = Invariants->observe(Bound);
        if (!Relative || (Basis && *Basis != Relative->Node))
          return false;
        Basis = Relative->Node;
        // X = counter - B for the same input-derived basis B in every guard.
        // Coherent fixed leaf choices yield a fixed root or UB/poison; select
        // may mask poison. The exact relative exits exclude infinite defined
        // continuation without freezing undef or assuming correlated uses.
        Continued.push_back(llvm::ConstantRange::makeExactICmpRegion(
            Pred, llvm::APInt(Width, Relative->Offset) -
                      llvm::APInt(Width, Observed.Offset)));
      } else if (Bound.Type == FlowOperand::Kind::Constant &&
                 Bound.Semantics == "i" + std::to_string(Width)) {
        if (Parameter)
          return false;
        Continued.push_back(llvm::ConstantRange::makeExactICmpRegion(
                                Pred, llvm::APInt(Width, Bound.Value))
                                .subtract(llvm::APInt(Width, Observed.Offset)));
      } else if (const auto Other =
                     observationOffset(Bound, Guard, Counter, Header)) {
        if (Parameter)
          return false;
        Continued.push_back(
            sameCounterRegion(Pred, llvm::APInt(Width, Observed.Offset),
                              llvm::APInt(Width, *Other)));
      } else if (ParameterBound && llvm::CmpInst::isEquality(Pred)) {
        const auto Relative = parameterObservation(Bound, Width);
        if (!Relative || (I && !Parameter) ||
            (Parameter && *Parameter != Relative->Parameter))
          return false;
        Parameter = Relative->Parameter;
        // Let X = counter - parameter. Equality (counter+A) == (parameter+B)
        // is X == B-A, even across wraparound. Every continuation guard uses
        // this same coordinate system. The fixed delta gives the same orbit
        // residues as in v5, for every concrete parameter and initial seed.
        // No undef is frozen: coherent choices are included among all choices
        // that a defined continuation must satisfy. A common exit is therefore
        // false or undefined; a varying undef bound cannot force continuation.
        Continued.push_back(llvm::ConstantRange::makeExactICmpRegion(
            Pred, llvm::APInt(Width, Relative->Offset) -
                      llvm::APInt(Width, Observed.Offset)));
      } else
        return false;
    }
    if (!modularExitCoverage(Continued, Width, Delta, Charge))
      return false;
    for (auto &L : Trial) {
      Charge(1);
      L.Proof = Basis       ? FlowCounterProof::InvariantModular
                : Parameter ? FlowCounterProof::ParameterModular
                            : FlowCounterProof::Modular;
    }
    return true;
  }

public:
  BranchProof(const DataFlowGraph &Graph, llvm::StringRef Contract,
              llvm::function_ref<void(uint64_t)> Charge, bool Modular,
              bool ParameterBound, bool InvariantBound)
      : G(Graph), Contract(Contract), Charge(Charge), Modular(Modular),
        ParameterBound(ParameterBound), InvariantBound(InvariantBound) {
    Charge(5 * uint64_t(G.Blocks.size()));
    Predecessors.resize(G.Blocks.size());
    HeaderValues.resize(G.Blocks.size(), NoIndex);
    ArmValues.resize(G.Blocks.size(), NoIndex);
    Owners.resize(G.Blocks.size(), NoIndex);
    Seen.resize(G.Blocks.size());
    for (uint32_t From = 0; From < G.Blocks.size(); ++From) {
      Charge(1);
      for (const auto To : G.Blocks[From].Successors) {
        Charge(1);
        Predecessors[To].push_back(From);
      }
    }
  }

  void proveRound(const FlowComponents &C, std::vector<uint32_t> &Cut,
                  FlowProgress &P, uint64_t Allowance) {
    for (uint32_t Header = 0; Header < G.Blocks.size(); ++Header) {
      Charge(1);
      const auto Id = C.Id[Header];
      if (C.Header[Id] != Header || C.MultipleEntries[Id] || !C.Backedges[Id] ||
          !inventory(Header, C, Cut))
        continue;
      const auto &First = Guards.front();
      std::array<uint32_t, 2> Candidates{NoIndex, NoIndex};
      size_t Count = 0;
      for (const auto &Input : G.Nodes[First.Compare].Inputs) {
        Charge(1);
        if (Input.Type != FlowOperand::Kind::Node)
          continue;
        auto Counter = NoIndex;
        if (G.Nodes[Input.Node].Phi && G.Nodes[Input.Node].Block == Header)
          Counter = Input.Node;
        else if (const auto Step = counterUpdate(
                     G, First.Block, Header, Input.Node, First.Compare,
                     Contract, Charge, false, Modular))
          Counter = Step->Loop.Counter;
        if (Counter != NoIndex && (!Count || Candidates[0] != Counter))
          Candidates[Count++] = Counter;
      }
      bool Proved = false;
      // Exhaust every legacy counter candidate first. A wrapping first
      // operand must not allocate/charge the invariant walk before a second
      // operand's existing nowrap proof. Legacy successes keep their exact
      // charge sequence, witness choice and minimum allowance.
      for (bool Fallback : {false, true}) {
        if (Fallback && (!InvariantBound || Proved))
          break;
        for (size_t I = 0; I < Count; ++I) {
          Charge(1);
          if (!(Modular ? modularCandidate(Candidates[I], Header, Fallback)
                        : candidate(Candidates[I], Header)))
            continue;
          Proved = true;
          break;
        }
      }
      if (Proved) {
        if (Trial.size() > Allowance - P.Loops.size())
          throw DataFlowInterrupted(ComparisonState::ResourceLimited);
        Charge(1 + Trial.size());
        P.Loops.insert(P.Loops.end(), Trial.begin(), Trial.end());
        ++P.LoopCount;
        P.BackedgeCount += uint32_t(Backedges.size());
        for (const auto &Edge : Backedges) {
          Charge(1);
          Cut[Edge.Latch] = Header;
        }
      }
    }
  }
};
} // namespace

FlowProgress proveFlowProgress(const DataFlowGraph &G, llvm::StringRef Contract,
                               const DataFlowOptions &Options,
                               llvm::function_ref<void(uint64_t)> Charge,
                               FlowProgressMode Mode) {
  if (auto Error = validateFiniteLLVMFlow(G, Contract, Charge))
    throw std::invalid_argument(*Error);
  FlowProgress P;
  P.SourceContract = P.ObservedContract = Contract.str();
  Charge(1 + Contract.size() * 2 + G.Blocks.size());
  std::vector<uint32_t> Cut(G.Blocks.size(), NoIndex);
  // The root and each witness occupy the same collection allowance as nodes.
  const uint64_t Retained = G.Nodes.size() + G.ParameterViews.size() + 1;
  if (Retained > Options.MaxNodes)
    throw DataFlowInterrupted(ComparisonState::ResourceLimited);
  const bool InvariantBound = Mode == FlowProgressMode::InvariantBound;
  const bool ParameterBound =
      Mode == FlowProgressMode::ParameterBound || InvariantBound;
  const bool Modular = Mode == FlowProgressMode::Modular || ParameterBound;
  const bool Branched = Mode == FlowProgressMode::Branched || Modular;
  const bool Nested = Mode == FlowProgressMode::Nested || Branched;
  std::optional<BranchProof> Branches;
  if (Branched)
    Branches.emplace(G, Contract, Charge, Modular, ParameterBound,
                     InvariantBound);
  do {
    const auto Previous = P.Loops.size();
    std::optional<FlowComponents> C;
    if (Mode != FlowProgressMode::SelfLoop)
      C = components(G, Charge,
                     Nested ? llvm::ArrayRef<uint32_t>(Cut)
                            : llvm::ArrayRef<uint32_t>());
    if (Branches)
      Branches->proveRound(*C, Cut, P, Options.MaxNodes - Retained);
    else
      for (uint32_t I = 0; I < G.Blocks.size(); ++I) {
        Charge(1 + G.Blocks[I].Successors.size());
        if (Nested && Cut[I] != NoIndex)
          continue;
        auto Header = I;
        if (C) {
          const auto Id = C->Id[I];
          Header = C->Header[Id];
          if (Header == NoIndex || C->MultipleEntries[Id] ||
              C->Backedges[Id] != 1 || C->Latch[Id] != I)
            continue;
          bool InternalExit = false;
          for (const auto To : G.Blocks[I].Successors)
            InternalExit |= To != Header && C->Id[To] == Id;
          if (InternalExit)
            continue;
        }
        if (auto L = counterLoop(G, I, Header, Contract, Charge)) {
          if (C) {
            bool Reset = false;
            for (const auto &Input : G.Nodes[L->Counter].Inputs) {
              Charge(1);
              Reset |= Input.Predecessor != I &&
                       C->Id[Input.Predecessor] == C->Id[I];
            }
            if (Reset)
              continue;
            L->Header = Header;
          }
          if (P.Loops.size() == Options.MaxNodes - Retained)
            throw DataFlowInterrupted(ComparisonState::ResourceLimited);
          P.Loops.push_back(*L);
          ++P.LoopCount;
          ++P.BackedgeCount;
          Cut[I] = Header;
        }
      }
    // A round only proves finite traversal of these backedges. An inner
    // cycle may still run forever; publish no progress until every residual
    // cycle is discharged. Each productive round cuts a new, distinct latch.
    if (!Nested || P.Loops.size() == Previous)
      break;
  } while (true);
  Charge(G.Blocks.size());
  std::vector<uint32_t> Degree(G.Blocks.size()), Ready;
  for (uint32_t I = 0; I < G.Blocks.size(); ++I)
    for (auto To : G.Blocks[I].Successors) {
      Charge(1);
      if (To != Cut[I])
        ++Degree[To];
    }
  for (uint32_t I = 0; I < Degree.size(); ++I) {
    Charge(1);
    if (!Degree[I])
      Ready.push_back(I);
  }
  for (size_t I = 0; I < Ready.size(); ++I) {
    Charge(1);
    const auto From = Ready[I];
    for (auto To : G.Blocks[From].Successors) {
      Charge(1);
      if (To != Cut[From] && !--Degree[To])
        Ready.push_back(To);
    }
  }
  if (Ready.size() != G.Blocks.size()) {
    P.Loops.clear();
    P.LoopCount = P.BackedgeCount = 0;
    return P;
  }
  if (Nested)
    std::sort(P.Loops.begin(), P.Loops.end(),
              [&](const FlowLoopProgress &A, const FlowLoopProgress &B) {
                Charge(1);
                return A.Block < B.Block;
              });
  P.Kind = P.Loops.empty() ? FlowProgressKind::Acyclic
                           : FlowProgressKind::CounterLoops;
  return P;
}

std::optional<std::string>
validateFlowProgress(const DataFlowGraph &G, const DataFlowOptions &Options,
                     llvm::function_ref<void(uint64_t)> Charge) {
  if (!hasProgressProof(G.Profile))
    return G.Progress
               ? std::optional<std::string>(
                     "progress evidence disagrees with the graph profile")
               : std::nullopt;
  if (!G.Progress)
    return "progress profile has no source contract evidence";
  const auto &P = *G.Progress;
  Charge(1 + P.SourceContract.size() + P.ObservedContract.size() +
         P.Loops.size());
  if (G.retainedItems() > Options.MaxNodes || P.SourceContract.empty() ||
      P.ObservedContract.empty() ||
      P.SourceContract.size() > limits::diff::kMaxFlowSemanticsBytes ||
      P.ObservedContract.size() > limits::diff::kMaxFlowSemanticsBytes ||
      (P.Kind == FlowProgressKind::Unproven &&
       P.SourceContract != P.ObservedContract))
    return "progress evidence exceeds its allowance or changes an unproved "
           "contract";
  try {
    const auto Derived = proveFlowProgress(G, P.ObservedContract, Options,
                                           Charge, flowProgressMode(G.Profile));
    if (Derived.Kind != P.Kind || Derived.Loops != P.Loops ||
        Derived.LoopCount != P.LoopCount ||
        Derived.BackedgeCount != P.BackedgeCount)
      return "progress witnesses do not prove the represented control flow";
  } catch (const std::invalid_argument &Error) {
    return Error.what();
  }
  return {};
}
} // namespace neverd::diff

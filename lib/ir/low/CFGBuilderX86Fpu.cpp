//===- CFGBuilderX86Fpu.cpp - x87 FPU stack fixup for CFGBuilder ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The x86/x86-64 x87 stack-pointer (TOP) fixup for CFGBuilder.  It is an
/// architecture-gated routine of CFG construction (it returns immediately
/// unless the image is x86 or x86-64), so it lives here following the
/// target-dispatch split used by the jump-table detectors
/// (JumpTableResolverARM.cpp) rather than in the architecture-neutral
/// CFGBuilder.cpp.
///
/// The lifter names x87 registers as physical slots ST((TOP+i)&7) while
/// advancing TOP in worklist (lift) order.  When a branch leaves a value on the
/// FP stack and one arm net-changes the depth, the other arm is lifted with the
/// wrong TOP; fixupFpuStack re-bases each block's ST(i) references so its TOP
/// matches the control-flow predecessor's exit TOP.  For blocks reachable at
/// several distinct TOPs it rebuilds the CFG as the (block x entry-TOP) product
/// so each copy is single-TOP.  A no-op (per-block offset 0) for straight-line
/// or stack-balanced code, which is the overwhelmingly common case.
///
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/ImportCallee.h"
#include "neverd/ir/low/LowNoReturn.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/SymbolDecoration.h"

#include <algorithm>
#include <list>
#include <llvm/Support/SaveAndRestore.h>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <utility>
#include <vector>

namespace neverd {

namespace {

bool isStReg(const NdVar &V) {
  return V.isReg() && V.Offset >= x86reg::ST0 && V.Offset <= x86reg::ST7 &&
         ((V.Offset - x86reg::ST0) % x86reg::FPURegStride == 0);
}

void rebaseStReg(NdVar &V, int Offset) {
  if (!isStReg(V))
    return;
  V.Offset = x86reg::stReg((x86reg::stRegIndex(V.Offset) + Offset) & 7);
}

// Sign-extend a per-instruction TOP change (range -2..+2) from its &7 form.
int demaskDelta(int D) {
  D &= 7;
  return D >= 4 ? D - 8 : D;
}

// FXAM classifies the encoding, not a rounded host floating-point value. Keep
// its integer lowering in LowIR so LLVM, HighC and concrete execution share
// the same condition-code semantics. Tag is an all-path CFG fact.
std::vector<LowOp> lowerX87Examine(const LowOp &Exam, bool Empty,
                                   uint64_t &NextTemp) {
  std::vector<LowOp> Ops;
  auto emit = [&](NdOp Code, unsigned Width,
                  std::initializer_list<NdVar> Inputs) {
    LowOp Op;
    Op.Opcode = Code;
    Op.Addr = Exam.Addr;
    Op.Seq = Ops.size();
    Op.Output = NdVar::tmp(NextTemp++, Width);
    for (const auto &V : Inputs)
      Op.addInput(V);
    Ops.push_back(Op);
    return Op.Output;
  };
  auto n = [](uint64_t Bits, unsigned Width = 2) {
    return NdVar::scalar(Bits, Width);
  };
  const NdVar SignExponent = emit(NdOp::SUBBYTES, 2, {Exam.Inputs[1], n(8)});
  const NdVar Sign =
      emit(NdOp::INT_AND, 2,
           {emit(NdOp::INT_RIGHT, 2, {SignExponent, n(6)}), n(0x200)});
  NdVar Class = n(0x4100); // Empty: C3=1, C2=0, C0=1, with payload's sign.
  if (!Empty) {
    const NdVar Significand = emit(NdOp::SUBBYTES, 8, {Exam.Inputs[1], n(0)});
    const NdVar Exponent = emit(NdOp::INT_AND, 2, {SignExponent, n(0x7fff)});
    const NdVar IntegerBit =
        emit(NdOp::INT_NOTEQUAL, 1,
             {emit(NdOp::INT_AND, 8, {Significand, n(UINT64_C(1) << 63, 8)}),
              n(0, 8)});
    const NdVar FractionZero =
        emit(NdOp::INT_EQUAL, 1,
             {emit(NdOp::INT_AND, 8, {Significand, n(UINT64_MAX >> 1, 8)}),
              n(0, 8)});
    const NdVar ZeroOrDenormal =
        emit(NdOp::SELECT, 2,
             {emit(NdOp::INT_EQUAL, 1, {Significand, n(0, 8)}), n(0x4000),
              n(0x4400)});
    const NdVar InfinityOrNan =
        emit(NdOp::SELECT, 2, {FractionZero, n(0x500), n(0x100)});
    const NdVar FiniteOrSpecial =
        emit(NdOp::SELECT, 2,
             {emit(NdOp::INT_EQUAL, 1, {Exponent, n(0x7fff)}), InfinityOrNan,
              n(0x400)});
    const NdVar Supported =
        emit(NdOp::SELECT, 2, {IntegerBit, FiniteOrSpecial, n(0)});
    Class = emit(NdOp::SELECT, 2,
                 {emit(NdOp::INT_EQUAL, 1, {Exponent, n(0)}), ZeroOrDenormal,
                  Supported});
  }
  const NdVar OtherStatus = emit(NdOp::INT_AND, 2, {Exam.Inputs[2], n(0xb8ff)});
  const NdVar Flags = emit(NdOp::INT_OR, 2, {Class, Sign});
  emit(NdOp::INT_OR, 2, {OtherStatus, Flags});
  Ops.back().Output = Exam.Output;
  return Ops;
}

} // namespace

// Only this compact projection is shared: callers retain independent active
// sets, recursion depths, proof allowances and completed effect answers.
struct X87CallGraphStep {
  int Delta = 0;
  uint8_t Reads = 0;
  uint8_t Writes = 0;
  std::optional<va_t> Callee;
  bool Unknown = false;
};
struct X87CallGraphBlock {
  std::vector<X87CallGraphStep> Steps;
  std::vector<int> Successors;
  bool Returns = false;
  bool Stops = false;
};
struct X87CallGraph {
  enum class ProjectionRejection : uint8_t {
    None,
    ExceptionalEdges,
    MissingTerminator,
  };
  std::vector<X87CallGraphBlock> Blocks;
  size_t Cost = 0;
  bool Complete = false;
  ProjectionRejection Rejection = ProjectionRejection::None;
};

class X87CallGraphCache {
  friend class X87CallEffectIndex;
  friend class CFGBuilder;
  struct Context {
    const BinaryImage *Image;
    Arch Architecture;
    InstructionMode Mode;
    Bitness Bits;
    BinaryFormat Format;
    BinaryFormat ABI;
    const libc::NoReturnTargetIndex *NoReturnTargets;
    const NoReturnCalleeProver *NoReturnCallees;
    unsigned NoReturnDepth;
    const detail::AbsoluteRelocationRootIndex *RelocationRoots;
    const ExecutableCodeOwnerIndex *CodeOwners;
    std::optional<std::vector<va_t>> Entries;
    std::optional<std::vector<va_t>> ProtectedSlots;
    std::optional<std::vector<va_t>> UnsafeBranches;
  };
  struct Entry {
    va_t Address;
    std::shared_ptr<const X87CallGraph> Value;
    size_t Bytes;
  };
  static constexpr size_t MaxGraphs = 128;
  static constexpr size_t MaxBytes = 8 * 1024 * 1024;
  std::mutex Mutex;
  std::optional<Context> Inputs;
  std::list<Entry> Graphs;
  size_t RetainedBytes = 0;
  size_t Hits = 0;

  static bool account(size_t &Bytes, size_t Count, size_t Width) {
    if (Bytes > MaxBytes || Count > (MaxBytes - Bytes) / Width)
      return false;
    Bytes += Count * Width;
    return true;
  }
  static bool accountAllocation(size_t &Bytes, size_t Count, size_t Width) {
    return !Count ||
           (account(Bytes, 8, sizeof(void *)) && account(Bytes, Count, Width));
  }
  static bool sameSet(const std::optional<std::vector<va_t>> &Frozen,
                      const std::set<va_t> *Live) {
    return Frozen.has_value() == (Live != nullptr) &&
           (!Live ||
            (Frozen->size() == Live->size() &&
             std::equal(Frozen->begin(), Frozen->end(), Live->begin())));
  }
  bool matches(const CFGBuilder &B) const {
    return Inputs && Inputs->Image == B.CurrentImg &&
           Inputs->Architecture == B.CurrentImg->Arch &&
           Inputs->Mode == B.CurrentImg->Mode &&
           Inputs->Bits == B.CurrentImg->Bits &&
           Inputs->Format == B.CurrentImg->Format &&
           Inputs->ABI == B.CurrentImg->abiFormat() &&
           Inputs->NoReturnTargets == B.NoReturnTargets &&
           Inputs->NoReturnCallees == B.NoReturnCallees &&
           Inputs->NoReturnDepth == B.NoReturnCalleeDepth + 1 &&
           Inputs->RelocationRoots == B.AbsoluteRelocationRoots &&
           Inputs->CodeOwners == B.ExecutableCodeOwners &&
           sameSet(Inputs->Entries, B.KnownFuncEntries) &&
           sameSet(Inputs->ProtectedSlots,
                   B.ProtectedJumpTableRelocationSlots) &&
           sameSet(Inputs->UnsafeBranches, B.UnsafeJumpTableBranches);
  }
  static std::optional<size_t> graphBytes(const X87CallGraph &Graph) {
    size_t Bytes = sizeof(Entry) + sizeof(X87CallGraph) + 16 * sizeof(void *);
    if (!accountAllocation(Bytes, Graph.Blocks.capacity(),
                           sizeof(X87CallGraphBlock)))
      return std::nullopt;
    for (const auto &Block : Graph.Blocks)
      if (!accountAllocation(Bytes, Block.Steps.capacity(),
                             sizeof(X87CallGraphStep)) ||
          !accountAllocation(Bytes, Block.Successors.capacity(), sizeof(int)))
        return std::nullopt;
    return Bytes;
  }
  static std::optional<std::vector<va_t>> freeze(const std::set<va_t> *Values) {
    if (!Values)
      return std::nullopt;
    return std::vector<va_t>(Values->begin(), Values->end());
  }
  static std::optional<size_t> contextBytes(const Context &C) {
    size_t Bytes = sizeof(Context);
    for (const auto *Values :
         {&C.Entries, &C.ProtectedSlots, &C.UnsafeBranches})
      if (*Values &&
          !accountAllocation(Bytes, (*Values)->capacity(), sizeof(va_t)))
        return std::nullopt;
    return Bytes;
  }
  std::shared_ptr<const X87CallGraph> find(const CFGBuilder &B, va_t Address) {
    std::lock_guard<std::mutex> Lock(Mutex);
    if (!matches(B))
      return {};
    for (auto It = Graphs.begin(); It != Graphs.end(); ++It)
      if (It->Address == Address) {
        Graphs.splice(Graphs.begin(), Graphs, It);
        ++Hits;
        return Graphs.front().Value;
      }
    return {};
  }
  std::shared_ptr<const X87CallGraph>
  publish(const CFGBuilder &B, va_t Address,
          std::shared_ptr<const X87CallGraph> Graph) {
    if (!Graph->Complete &&
        Graph->Rejection == X87CallGraph::ProjectionRejection::None)
      return Graph;
    const auto Bytes = graphBytes(*Graph);
    if (!Bytes)
      return Graph;
    // Bound context copies before allocation, then account their actual
    // capacities before publication. The limit is retained payload, not RSS
    // of in-flight builds or graphs still used by an active private index.
    size_t Estimated = sizeof(Context);
    for (const auto *Values :
         {B.KnownFuncEntries, B.ProtectedJumpTableRelocationSlots,
          B.UnsafeJumpTableBranches})
      if (Values && !accountAllocation(Estimated, Values->size(), sizeof(va_t)))
        return Graph;
    if (*Bytes > MaxBytes - Estimated)
      return Graph;
    std::lock_guard<std::mutex> Lock(Mutex);
    if (!matches(B)) {
      Context Next{B.CurrentImg,
                   B.CurrentImg->Arch,
                   B.CurrentImg->Mode,
                   B.CurrentImg->Bits,
                   B.CurrentImg->Format,
                   B.CurrentImg->abiFormat(),
                   B.NoReturnTargets,
                   B.NoReturnCallees,
                   B.NoReturnCalleeDepth + 1,
                   B.AbsoluteRelocationRoots,
                   B.ExecutableCodeOwners,
                   freeze(B.KnownFuncEntries),
                   freeze(B.ProtectedJumpTableRelocationSlots),
                   freeze(B.UnsafeJumpTableBranches)};
      const auto NextBytes = contextBytes(Next);
      if (!NextBytes || *Bytes > MaxBytes - *NextBytes)
        return Graph;
      Graphs.clear();
      Inputs = std::move(Next);
      RetainedBytes = *NextBytes;
    }
    const auto InputBytes = contextBytes(*Inputs);
    if (!InputBytes || *Bytes > MaxBytes - *InputBytes)
      return Graph;
    // Another worker may have published while this worker built outside the
    // lock. Use its identical immutable projection without waiting on builds.
    for (auto It = Graphs.begin(); It != Graphs.end(); ++It)
      if (It->Address == Address) {
        Graphs.splice(Graphs.begin(), Graphs, It);
        return Graphs.front().Value;
      }
    while (!Graphs.empty() &&
           (Graphs.size() >= MaxGraphs || *Bytes > MaxBytes - RetainedBytes)) {
      RetainedBytes -= Graphs.back().Bytes;
      Graphs.pop_back();
    }
    Graphs.push_front({Address, Graph, *Bytes});
    RetainedBytes += *Bytes;
    return Graph;
  }
};

std::shared_ptr<X87CallGraphCache> createX87CallGraphCache() {
  return std::make_shared<X87CallGraphCache>();
}

std::array<size_t, 3> CFGBuilder::x87CallGraphCacheStatsForTesting() const {
  if (!SharedX87CallGraphs)
    return {};
  std::lock_guard<std::mutex> Lock(SharedX87CallGraphs->Mutex);
  return {SharedX87CallGraphs->Hits, SharedX87CallGraphs->Graphs.size(),
          SharedX87CallGraphs->RetainedBytes};
}

/// The index lives for one CFG build of an unchanged image. Its cached local
/// graphs contain machine facts, not answers obtained under a caller's budget.
/// Each query pays for its complete call closure and every dataflow transfer.
class X87CallEffectIndex {
  friend class CFGBuilder;
  using Step = X87CallGraphStep;
  using Block = X87CallGraphBlock;
  using Graph = X87CallGraph;
  struct Query {
    size_t Remaining = limits::kMaxX87CallProofWork;
    std::set<va_t> Charged, Active;
    std::map<std::pair<va_t, unsigned>, std::optional<int>> Results;
    bool pay(size_t Cost) {
      if (Cost > Remaining) {
        Remaining = 0;
        return false;
      }
      Remaining -= Cost;
      return true;
    }
  };
  const CFGBuilder &Settings;
  const BinaryImage &Image;
  std::map<va_t, std::shared_ptr<const Graph>> Graphs;
  std::map<va_t, std::optional<int>> Answers;

  std::optional<int> importEffect(llvm::StringRef Name) const {
    bool Floating = false, LongDouble = false, Complex = false;
    if (const auto *Prototype =
            libc::libcPrototype(Name.str(), Image.abiFormat())) {
      Floating = libc::isFloatingType(Prototype->Return);
      LongDouble = Prototype->Return == "long double";
    } else if (const auto Arity = importNamesAreCNames(Image.Format)
                                      ? libc::libcArity(Name.str())
                                      : libc::libcArityForSymbol(Name.str())) {
      Floating = libc::floatReturnBytes(*Arity) != 0;
      LongDouble = Arity->FpRetLongDouble;
      Complex = Arity->FpRetComplex;
    } else {
      return std::nullopt;
    }
    if (Image.Arch == Arch::X86)
      return Complex ? std::nullopt
                     : std::optional<int>(Floating || LongDouble ? -1 : 0);
    // Win64 toolchains disagree on long double's representation. An import
    // name alone cannot choose MSVC's double or MinGW's x87 result contract.
    if (LongDouble)
      return Image.abiFormat() == BinaryFormat::COFF || Complex
                 ? std::nullopt
                 : std::optional<int>(-1);
    return 0;
  }

  static bool modeledIntrinsic(const LowOp &Op) {
    if (Op.NumInputs == 0 || !Op.Inputs[0].isConst())
      return false;
    if (isArchitecturalNoReturn(Op))
      return true;
    // The decoder already accounts for the numeric x87 operations' pops and
    // pushes. Environment restores, tag changes and unknown intrinsics have
    // additional state effects and cannot acquire a net-stack summary here.
    switch (static_cast<Intrinsic>(Op.Inputs[0].Offset)) {
    case Intrinsic::X87Fsin:
    case Intrinsic::X87Fcos:
    case Intrinsic::X87F2xm1:
    case Intrinsic::X87Fscale:
    case Intrinsic::X87Fprem:
    case Intrinsic::X87Fprem1:
    case Intrinsic::X87Fpatan:
    case Intrinsic::X87Fyl2x:
    case Intrinsic::X87Fyl2xp1:
    case Intrinsic::X87Fptan:
    case Intrinsic::X87Fxtractsig:
    case Intrinsic::X87Fxtractexp:
    case Intrinsic::X87Fnclex:
    case Intrinsic::X87ReadStatus:
    case Intrinsic::X87Fxam:
    case Intrinsic::X87Wait:
    case Intrinsic::Pause:
    case Intrinsic::Cpuid:
    case Intrinsic::Rdtsc:
    case Intrinsic::Rdtscp:
    case Intrinsic::Mfence:
    case Intrinsic::Lfence:
    case Intrinsic::Sfence:
      return true;
    default:
      return false;
    }
  }

  const Graph &graph(va_t Entry) {
    if (const auto It = Graphs.find(Entry); It != Graphs.end())
      return *It->second;
    const bool HasCodeOwner = Image.hasExecutableCodeOwnerAt(Entry);
    if (HasCodeOwner && Settings.SharedX87CallGraphs)
      if (auto Shared = Settings.SharedX87CallGraphs->find(Settings, Entry))
        return *Graphs.emplace(Entry, std::move(Shared)).first->second;
    auto Built = std::make_shared<Graph>();
    const auto It = Graphs.emplace(Entry, Built).first;
    Graph &G = *Built;
    if (!HasCodeOwner)
      return G;
    Decoder Dec;
    if (!Dec.init(Image))
      return G;
    CFGBuilder Builder;
    Builder.SkipX87StackFixup = true;
    Builder.setKnownFuncEntries(Settings.KnownFuncEntries);
    Builder.setNoReturnTargetIndex(Settings.NoReturnTargets);
    Builder.setNoReturnCalleeProver(Settings.NoReturnCallees,
                                    Settings.NoReturnCalleeDepth + 1);
    Builder.setAbsoluteRelocationRootIndex(Settings.AbsoluteRelocationRoots);
    Builder.setExecutableCodeOwnerIndex(Settings.ExecutableCodeOwners);
    Builder.ProtectedJumpTableRelocationSlots =
        Settings.ProtectedJumpTableRelocationSlots;
    Builder.UnsafeJumpTableBranches = Settings.UnsafeJumpTableBranches;
    const LowFunc F = Builder.build(Image, Dec, Entry);
    if (!F.hasCompleteInstructionLift() || !F.TruncatedPathAddresses.empty() ||
        F.Blocks.empty() || F.Blocks.front().StartAddr != Entry)
      return G;
    auto RejectProjection =
        [&](Graph::ProjectionRejection Reason) -> const Graph & {
      // These two exclusions are immutable facts of a completely lifted CFG,
      // not failures of a caller's proof allowance. Retain only an empty
      // refusal marker; prove() still rejects !Complete before charging Cost.
      G = Graph{};
      G.Rejection = Reason;
      if (Settings.SharedX87CallGraphs)
        It->second =
            Settings.SharedX87CallGraphs->publish(Settings, Entry, Built);
      return *It->second;
    };
    G.Blocks.resize(F.Blocks.size());
    for (size_t I = 0; I < F.Blocks.size(); ++I) {
      const LowBlock &Source = F.Blocks[I];
      Block &B = G.Blocks[I];
      if (Source.Id != static_cast<int>(I) ||
          Source.InstructionBoundaries.empty())
        return G;
      if (!Source.ExceptionalSuccs.empty() || !Source.ExceptionalPreds.empty())
        return RejectProjection(Graph::ProjectionRejection::ExceptionalEdges);
      G.Cost += 1 + Source.Ops.size() + Source.Succs.size();
      if (G.Cost > limits::kMaxX87CallProofWork)
        return G;
      B.Successors = Source.Succs;
      for (int S : B.Successors)
        if (S < 0 || static_cast<size_t>(S) >= F.Blocks.size())
          return G;
      for (const LowInstructionBoundary &Boundary :
           Source.InstructionBoundaries) {
        const auto RecIt = Builder.Insns.find(Boundary.Address);
        if (RecIt == Builder.Insns.end() ||
            RecIt->second.Size != Boundary.Size ||
            Boundary.FirstOp > Source.Ops.size() ||
            Boundary.OpCount > Source.Ops.size() - Boundary.FirstOp)
          return G;
        const auto &Rec = RecIt->second;
        Step S;
        S.Delta = demaskDelta(Rec.FpuTopOut - Rec.FpuTopIn);
        S.Unknown = Rec.FpuReset;
        for (size_t O = Boundary.FirstOp;
             O < Boundary.FirstOp + Boundary.OpCount; ++O) {
          const LowOp &Op = Source.Ops[O];
          if (Op.Opcode == NdOp::INTRINSIC) {
            S.Unknown |= !modeledIntrinsic(Op);
            B.Stops |= isArchitecturalNoReturn(Op);
          }
          for (unsigned K = 0; K < Op.NumInputs; ++K)
            if (isStReg(Op.Inputs[K])) {
              const uint8_t Bit =
                  uint8_t(1u << ((x86reg::stRegIndex(Op.Inputs[K].Offset) -
                                  Rec.FpuTopIn) &
                                 7));
              S.Reads |= Bit & ~S.Writes;
              S.Unknown |= Op.Inputs[K].Size != x86reg::FPURegSize;
            }
          if (isStReg(Op.Output)) {
            if (Op.Output.Size != x86reg::FPURegSize)
              S.Unknown = true;
            else
              S.Writes |= uint8_t(
                  1u << ((x86reg::stRegIndex(Op.Output.Offset) - Rec.FpuTopIn) &
                         7));
          }
          if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
            if (hasLowInstructionControlFlag(
                    Boundary.ControlFlags,
                    LowInstructionControlFlag::NoReturn)) {
              B.Stops = true;
              continue;
            }
            if (S.Callee || Op.NumInputs == 0)
              S.Unknown = true;
            else if (Op.Inputs[0].isConst())
              S.Callee = Op.Inputs[0].Offset;
            else
              S.Callee = loadedCallSlot(F, Source, O, Op.Inputs[0]);
            S.Unknown |= !S.Callee;
          }
          B.Returns |= Op.Opcode == NdOp::RETURN;
        }
        if (S.Delta || S.Reads || S.Writes || S.Callee || S.Unknown)
          B.Steps.push_back(S);
      }
      if (B.Successors.empty() && !B.Returns && !B.Stops)
        return RejectProjection(Graph::ProjectionRejection::MissingTerminator);
    }
    G.Complete = true;
    if (Settings.SharedX87CallGraphs)
      It->second =
          Settings.SharedX87CallGraphs->publish(Settings, Entry, Built);
    return *It->second;
  }

  std::optional<int> prove(va_t Entry, unsigned Depth, Query &Q) {
    if (!Q.pay(1) || Depth >= limits::kMaxX87CallProofDepth)
      return std::nullopt;
    const auto Key = std::make_pair(Entry, Depth);
    if (auto It = Q.Results.find(Key); It != Q.Results.end())
      return It->second;
    if (!Q.Active.insert(Entry).second)
      return std::nullopt;
    auto Compute = [&]() -> std::optional<int> {
      if (Q.Charged.insert(Entry).second &&
          Q.Charged.size() > limits::kMaxX87CallProofFunctions)
        return std::nullopt;
      if (const auto Name = importCalleeName(Image, Entry); !Name.empty())
        return importEffect(Name);
      const Graph &G = graph(Entry);
      if (!G.Complete || !Q.pay(G.Cost))
        return std::nullopt;
      std::map<va_t, int> Callees;
      for (const Block &B : G.Blocks)
        for (const Step &S : B.Steps) {
          if (S.Unknown)
            return std::nullopt;
          if (S.Callee && !Callees.count(*S.Callee)) {
            const auto Effect = prove(*S.Callee, Depth + 1, Q);
            if (!Effect || (*Effect != 0 && *Effect != -1))
              return std::nullopt;
            Callees.emplace(*S.Callee, *Effect);
          }
        }
      struct State {
        int Top = 0;
        uint8_t Defined = 0;
      };
      std::vector<std::optional<State>> Entries(G.Blocks.size()),
          Exits(G.Blocks.size());
      std::queue<int> Work;
      Entries[0] = State{};
      Work.push(0);
      while (!Work.empty()) {
        const int Id = Work.front();
        Work.pop();
        const Block &B = G.Blocks[Id];
        if (!Q.pay(1 + B.Steps.size() + B.Successors.size()))
          return std::nullopt;
        State Value = *Entries[Id];
        for (const Step &S : B.Steps) {
          for (int K = 0; K != 8; ++K)
            if ((S.Reads & (1u << K)) &&
                !(Value.Defined & (1u << ((Value.Top + K) & 7))))
              return std::nullopt;
          const int Delta = S.Delta + (S.Callee ? Callees.at(*S.Callee) : 0);
          // Pushes discard stale slots before their new definitions; pops
          // discard them after the instruction's reads and writes.
          for (int K = Delta; K < 0; ++K)
            Value.Defined &= uint8_t(~(1u << ((Value.Top + K) & 7)));
          for (int K = 0; K != 8; ++K)
            if (S.Writes & (1u << K))
              Value.Defined |= uint8_t(1u << ((Value.Top + K) & 7));
          for (int K = 0; K < Delta; ++K)
            Value.Defined &= uint8_t(~(1u << ((Value.Top + K) & 7)));
          Value.Top += Delta;
          if (Value.Top < -8 || Value.Top > 8)
            return std::nullopt;
          if (S.Callee && Callees.at(*S.Callee) == -1)
            Value.Defined |= uint8_t(1u << (Value.Top & 7));
        }
        Exits[Id] = Value;
        for (int S : B.Successors) {
          if (!Entries[S]) {
            Entries[S] = Value;
            Work.push(S);
          } else {
            if (Entries[S]->Top != Value.Top)
              return std::nullopt;
            const uint8_t Meet = Entries[S]->Defined & Value.Defined;
            if (Meet != Entries[S]->Defined) {
              Entries[S]->Defined = Meet;
              Work.push(S);
            }
          }
        }
      }
      std::optional<int> Result;
      for (size_t I = 0; I < G.Blocks.size(); ++I) {
        if (!Exits[I] || !G.Blocks[I].Returns)
          continue;
        const State &Out = *Exits[I];
        if ((Result && *Result != Out.Top) || (Out.Top != 0 && Out.Top != -1) ||
            (Out.Top == -1 && !(Out.Defined & (1u << 7))))
          return std::nullopt;
        Result = Out.Top;
      }
      return Result;
    };
    const auto Result = Compute();
    Q.Active.erase(Entry);
    Q.Results.emplace(Key, Result);
    return Result;
  }

public:
  explicit X87CallEffectIndex(const CFGBuilder &Builder)
      : Settings(Builder), Image(*Builder.CurrentImg) {}
  std::optional<int> effect(va_t Entry) {
    if (auto It = Answers.find(Entry); It != Answers.end())
      return It->second;
    Query Q;
    const auto Result = prove(Entry, 0, Q);
    Answers.emplace(Entry, Result);
    return Result;
  }
};

std::optional<int> CFGBuilder::x87CallEffectForTesting(const BinaryImage &Image,
                                                       va_t Entry,
                                                       size_t &Remaining,
                                                       unsigned Depth) {
  llvm::SaveAndRestore<const BinaryImage *> Restore(CurrentImg, &Image);
  X87CallEffectIndex Index(*this);
  X87CallEffectIndex::Query Q;
  Q.Remaining = Remaining;
  const auto Result = Index.prove(Entry, Depth, Q);
  Remaining = Q.Remaining;
  return Result;
}

void CFGBuilder::fixupFpuStack(LowFunc &Func) {
  if (SkipX87StackFixup)
    return;
  if (!CurrentImg)
    return;
  if (CurrentImg->Arch != Arch::X86 && CurrentImg->Arch != Arch::X64)
    return;
  if (Func.Blocks.empty())
    return;

  const size_t N = Func.Blocks.size();
  std::vector<int> LiftedIn(N, 0), LiftedOut(N, 0), BlockDelta(N, 0);
  std::vector<bool> HasReset(N, false), HasFpu(N, false);
  std::vector<std::map<va_t, uint8_t>> PoppedSlots(N);
  bool HasExamine = false;

  for (size_t I = 0; I < N; ++I) {
    auto &Blk = Func.Blocks[I];
    bool First = true;
    int CallShift = 0;
    for (const LowInstructionBoundary &Boundary : Blk.InstructionBoundaries) {
      const auto It = Insns.find(Boundary.Address);
      if (It == Insns.end())
        continue;
      const InsnRecord &Rec = It->second;
      if (First) {
        LiftedIn[I] = Rec.FpuTopIn;
        First = false;
      }
      if (Rec.FpuReset) {
        HasReset[I] = true;
        CallShift = 0;
      }
      const int Delta = demaskDelta(Rec.FpuTopOut - Rec.FpuTopIn);
      BlockDelta[I] += Delta;
      const int InstructionTop = (Rec.FpuTopIn + CallShift) & 7;
      bool RotateOnly = false;
      for (size_t O = Boundary.FirstOp;
           O < Boundary.FirstOp + Boundary.OpCount && O < Blk.Ops.size(); ++O) {
        LowOp &Op = Blk.Ops[O];
        if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
            Op.Inputs[0].isConst()) {
          const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
          HasExamine |= Id == Intrinsic::X87Fxam;
          if (Id == Intrinsic::X87Fxam)
            UnsupportedInstructionAddresses.insert(Op.Addr);
          RotateOnly |= Id == Intrinsic::X87Fincstp;
        }
        rebaseStReg(Op.Output, CallShift);
        for (uint8_t K = 0; K < Op.NumInputs; ++K)
          rebaseStReg(Op.Inputs[K], CallShift);
        if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
            Op.NumInputs == 0 ||
            hasLowInstructionControlFlag(Boundary.ControlFlags,
                                         LowInstructionControlFlag::NoReturn))
          continue;
        const auto Target = Op.Inputs[0].isConst()
                                ? std::optional<va_t>(Op.Inputs[0].Offset)
                                : loadedCallSlot(Func, Blk, O, Op.Inputs[0]);
        if (!Target)
          continue;
        if (!X87CallEffects)
          X87CallEffects = std::make_shared<X87CallEffectIndex>(*this);
        if (X87CallEffects->effect(*Target) != std::optional<int>(-1))
          continue;
        // The callee's complete returning paths push exactly one initialized
        // value. Publish the definition before SSA and advance TOP before
        // rebasing subsequent instructions, including successor blocks.
        --CallShift;
        --BlockDelta[I];
        Op.Output = NdVar::reg(x86reg::stReg((Rec.FpuTopIn + CallShift) & 7),
                               x86reg::FPURegSize);
      }
      if (!RotateOnly && !Rec.FpuReset)
        for (int K = 0; K < Delta; ++K)
          PoppedSlots[I][Boundary.Address] |=
              uint8_t(1u << ((InstructionTop + K) & 7));
      LiftedOut[I] = (Rec.FpuTopOut + CallShift) & 7;
    }
    for (auto &Op : Blk.Ops) {
      if (isStReg(Op.Output)) {
        HasFpu[I] = true;
        break;
      }
      for (int K = 0; K < Op.NumInputs && !HasFpu[I]; ++K)
        if (isStReg(Op.Inputs[K]))
          HasFpu[I] = true;
      if (HasFpu[I])
        break;
    }
  }

  // No x87 anywhere: nothing to do (the overwhelmingly common case).
  bool AnyFpu = false;
  for (size_t I = 0; I < N; ++I)
    AnyFpu = AnyFpu || HasFpu[I];
  if (!AnyFpu)
    return;
  if (!X87CallEffects)
    X87CallEffects = std::make_shared<X87CallEffectIndex>(*this);
  const bool ReturnsX87 =
      X87CallEffects->effect(Func.Entry) == std::optional<int>(-1);
  auto bindReturn = [&](LowBlock &Block, int Top) {
    if (!ReturnsX87)
      return;
    for (LowOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::RETURN && Op.NumInputs == 1)
        Op.Inputs[0] = NdVar::reg(x86reg::stReg(Top & 7), x86reg::FPURegSize);
  };

  int EntryBlk = -1;
  for (size_t I = 0; I < N; ++I)
    if (Func.Blocks[I].StartAddr == Func.Entry) {
      EntryBlk = static_cast<int>(I);
      break;
    }
  if (EntryBlk < 0)
    return;

  // Normalized (0..7) exit TOP for block B entered at TOP T.  A reset block
  // (FNINIT) restarts from its lifted exit regardless of the entry.
  auto exitTop = [&](int B, int T) {
    return (HasReset[B] ? LiftedOut[B] : (T + BlockDelta[B])) & 7;
  };

  // Propagate the control-flow-correct TOP along CFG edges as a *set* per
  // block: each block inherits every predecessor's exit TOP.  Straight-line /
  // stack- balanced code yields one TOP per block; a block reached at several
  // depths (a switch arm shared by a clang-peeled first iteration and the
  // steady loop body) yields several — x87 ST(i) is TOP-relative, so no single
  // re-base fits.
  std::vector<std::set<int>> TopSets(N);
  {
    std::queue<std::pair<int, int>> WL;
    TopSets[EntryBlk].insert(0);
    WL.push({EntryBlk, 0});
    size_t Steps = 0;
    const size_t StepCap = N * 8 + 16; // bounded: at most 8 TOPs per block
    while (!WL.empty()) {
      if (++Steps > StepCap)
        return; // pathological propagation: leave the function as lifted
      auto [B, T] = WL.front();
      WL.pop();
      int Ex = exitTop(B, T);
      for (int S : Func.Blocks[B].Succs) {
        if (S < 0 || S >= static_cast<int>(N))
          continue;
        if (TopSets[S].insert(Ex).second)
          WL.push({S, Ex});
      }
    }
  }

  bool MultiTop = false;
  for (size_t I = 0; I < N; ++I)
    if (TopSets[I].size() > 1) {
      MultiTop = true;
      break;
    }

  auto lowerExaminations = [&](const std::vector<std::pair<int, int>>
                                   &Origins) {
    if (!HasExamine)
      return;
    struct Tags {
      uint8_t Full = 0, Empty = 0, Payload = 0;
      bool operator==(const Tags &) const = default;
    };
    const size_t Count = Func.Blocks.size();
    std::vector<std::optional<Tags>> Incoming(Count);
    std::queue<int> Work;
    size_t Budget = limits::kMaxX87CallProofWork;
    auto seed = [&](int Id, Tags Value) {
      if (Id < 0 || size_t(Id) >= Count)
        return;
      auto &Old = Incoming[Id];
      const Tags Merged = Old ? Tags{uint8_t(Old->Full & Value.Full),
                                     uint8_t(Old->Empty & Value.Empty),
                                     uint8_t(Old->Payload & Value.Payload)}
                              : Value;
      if (!Old || *Old != Merged) {
        Old = Merged;
        Work.push(Id);
      }
    };
    std::map<std::pair<int, size_t>, bool> ProvenTags;
    auto transfer = [&](int Id, Tags State, bool Publish) {
      const LowBlock &Block = Func.Blocks[Id];
      const auto [Original, Offset] = Origins[Id];
      auto makeFull = [&](unsigned Slot) {
        State.Full |= uint8_t(1u << Slot);
        State.Payload |= uint8_t(1u << Slot);
        State.Empty &= uint8_t(~(1u << Slot));
      };
      auto makeEmpty = [&](unsigned Slot) {
        State.Empty |= uint8_t(1u << Slot);
        State.Full &= uint8_t(~(1u << Slot));
      };
      for (const auto &Boundary : Block.InstructionBoundaries) {
        if (Boundary.OpCount >= Budget || Boundary.FirstOp > Block.Ops.size() ||
            Boundary.OpCount > Block.Ops.size() - Boundary.FirstOp) {
          Budget = 0;
          return Tags{};
        }
        Budget -= 1 + Boundary.OpCount;
        for (size_t O = Boundary.FirstOp;
             O < Boundary.FirstOp + Boundary.OpCount; ++O) {
          const LowOp &Op = Block.Ops[O];
          if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
            State = {}; // The return definition below can establish one slot.
          if (Op.Opcode == NdOp::INTRINSIC) {
            if (!Op.NumInputs || !Op.Inputs[0].isConst()) {
              State = {};
            } else {
              const auto Kind = static_cast<Intrinsic>(Op.Inputs[0].Offset);
              if (Kind == Intrinsic::X87Fxam) {
                if (Publish && Op.NumInputs == 3 && Op.Output.Size == 2 &&
                    isStReg(Op.Inputs[1]) && Op.Inputs[1].Size == 10 &&
                    Op.Inputs[2].Size == 2) {
                  const unsigned Bit =
                      1u << x86reg::stRegIndex(Op.Inputs[1].Offset);
                  if ((State.Full & State.Payload) & Bit)
                    ProvenTags[{Id, O}] = false;
                  else if ((State.Empty & State.Payload) & Bit)
                    ProvenTags[{Id, O}] = true;
                }
              } else if (Kind == Intrinsic::X87Fninit) {
                // FNINIT empties tags without defining retained payload bits.
                // FXAM still reads that payload's sign even for an empty slot.
                State = {0, 0xff, State.Payload};
              } else if (Kind == Intrinsic::X87Ffree && Op.NumInputs == 3 &&
                         isStReg(Op.Inputs[1])) {
                makeEmpty(x86reg::stRegIndex(Op.Inputs[1].Offset));
              } else if (Kind != Intrinsic::X87Fincstp &&
                         Kind != Intrinsic::X87Fnclex &&
                         Kind != Intrinsic::X87Wait &&
                         Kind != Intrinsic::X87ReadStatus) {
                // Restores, resumable traps and unknown extensions may replace
                // tags. A subsequent explicit definition can recover a slot.
                State = {};
              }
            }
          }
          if (isStReg(Op.Output)) {
            if (Op.Output.Size == x86reg::FPURegSize)
              makeFull(x86reg::stRegIndex(Op.Output.Offset));
            else
              State = {}; // MMX/partial state is not a full x87 definition.
          }
        }
        auto Pop = PoppedSlots[Original].find(Boundary.Address);
        if (Pop != PoppedSlots[Original].end())
          for (unsigned Slot = 0; Slot < 8; ++Slot)
            if (Pop->second & (1u << Slot))
              makeEmpty((Slot + Offset) & 7);
      }
      return State;
    };
    // Each tag bit has an independent gen/kill transfer. Summarize blocks
    // once; replaying their instruction bodies at every loop meet spends the
    // proof budget on unchanged instructions in large CRT formatters.
    std::vector<Tags> Generated(Count), Preserved(Count);
    for (size_t I = 0; I < Count && Budget; ++I) {
      Generated[I] = transfer(I, {}, false);
      const Tags All = transfer(I, {0xff, 0xff, 0xff}, false);
      Preserved[I] = {uint8_t(All.Full & ~Generated[I].Full),
                      uint8_t(All.Empty & ~Generated[I].Empty),
                      uint8_t(All.Payload & ~Generated[I].Payload)};
    }
    for (const LowBlock &Block : Func.Blocks) {
      auto rootWithin = [&](const std::set<va_t> &Roots) {
        auto It = Roots.lower_bound(Block.StartAddr);
        return It != Roots.end() &&
               (*It == Block.StartAddr || *It < Block.EndAddr);
      };
      if (Block.StartAddr == Func.Entry || Block.Preds.empty() ||
          !Block.ExceptionalPreds.empty() ||
          rootWithin(Func.ModuleAnalysisRoots) ||
          rootWithin(CurrentImg->CodeRefTargets))
        seed(Block.Id, {});
    }
    auto solve = [&]() {
      while (!Work.empty() && Budget) {
        const int Id = Work.front();
        Work.pop();
        --Budget;
        const Tags In = *Incoming[Id], Gen = Generated[Id],
                   Keep = Preserved[Id];
        const Tags Out{uint8_t(Gen.Full | (In.Full & Keep.Full)),
                       uint8_t(Gen.Empty | (In.Empty & Keep.Empty)),
                       uint8_t(Gen.Payload | (In.Payload & Keep.Payload))};
        for (int Successor : Func.Blocks[Id].Succs)
          seed(Successor, Out);
        for (const auto &Edge : Func.Blocks[Id].ExceptionalSuccs)
          seed(Edge.BlockId, {});
      }
    };
    solve();
    // A disconnected component has an independent unknown initial FPU state.
    for (size_t I = 0; I < Count; ++I)
      if (!Incoming[I])
        seed(I, {});
    solve();
    if (Budget)
      for (size_t I = 0; I < Count && Budget; ++I)
        transfer(I, *Incoming[I], true);
    if (!Budget)
      ProvenTags.clear();
    uint64_t NextTemp = 0;
    for (const auto &B : Func.Blocks)
      for (const LowOp &Op : B.Ops) {
        if (Op.Output.isTemp())
          NextTemp = std::max(NextTemp, Op.Output.Offset + 1);
        for (unsigned K = 0; K < Op.NumInputs; ++K)
          if (Op.Inputs[K].isTemp())
            NextTemp = std::max(NextTemp, Op.Inputs[K].Offset + 1);
      }
    std::set<va_t> Expanded, Pending;
    for (LowBlock &B : Func.Blocks) {
      std::vector<LowOp> Rewritten;
      std::vector<size_t> NewOffsets(B.Ops.size() + 1);
      for (size_t O = 0; O < B.Ops.size(); ++O) {
        NewOffsets[O] = Rewritten.size();
        const LowOp &Op = B.Ops[O];
        if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
            Op.Inputs[0].isConst() &&
            Op.Inputs[0].Offset == static_cast<uint64_t>(Intrinsic::X87Fxam)) {
          if (auto It = ProvenTags.find({B.Id, O}); It != ProvenTags.end()) {
            auto Lowered = lowerX87Examine(Op, It->second, NextTemp);
            Rewritten.insert(Rewritten.end(), Lowered.begin(), Lowered.end());
            Expanded.insert(Op.Addr);
            continue;
          }
          Pending.insert(Op.Addr);
          UnsupportedInstructionAddresses.insert(Op.Addr);
        }
        Rewritten.push_back(Op);
      }
      NewOffsets[B.Ops.size()] = Rewritten.size();
      for (auto &Boundary : B.InstructionBoundaries) {
        if (Boundary.FirstOp > B.Ops.size() ||
            Boundary.OpCount > B.Ops.size() - Boundary.FirstOp)
          continue;
        const auto End = Boundary.FirstOp + Boundary.OpCount;
        Boundary.OpCount = NewOffsets[End] - NewOffsets[Boundary.FirstOp];
        Boundary.FirstOp = NewOffsets[Boundary.FirstOp];
      }
      B.Ops = std::move(Rewritten);
    }
    for (va_t Address : Expanded)
      if (!Pending.count(Address))
        UnsupportedInstructionAddresses.erase(Address);
  };

  if (!MultiTop) {
    std::vector<std::pair<int, int>> Origins;
    // One TOP per block — re-base each block in place (the common case; a no-op
    // for offset 0, i.e. straight-line / stack-balanced code).
    for (size_t I = 0; I < N; ++I) {
      const int Top = TopSets[I].empty() ? LiftedIn[I] : *TopSets[I].begin();
      Origins.emplace_back(I, HasReset[I] ? 0 : (Top - LiftedIn[I]) & 7);
      if (TopSets[I].empty())
        continue;
      int Offset = (Top - LiftedIn[I]) & 7;
      if (!HasReset[I] && HasFpu[I] && Offset != 0)
        for (auto &Op : Func.Blocks[I].Ops) {
          rebaseStReg(Op.Output, Offset);
          for (int K = 0; K < Op.NumInputs; ++K)
            rebaseStReg(Op.Inputs[K], Offset);
        }
      bindReturn(Func.Blocks[I], exitTop(static_cast<int>(I), Top));
    }
    lowerExaminations(Origins);
    return;
  }

  // A block reached at several TOPs cannot be re-based by one offset, so
  // rebuild the CFG as the product (block × entry-TOP): each reached (B,t)
  // state becomes its own block re-based for that TOP.  Every copy is then
  // single-TOP, and the disjoint x87 phases get independent register mappings;
  // each successor edge is routed to the copy whose TOP matches the
  // predecessor's exit TOP.
  std::map<std::pair<int, int>, int> StateIdx;
  std::vector<std::pair<int, int>> States;
  auto addState = [&](int B, int T) {
    auto Key = std::make_pair(B, T);
    if (StateIdx.emplace(Key, static_cast<int>(States.size())).second)
      States.push_back(Key);
  };
  addState(EntryBlk, 0); // entry stays block 0
  for (size_t B = 0; B < N; ++B)
    for (int T : TopSets[B])
      addState(static_cast<int>(B), T);
  // Blocks unreachable in the propagation keep one copy so no block or edge is
  // dropped (they are dead, so the chosen TOP is immaterial).
  for (size_t B = 0; B < N; ++B)
    if (TopSets[B].empty())
      addState(static_cast<int>(B), LiftedIn[B] & 7);

  auto stateFor = [&](int S, int Ex) {
    int T = TopSets[S].count(Ex)  ? Ex
            : !TopSets[S].empty() ? *TopSets[S].begin()
                                  : (LiftedIn[S] & 7);
    auto It = StateIdx.find({S, T});
    return It != StateIdx.end() ? It->second : S;
  };

  std::vector<LowBlock> NewBlocks(States.size());
  for (size_t NI = 0; NI < States.size(); ++NI) {
    int B = States[NI].first, T = States[NI].second;
    LowBlock NB = Func.Blocks[B];
    NB.Id = static_cast<int>(NI);
    NB.Preds.clear();
    NB.Succs.clear();
    if (!HasReset[B] && HasFpu[B]) {
      int Offset = (T - LiftedIn[B]) & 7;
      if (Offset != 0)
        for (auto &Op : NB.Ops) {
          rebaseStReg(Op.Output, Offset);
          for (int K = 0; K < Op.NumInputs; ++K)
            rebaseStReg(Op.Inputs[K], Offset);
        }
    }
    int Ex = exitTop(B, T);
    bindReturn(NB, Ex);
    for (int S : Func.Blocks[B].Succs)
      NB.Succs.push_back(S >= 0 && S < static_cast<int>(N) ? stateFor(S, Ex)
                                                           : S);
    NewBlocks[NI] = std::move(NB);
  }
  for (size_t NI = 0; NI < NewBlocks.size(); ++NI)
    for (int S : NewBlocks[NI].Succs)
      if (S >= 0 && S < static_cast<int>(NewBlocks.size()))
        NewBlocks[S].Preds.push_back(static_cast<int>(NI));
  Func.Blocks = std::move(NewBlocks);
  std::vector<std::pair<int, int>> Origins;
  for (const auto &[B, T] : States)
    Origins.emplace_back(B, HasReset[B] ? 0 : (T - LiftedIn[B]) & 7);
  lowerExaminations(Origins);
}

} // namespace neverd

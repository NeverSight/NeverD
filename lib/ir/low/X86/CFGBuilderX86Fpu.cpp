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
#include "neverd/ir/Limits.h"
#include "neverd/ir/X86/X86FPState.h"
#include "neverd/ir/X86/X87Context.h"
#include "neverd/ir/X86/X87GuardedValue.h"
#include "neverd/ir/X86/X87Stack.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/CallRegisterBody.h"
#include "neverd/ir/low/ImportCallee.h"
#include "neverd/ir/low/LowNoReturn.h"
#include "neverd/ir/low/X86/X87ImportCall.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/SymbolDecoration.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/ScopeExit.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <list>
#include <llvm/Support/SaveAndRestore.h>
#include <map>
#include <mutex>
#include <new>
#include <queue>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neverd {

namespace {

bool isStReg(const NdVar &V) {
  return V.isReg() && V.Offset >= x86reg::ST0 && V.Offset <= x86reg::ST7 &&
         ((V.Offset - x86reg::ST0) % x86reg::FPURegStride == 0);
}

/// The image's calling convention decides who owns the x87 register stack;
/// Decoder::configureFor gives its lifter the same answer.
bool ownsX87Context(const BinaryImage &Img) {
  return x87ContextOwnsStack(Img.Arch, Img.abiFormat(), Img.X87ContextOverride);
}

// The intrinsics that load or reinitialize the whole x87 state, or stand for
// an x87 operation NeverD does not model, may leave any tags. Value
// instructions define only their outputs (their pops are the lifter's TOP
// changes), and other intrinsics, such as an IDIV's divide-error
// precondition, do not touch the register stack.
bool replacesX87Tags(Intrinsic Kind) {
  switch (Kind) {
  case Intrinsic::X87Op:
  case Intrinsic::X87Fldenv:
  case Intrinsic::X87Fldenv16:
  case Intrinsic::X87Frstor:
  case Intrinsic::X87Frstor16:
  case Intrinsic::X87Fnsave:
  case Intrinsic::X87Fnsave16:
  case Intrinsic::Fxrstor:
  case Intrinsic::Fxrstor64Mem:
  case Intrinsic::Xrstor:
  case Intrinsic::Xrstor64:
  case Intrinsic::Xrstors:
  case Intrinsic::Xrstors64:
    return true;
  default:
    return false;
  }
}

bool isX87SignOperation(const LowOp &Op) {
  return (Op.Opcode == NdOp::FLOAT_NEG || Op.Opcode == NdOp::FLOAT_ABS) &&
         Op.NumInputs == 1 && isStReg(Op.Output) && isStReg(Op.Inputs[0]) &&
         Op.Output.Size == 10 && Op.Inputs[0].Size == 10 &&
         Op.Output.Offset == Op.Inputs[0].Offset;
}

bool isX87IntegerLoad(const LowOp &Op) {
  return Op.Opcode == NdOp::FLOAT_INT2FLOAT && Op.NumInputs == 1 &&
         isStReg(Op.Output) && Op.Output.Size == 10 &&
         (Op.Inputs[0].Size == 2 || Op.Inputs[0].Size == 4 ||
          Op.Inputs[0].Size == 8);
}

bool isX87GuardedValueOperation(const LowOp &Op) {
  return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
         Op.Inputs[0].isConst() &&
         isX87GuardedValueIntrinsic(
             static_cast<Intrinsic>(Op.Inputs[0].Offset));
}

/// One integer operation of a lowered x87 instruction at \p Address. A
/// constant in a numeric operand position is a number, as the lifter's own
/// emission makes it (isNumericConstantOperand): never an address its value
/// happens to name in an image.
LowOp loweredX87Op(NdOp Code, va_t Address, size_t Seq, NdVar Output,
                   std::initializer_list<NdVar> Inputs) {
  LowOp Op;
  Op.Opcode = Code;
  Op.Addr = Address;
  Op.Seq = Seq;
  Op.Output = Output;
  unsigned Index = 0;
  for (NdVar V : Inputs) {
    if (V.isConst() && V.Provenance == ConstantAddressProvenance::Unknown &&
        isNumericConstantOperand(Code, Index))
      V.Provenance = ConstantAddressProvenance::Scalar;
    Op.addInput(V);
    ++Index;
  }
  return Op;
}

// All signed FILD source widths fit exactly in binary80's 64-bit
// significand, independently of PC/RC. Construct bits with integer LowIR so
// neither C emitter depends on the host long double ABI. The tag prover must
// establish an empty push destination before this successful-load projection.
std::vector<LowOp> lowerX87IntegerLoad(const LowOp &Load, uint64_t &NextTemp) {
  std::vector<LowOp> Ops;
  auto emit = [&](NdOp Code, unsigned Width,
                  std::initializer_list<NdVar> Inputs) {
    Ops.push_back(loweredX87Op(Code, Load.Addr, Ops.size(),
                               NdVar::tmp(NextTemp++, Width), Inputs));
    return Ops.back().Output;
  };
  const NdVar Value = Load.Inputs[0].Size == 8
                          ? Load.Inputs[0]
                          : emit(NdOp::INT_SEXT, 8, {Load.Inputs[0]});
  const NdVar Negative = emit(NdOp::INT_SLESS, 1, {Value, NdVar::cst(0, 8)});
  const NdVar Negated = emit(NdOp::INT_SUB, 8, {NdVar::cst(0, 8), Value});
  const NdVar Magnitude = emit(NdOp::SELECT, 8, {Negative, Negated, Value});
  const NdVar Leading = emit(NdOp::LZCOUNT, 8, {Magnitude});
  // LZCOUNT(0) is 64. Mask the shift even on the zero path: SELECT cannot
  // repair an already-poisonous shift in LLVM or undefined C evaluation.
  const NdVar Shift = emit(NdOp::INT_AND, 8, {Leading, NdVar::cst(63, 8)});
  const NdVar Significand = emit(NdOp::INT_LEFT, 8, {Magnitude, Shift});
  const NdVar Exponent =
      emit(NdOp::INT_SUB, 8, {NdVar::cst(16383 + 63, 8), Leading});
  const NdVar Zero = emit(NdOp::INT_EQUAL, 1, {Magnitude, NdVar::cst(0, 8)});
  const NdVar Biased =
      emit(NdOp::SELECT, 8, {Zero, NdVar::cst(0, 8), Exponent});
  const NdVar Sign = emit(NdOp::INT_ZEXT, 8, {Negative});
  const NdVar SignBit = emit(NdOp::INT_LEFT, 8, {Sign, NdVar::cst(15, 8)});
  const NdVar Word = emit(NdOp::INT_OR, 8, {Biased, SignBit});
  const NdVar SignExponent = emit(NdOp::SUBBYTES, 2, {Word, NdVar::cst(0, 4)});
  emit(NdOp::CONCAT, 10, {SignExponent, Significand});
  Ops.back().Output = Load.Output;
  const NdVar Status = NdVar::reg(x86reg::FPU_SW, 2);
  emit(NdOp::INT_AND, 2,
       {Status, NdVar::cst(uint16_t(~(1u << x86reg::FPU_SW_C1_BIT)), 2)});
  Ops.back().Output = Status;
  return Ops;
}

// The tag prover admits this operation before any numeric projection can
// replace it. Sign changes preserve all other raw binary80 bits, including
// NaN payloads and encodings a host long double cannot represent.
std::vector<LowOp> lowerX87Sign(const LowOp &Sign, uint64_t &NextTemp) {
  std::vector<LowOp> Ops;
  auto emit = [&](NdOp Code, unsigned Width,
                  std::initializer_list<NdVar> Inputs) {
    Ops.push_back(loweredX87Op(Code, Sign.Addr, Ops.size(),
                               NdVar::tmp(NextTemp++, Width), Inputs));
    return Ops.back().Output;
  };
  const NdVar Significand =
      emit(NdOp::SUBBYTES, 8, {Sign.Inputs[0], NdVar::cst(0, 4)});
  const NdVar SignExponent =
      emit(NdOp::SUBBYTES, 2, {Sign.Inputs[0], NdVar::cst(8, 4)});
  const bool Absolute = Sign.Opcode == NdOp::FLOAT_ABS;
  const NdVar Changed =
      emit(Absolute ? NdOp::INT_AND : NdOp::INT_XOR, 2,
           {SignExponent, NdVar::cst(Absolute ? 0x7fff : 0x8000, 2)});
  emit(NdOp::CONCAT, 10, {Changed, Significand});
  Ops.back().Output = Sign.Output;
  return Ops;
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
    Ops.push_back(loweredX87Op(Code, Exam.Addr, Ops.size(),
                               NdVar::tmp(NextTemp++, Width), Inputs));
    return Ops.back().Output;
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

// A callback can synchronously request another graph while a leader builds.
// Never wait on any other flight from this nested call: two leaders could
// otherwise wait on each other, including through different caches.
struct ActiveX87CacheBuild {
  const ActiveX87CacheBuild *Previous;
};
thread_local const ActiveX87CacheBuild *CurrentX87CacheBuild = nullptr;
thread_local bool ReplayingCallRegisterBody = false;

} // namespace

// Only this compact projection is shared: callers retain independent active
// sets, recursion depths, proof allowances and completed effect answers.
struct X87CallGraphStep {
  int Delta = 0;
  uint8_t Reads = 0;
  uint8_t Writes = 0;
  std::optional<va_t> Callee;
  bool Unknown = false;
  bool ChangesControl = false;
};
struct X87CallGraphBlock {
  std::vector<X87CallGraphStep> Steps;
  std::vector<int> Successors;
  bool Returns = false;
  bool Stops = false;
};
/// A no-return answer a raw callee build read: the callee, the depth it was
/// asked at, and the answer.
struct X87NoReturnAnswer {
  va_t Target;
  unsigned Depth;
  bool NeverReturns;
};

struct X87CallGraph {
  enum class ProjectionRejection : uint8_t {
    None,
    ExceptionalEdges,
    MissingTerminator,
    InconsistentBlocks,
    ExceedsProofWork,
    /// The lift itself is incomplete (a decode failure, a path out of the
    /// image).  Unlike the others it can follow from a no-return answer, so
    /// it holds only while NoReturnAnswers do.
    IncompleteLift,
  };
  std::vector<X87CallGraphBlock> Blocks;
  size_t Cost = 0;
  bool Complete = false;
  bool UsesContext = false;
  ProjectionRejection Rejection = ProjectionRejection::None;
  /// For IncompleteLift, every no-return answer its build read.  The build
  /// is a function of the image, the cache context and those answers, so
  /// the same answers lift the same incomplete CFG again.
  std::vector<X87NoReturnAnswer> NoReturnAnswers;
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
    // The cache's immutable inventory, borrowed in place of Entries.
    const std::set<va_t> *EntriesSource = nullptr;
  };
  struct Entry {
    va_t Address;
    std::shared_ptr<const X87CallGraph> Value;
    size_t Bytes;
  };
  struct PendingGraph {
    Context Inputs;
    va_t Address;
    size_t Bytes;
    std::condition_variable Ready;
    std::shared_ptr<const X87CallGraph> Value;
    bool Done = false;
    PendingGraph(Context C, va_t A, size_t B)
        : Inputs(std::move(C)), Address(A), Bytes(B) {}
  };
  struct Request {
    std::shared_ptr<const X87CallGraph> Value;
    std::shared_ptr<PendingGraph> Leader;
  };
  static constexpr size_t MaxPendingGraphs =
      ::neverd::limits::ir::kCFGBuilderX86FpuMaxPendingGraphs;
  static constexpr size_t MaxPendingBytes =
      ::neverd::limits::ir::kCFGBuilderX86FpuMaxPendingBytes;
  // Entries in each of Graphs and Summaries, and the retained bytes of each.
  const size_t MaxGraphs;
  const size_t MaxBytes;
  const size_t MaxSummaryBytes;
  std::mutex Mutex;
  std::optional<Context> Inputs;
  std::list<Entry> Graphs;
  std::unordered_map<va_t, std::list<Entry>::iterator> GraphIndex;
  size_t RetainedBytes = 0;
  size_t Hits = 0;
  const NoReturnCalleeProver *NonReentrantProver;
  const libc::NoReturnTargetIndex *NonReentrantTargets;
  const std::set<va_t> *ImmutableEntries;
  const std::map<std::string, uint16_t> DeclaredFloatingImports;
  std::list<std::shared_ptr<PendingGraph>> Pending;
  size_t PendingBytes = 0;
  size_t Leaders = 0, Waits = 0, NonWaitingMisses = 0;
  std::condition_variable PendingChanged;
  struct SummaryFacts {
    CallRegisterBody Body;
    std::vector<va_t> Calls;
    std::vector<std::pair<va_t, bool>> NoReturnAnswers;
    // Blocks live only in Body. This owns every remaining LowFunc field except
    // the exception record, whose authoritative owner is the unchanged image.
    // Neither partial object can be emitted: cachedDefaultFunction joins them
    // into an independent complete LowFunc after validating the witness.
    std::unique_ptr<LowFunc> Function;
    const ExceptionFunction *Exception = nullptr;
  };
  struct SummaryEntry {
    va_t Address;
    std::shared_ptr<const SummaryFacts> Facts;
    size_t Bytes;
  };
  bool RetainCallRegisterBodies;
  // While set (warmCallGraphs), admission refuses what it could hold only by
  // evicting another entry, and records the refusal.
  bool KeepResident = false;
  bool RefusedResident = false;
  std::optional<Context> SummaryInputs;
  std::list<SummaryEntry> Summaries;
  std::unordered_map<va_t, std::list<SummaryEntry>::iterator> SummaryIndex;
  size_t SummaryBytes = 0;
  /// Whether a call to a target makes validatedSummary refuse the body that
  /// makes it (refusesCall), with the no-return answers of every incomplete
  /// lift its queries read.  Kept under RefusalInputs while those answers
  /// hold, so a target is proved once, not once per body and request.
  struct CallRefusal {
    bool Refuses = false;
    std::vector<X87NoReturnAnswer> Dependencies;
  };
  // A few dozen bytes per target: room for every function of a kernel.
  static constexpr size_t MaxRefusals =
      ::neverd::limits::ir::kCFGBuilderX86FpuMaxRefusals;
  static constexpr size_t MaxRefusalDependencies =
      ::neverd::limits::ir::kCFGBuilderX86FpuMaxRefusalDependencies;
  std::optional<Context> RefusalInputs;
  std::unordered_map<va_t, CallRefusal> Refusals;

public:
  X87CallGraphCache(const NoReturnCalleeProver *Prover,
                    const libc::NoReturnTargetIndex *Targets, bool RetainBodies,
                    const X87CallGraphCacheLimits &Limits,
                    const std::set<va_t> *Entries,
                    std::map<std::string, uint16_t> FloatingImports)
      : MaxGraphs(Limits.Entries), MaxBytes(Limits.GraphBytes),
        MaxSummaryBytes(Limits.BodyBytes), NonReentrantProver(Prover),
        NonReentrantTargets(Targets), ImmutableEntries(Entries),
        DeclaredFloatingImports(std::move(FloatingImports)),
        RetainCallRegisterBodies(RetainBodies) {}

private:
  bool canRetainSummary(const CFGBuilder &B) const {
    return RetainCallRegisterBodies && B.CurrentImg->Arch == Arch::X64 &&
           B.NoReturnCalleeDepth == 0 && !B.ProtectedJumpTableRelocationSlots &&
           !B.UnsafeJumpTableBranches &&
           (!B.NoReturnCallees || B.NoReturnCallees == NonReentrantProver) &&
           (!B.NoReturnTargets || B.NoReturnTargets == NonReentrantTargets);
  }
  std::optional<size_t> summaryBytes(llvm::ArrayRef<LowBlock> Blocks,
                                     size_t BlockCapacity, size_t Calls,
                                     size_t Answers) const {
    size_t Bytes =
        sizeof(SummaryEntry) + sizeof(SummaryFacts) + 16 * sizeof(void *);
    auto Allocation = [&](size_t Count, size_t Width) {
      if (!Count)
        return true;
      constexpr size_t Overhead = 8 * sizeof(void *);
      if (Bytes > MaxSummaryBytes - Overhead)
        return false;
      Bytes += Overhead;
      if (Count > (MaxSummaryBytes - Bytes) / Width)
        return false;
      Bytes += Count * Width;
      return true;
    };
    if (!Allocation(BlockCapacity, sizeof(LowBlock)) ||
        !Allocation(Calls, sizeof(va_t)) ||
        !Allocation(Answers, sizeof(std::pair<va_t, bool>)))
      return std::nullopt;
    for (const LowBlock &B : Blocks)
      if (!Allocation(B.Ops.capacity(), sizeof(LowOp)) ||
          !Allocation(B.InstructionBoundaries.capacity(),
                      sizeof(LowInstructionBoundary)) ||
          !Allocation(B.Succs.capacity(), sizeof(int)) ||
          !Allocation(B.Preds.capacity(), sizeof(int)) ||
          !Allocation(B.ExceptionalSuccs.capacity(), sizeof(ExceptionalEdge)) ||
          !Allocation(B.ExceptionalPreds.capacity(), sizeof(ExceptionalEdge)))
        return std::nullopt;
    return Bytes;
  }
  std::optional<size_t> functionMetadataBytes(const LowFunc &F) const {
    // Registration states have independent callback/proof ownership. Leave
    // those functions on the ordinary construction path.
    if (F.RegistrationStates)
      return std::nullopt;
    size_t Bytes = sizeof(LowFunc) + 8 * sizeof(void *);
    auto Allocation = [&](size_t Count, size_t Width) {
      if (!Count)
        return true;
      constexpr size_t Overhead = 8 * sizeof(void *);
      if (Bytes > MaxSummaryBytes - Overhead)
        return false;
      Bytes += Overhead;
      if (Count > (MaxSummaryBytes - Bytes) / Width)
        return false;
      Bytes += Count * Width;
      return true;
    };
    auto Vector = [&](const auto &Values) {
      return Allocation(Values.capacity(), sizeof(Values.front()));
    };
    auto String = [&](const std::string &Value) {
      return Value.capacity() < MaxSummaryBytes &&
             Allocation(Value.capacity() + 1, sizeof(char));
    };
    auto Set = [&](const std::set<va_t> &Values) {
      return Allocation(Values.size(), sizeof(va_t) + 8 * sizeof(void *));
    };
    // Blocks are charged by summaryBytes. ExceptionMetadata is not retained:
    // its exact source record is borrowed from the frozen image. Account all
    // other dynamic LowFunc fields, including nested table/provenance payloads.
    if (!String(F.Name) || !String(F.DebugName) || !String(F.SourceFile) ||
        !Vector(F.JumpTables) || !Vector(F.FunctionTemporaries) ||
        !Set(F.ModuleAnalysisRoots) || !Set(F.OrdinaryModuleAnalysisRoots) ||
        !Vector(F.CxxContinuationExits) || !Vector(F.CxxContinuationEntries) ||
        !Vector(F.SEHLocalUnwindContinuations) ||
        !Vector(F.RelocatedInstructionAddressOccurrences) ||
        !Vector(F.I386GetPcOccurrences) ||
        !Vector(F.RelocatedInstructionScalarModelOccurrences) ||
        !Set(F.EverPublishedJumpTableBranchAddresses) ||
        !Set(F.UnsafeIndirectBranchAddresses) ||
        !Vector(F.DecodeFailureAddresses) ||
        !Vector(F.UnsupportedInstructionAddresses) ||
        !Vector(F.UnprovenReturnAddresses) ||
        !Vector(F.TruncatedPathAddresses) || !Vector(F.CodeRefTargets))
      return std::nullopt;
    for (const JumpTable &Table : F.JumpTables)
      if (!Vector(Table.StorageRanges) ||
          !Vector(Table.SuppressibleRelocationSlots) ||
          !Vector(Table.AuthenticatedTableLoads) ||
          !Vector(Table.SelectorUseRefs) || !Vector(Table.Targets) ||
          !Vector(Table.SlotIndices) || !Vector(Table.CaseLabels))
        return std::nullopt;
    for (const auto &Exit : F.CxxContinuationExits)
      if (!Vector(Exit.Targets))
        return std::nullopt;
    for (const auto &Unwind : F.SEHLocalUnwindContinuations)
      if (!String(Unwind.OperationDigest) || !Vector(Unwind.CalleeDependencies))
        return std::nullopt;
    for (const auto &Occurrence : F.RelocatedInstructionAddressOccurrences)
      if (!Vector(Occurrence.ArithmeticProof))
        return std::nullopt;
    return Bytes;
  }
  void retainSummary(const CFGBuilder &B, va_t Address, LowFunc &F,
                     const CFGBuilder::CallRegisterBodyWitness &Witness) {
    // An incomplete lift is retained too: replaying the answers its build
    // read reproduces it as it does a complete one, and a caller's call
    // effects read it as incomplete either way.
    if (!canRetainSummary(B) || !Witness.Complete || F.Blocks.empty() ||
        F.Blocks.front().StartAddr != Address)
      return;
    const bool Covered = F.hasCompleteLiftCoverage();
    // Called only after the raw graph projection has finished reading F. Move
    // its blocks into one owner instead of copying them for every consumer.
    if (!summaryBytes(F.Blocks, F.Blocks.capacity(), Witness.Calls.size(),
                      Witness.NoReturnAnswers.size()))
      return;
    size_t EstimatedContext = sizeof(Context);
    if (!accountFrozenInputs(EstimatedContext, B))
      return;
    try {
      // The completed builder no longer needs geometric growth capacity.
      // Keeping that slack would evict live bodies much sooner than the old
      // copied projection. Compact only after bounded admission, then charge
      // actual capacities again: shrink_to_fit is a nonbinding request.
      for (auto &Block : F.Blocks) {
        Block.Ops.shrink_to_fit();
        Block.InstructionBoundaries.shrink_to_fit();
        Block.Succs.shrink_to_fit();
        Block.Preds.shrink_to_fit();
        Block.ExceptionalSuccs.shrink_to_fit();
        Block.ExceptionalPreds.shrink_to_fit();
      }
      F.Blocks.shrink_to_fit();
      const auto BodyBytes =
          summaryBytes(F.Blocks, F.Blocks.capacity(), Witness.Calls.size(),
                       Witness.NoReturnAnswers.size());
      if (!BodyBytes || *BodyBytes > MaxSummaryBytes - EstimatedContext)
        return;
      auto MetadataBytes = functionMetadataBytes(F);
      if (F.ExceptionMetadata.has_value() != (Witness.Exception != nullptr) ||
          (MetadataBytes &&
           *MetadataBytes > MaxSummaryBytes - EstimatedContext - *BodyBytes))
        MetadataBytes.reset();
      auto Facts = std::make_shared<SummaryFacts>();
      Facts->Body.Complete = Covered;
      Facts->Body.UnsafeIndirectBranches =
          !F.UnsafeIndirectBranchAddresses.empty();
      Facts->Body.HasIndependentRoots =
          CallRegisterBodyView(F).HasIndependentRoots;
      if (MetadataBytes) {
        Facts->Function = std::make_unique<LowFunc>(std::move(F));
        Facts->Body.Blocks = std::move(Facts->Function->Blocks);
        Facts->Exception = Witness.Exception;
        Facts->Function->ExceptionMetadata.reset();
      } else {
        Facts->Body.Blocks = std::move(F.Blocks);
      }
      Facts->Calls.assign(Witness.Calls.begin(), Witness.Calls.end());
      Facts->NoReturnAnswers = Witness.NoReturnAnswers;
      auto Bytes = summaryBytes(
          Facts->Body.Blocks, Facts->Body.Blocks.capacity(),
          Facts->Calls.capacity(), Facts->NoReturnAnswers.capacity());
      if (!Bytes || *Bytes > MaxSummaryBytes - EstimatedContext)
        return;
      if (Facts->Function) {
        MetadataBytes = functionMetadataBytes(*Facts->Function);
        if (!MetadataBytes ||
            *MetadataBytes > MaxSummaryBytes - EstimatedContext - *Bytes) {
          Facts->Function.reset();
          Facts->Exception = nullptr;
        } else {
          *Bytes += *MetadataBytes;
        }
      }
      std::lock_guard<std::mutex> Lock(Mutex);
      if (!SummaryInputs || !matches(*SummaryInputs, B)) {
        Context Next = freezeContext(B);
        const auto NextBytes = contextBytes(Next);
        if (!NextBytes || *Bytes > MaxSummaryBytes - *NextBytes)
          return;
        Summaries.clear();
        SummaryIndex.clear();
        SummaryInputs = std::move(Next);
        SummaryBytes = *NextBytes;
      }
      const auto InputBytes = contextBytes(*SummaryInputs);
      if (!InputBytes || *Bytes > MaxSummaryBytes - *InputBytes)
        return;
      if (const auto Found = SummaryIndex.find(Address);
          Found != SummaryIndex.end()) {
        Summaries.splice(Summaries.begin(), Summaries, Found->second);
        return;
      }
      const bool NeedsRoom = Summaries.size() >= MaxGraphs ||
                             *Bytes > MaxSummaryBytes - SummaryBytes;
      if (NeedsRoom && KeepResident) {
        RefusedResident = true;
        return;
      }
      while (!Summaries.empty() && (Summaries.size() >= MaxGraphs ||
                                    *Bytes > MaxSummaryBytes - SummaryBytes)) {
        SummaryBytes -= Summaries.back().Bytes;
        SummaryIndex.erase(Summaries.back().Address);
        Summaries.pop_back();
      }
      Summaries.push_front({Address, std::move(Facts), *Bytes});
      SummaryIndex[Address] = Summaries.begin();
      SummaryBytes += *Bytes;
    } catch (const std::bad_alloc &) {
      // The ordinary build remains authoritative when retention is unavailable.
    }
  }
  std::shared_ptr<const SummaryFacts> summary(const CFGBuilder &B,
                                              va_t Address) {
    if (!canRetainSummary(B))
      return {};
    std::lock_guard<std::mutex> Lock(Mutex);
    if (!SummaryInputs || !matches(*SummaryInputs, B))
      return {};
    if (const auto Found = SummaryIndex.find(Address);
        Found != SummaryIndex.end()) {
      Summaries.splice(Summaries.begin(), Summaries, Found->second);
      return Summaries.front().Facts;
    }
    return {};
  }
  /// Whether validatedSummary refuses a body for its call to \p Target:
  /// where the image owns the x87 context the callee uses it, or the call
  /// pops a register.  An answer the queries cannot give refuses nothing.
  bool refusesCall(const CFGBuilder &B, va_t Target);
  std::shared_ptr<const SummaryFacts> validatedSummary(
      const std::shared_ptr<X87CallGraphCache> &Cache, const BinaryImage &Img,
      va_t Entry, const std::set<va_t> *Entries,
      const libc::NoReturnTargetIndex *NoReturnTargets,
      const NoReturnCalleeProver *NoReturnCallees,
      const detail::AbsoluteRelocationRootIndex *RelocationRoots,
      const ExecutableCodeOwnerIndex *CodeOwners, bool RequireFunction);
  bool account(size_t &Bytes, size_t Count, size_t Width) const {
    if (Bytes > MaxBytes || Count > (MaxBytes - Bytes) / Width)
      return false;
    Bytes += Count * Width;
    return true;
  }
  bool accountAllocation(size_t &Bytes, size_t Count, size_t Width) const {
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
  static bool matches(const Context &C, const CFGBuilder &B) {
    return C.Image == B.CurrentImg && C.Architecture == B.CurrentImg->Arch &&
           C.Mode == B.CurrentImg->Mode && C.Bits == B.CurrentImg->Bits &&
           C.Format == B.CurrentImg->Format &&
           C.ABI == B.CurrentImg->abiFormat() &&
           C.NoReturnTargets == B.NoReturnTargets &&
           C.NoReturnCallees == B.NoReturnCallees &&
           C.NoReturnDepth == B.NoReturnCalleeDepth + 1 &&
           C.RelocationRoots == B.AbsoluteRelocationRoots &&
           C.CodeOwners == B.ExecutableCodeOwners &&
           // The immutable inventory a context borrows still holds what it
           // held; any other set is compared entry by entry.
           (C.EntriesSource ? B.KnownFuncEntries &&
                                  (C.EntriesSource == B.KnownFuncEntries ||
                                   *C.EntriesSource == *B.KnownFuncEntries)
                            : sameSet(C.Entries, B.KnownFuncEntries)) &&
           sameSet(C.ProtectedSlots, B.ProtectedJumpTableRelocationSlots) &&
           sameSet(C.UnsafeBranches, B.UnsafeJumpTableBranches);
  }
  bool matches(const CFGBuilder &B) const {
    return Inputs && matches(*Inputs, B);
  }
  std::optional<size_t> graphBytes(const X87CallGraph &Graph) const {
    size_t Bytes = sizeof(Entry) + sizeof(X87CallGraph) + 16 * sizeof(void *);
    if (!accountAllocation(Bytes, Graph.Blocks.capacity(),
                           sizeof(X87CallGraphBlock)) ||
        !accountAllocation(Bytes, Graph.NoReturnAnswers.capacity(),
                           sizeof(X87NoReturnAnswer)))
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
  std::optional<size_t> contextBytes(const Context &C) const {
    size_t Bytes = sizeof(Context);
    for (const auto *Values :
         {&C.Entries, &C.ProtectedSlots, &C.UnsafeBranches})
      if (*Values &&
          !accountAllocation(Bytes, (*Values)->capacity(), sizeof(va_t)))
        return std::nullopt;
    return Bytes;
  }
  /// Whether \p B's entries are the cache's immutable inventory, which a
  /// context borrows instead of copying: tens of thousands of entries on a
  /// large image, frozen again for every graph a request misses.
  bool borrowsEntries(const CFGBuilder &B) const {
    return ImmutableEntries && B.KnownFuncEntries == ImmutableEntries;
  }
  /// Charge \p Bytes for the inputs a context frozen from \p B copies.
  bool accountFrozenInputs(size_t &Bytes, const CFGBuilder &B) const {
    for (const auto *Values :
         {borrowsEntries(B) ? nullptr : B.KnownFuncEntries,
          B.ProtectedJumpTableRelocationSlots, B.UnsafeJumpTableBranches})
      if (Values && !accountAllocation(Bytes, Values->size(), sizeof(va_t)))
        return false;
    return true;
  }
  Context freezeContext(const CFGBuilder &B) const {
    const bool Borrowed = borrowsEntries(B);
    return {B.CurrentImg,
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
            Borrowed ? std::nullopt : freeze(B.KnownFuncEntries),
            freeze(B.ProtectedJumpTableRelocationSlots),
            freeze(B.UnsafeJumpTableBranches),
            Borrowed ? ImmutableEntries : nullptr};
  }
  Request acquire(const CFGBuilder &B, va_t Address) {
    std::unique_lock<std::mutex> Lock(Mutex);
    if (matches(B))
      if (const auto Found = GraphIndex.find(Address);
          Found != GraphIndex.end()) {
        Graphs.splice(Graphs.begin(), Graphs, Found->second);
        ++Hits;
        return {Graphs.front().Value, {}};
      }
    // Arbitrary prover/name-resolver callbacks may dispatch a request to
    // another thread and wait for it. Only explicitly registered callback
    // owners permit waiting. Nested construction never waits on any cache.
    if (CurrentX87CacheBuild ||
        (B.NoReturnCallees && B.NoReturnCallees != NonReentrantProver) ||
        (B.NoReturnTargets && B.NoReturnTargets != NonReentrantTargets)) {
      ++NonWaitingMisses;
      return {};
    }
    for (const auto &Flight : Pending)
      if (Flight->Address == Address && matches(Flight->Inputs, B)) {
        // Keep the state alive even after its leader removes the pending slot.
        const auto Waiting = Flight;
        ++Waits;
        PendingChanged.notify_all();
        Waiting->Ready.wait(Lock, [&] { return Waiting->Done; });
        if (Waiting->Value)
          ++Hits;
        // A failure/incomplete graph resumes the original independent build,
        // without re-entering acquire or sharing the failed proof result.
        return {Waiting->Value, {}};
      }
    if (Pending.size() >= MaxPendingGraphs) {
      ++NonWaitingMisses;
      return {};
    }
    size_t Estimated = sizeof(PendingGraph) + 8 * sizeof(void *);
    if (!accountFrozenInputs(Estimated, B))
      return {};
    if (Estimated > MaxPendingBytes - PendingBytes) {
      ++NonWaitingMisses;
      return {};
    }
    try {
      Context Frozen = freezeContext(B);
      const auto ContextSize = contextBytes(Frozen);
      if (!ContextSize)
        return {};
      const size_t Bytes = *ContextSize + sizeof(PendingGraph) -
                           sizeof(Context) + 8 * sizeof(void *);
      if (Bytes > MaxPendingBytes - PendingBytes) {
        ++NonWaitingMisses;
        return {};
      }
      auto Flight =
          std::make_shared<PendingGraph>(std::move(Frozen), Address, Bytes);
      Pending.push_back(Flight);
      PendingBytes += Bytes;
      ++Leaders;
      return {{}, std::move(Flight)};
    } catch (const std::bad_alloc &) {
      // Single-flight admission is optional. No pending state was published
      // before all allocations succeeded, so this retains the cold path.
      ++NonWaitingMisses;
      return {};
    }
  }
  void finish(const std::shared_ptr<PendingGraph> &Flight,
              std::shared_ptr<const X87CallGraph> Value) noexcept {
    std::lock_guard<std::mutex> Lock(Mutex);
    Flight->Value = std::move(Value);
    Flight->Done = true;
    for (auto It = Pending.begin(); It != Pending.end(); ++It)
      if (*It == Flight) {
        PendingBytes -= Flight->Bytes;
        Pending.erase(It);
        break;
      }
    // Waiters only need completion and its immutable result after removal.
    // Release the frozen inventories before their private references drain.
    Flight->Inputs.Entries.reset();
    Flight->Inputs.ProtectedSlots.reset();
    Flight->Inputs.UnsafeBranches.reset();
    Flight->Ready.notify_all();
    PendingChanged.notify_all();
  }
  class Completion {
    X87CallGraphCache *Cache;
    std::shared_ptr<PendingGraph> Flight;
    std::shared_ptr<const X87CallGraph> Value;
    ActiveX87CacheBuild Active;

  public:
    Completion(X87CallGraphCache *Owner, std::shared_ptr<PendingGraph> Pending)
        : Cache(Owner), Flight(std::move(Pending)),
          Active{CurrentX87CacheBuild} {
      if (Flight)
        CurrentX87CacheBuild = &Active;
    }
    Completion(const Completion &) = delete;
    Completion &operator=(const Completion &) = delete;
    ~Completion() {
      if (Flight) {
        CurrentX87CacheBuild = Active.Previous;
        Cache->finish(Flight, std::move(Value));
      }
    }
    void completed(std::shared_ptr<const X87CallGraph> Graph) {
      if (!Flight ||
          (!Graph->Complete &&
           Graph->Rejection == X87CallGraph::ProjectionRejection::None))
        return;
      const auto Bytes = Cache->graphBytes(*Graph);
      const auto ContextSize = Cache->contextBytes(Flight->Inputs);
      if (Bytes && ContextSize && *Bytes <= Cache->MaxBytes - *ContextSize)
        Value = std::move(Graph);
    }
  };
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
    if (!accountFrozenInputs(Estimated, B))
      return Graph;
    if (*Bytes > MaxBytes - Estimated)
      return Graph;
    std::lock_guard<std::mutex> Lock(Mutex);
    if (!matches(B)) {
      Context Next = freezeContext(B);
      const auto NextBytes = contextBytes(Next);
      if (!NextBytes || *Bytes > MaxBytes - *NextBytes)
        return Graph;
      Graphs.clear();
      GraphIndex.clear();
      Inputs = std::move(Next);
      RetainedBytes = *NextBytes;
    }
    const auto InputBytes = contextBytes(*Inputs);
    if (!InputBytes || *Bytes > MaxBytes - *InputBytes)
      return Graph;
    // Another worker may have published while this worker built outside the
    // lock. Use its identical immutable projection without waiting on builds.
    // An incomplete-lift refusal is identical only while its answers hold;
    // this build read the answers of now, so it replaces the refusal.
    if (const auto Found = GraphIndex.find(Address);
        Found != GraphIndex.end()) {
      if (Found->second->Value->Rejection !=
          X87CallGraph::ProjectionRejection::IncompleteLift) {
        Graphs.splice(Graphs.begin(), Graphs, Found->second);
        return Graphs.front().Value;
      }
      RetainedBytes -= Found->second->Bytes;
      Graphs.erase(Found->second);
      GraphIndex.erase(Found);
    }
    if ((Graphs.size() >= MaxGraphs || *Bytes > MaxBytes - RetainedBytes) &&
        KeepResident) {
      RefusedResident = true;
      return Graph;
    }
    while (!Graphs.empty() &&
           (Graphs.size() >= MaxGraphs || *Bytes > MaxBytes - RetainedBytes)) {
      RetainedBytes -= Graphs.back().Bytes;
      GraphIndex.erase(Graphs.back().Address);
      Graphs.pop_back();
    }
    Graphs.push_front({Address, Graph, *Bytes});
    GraphIndex[Address] = Graphs.begin();
    RetainedBytes += *Bytes;
    return Graph;
  }
};

std::shared_ptr<X87CallGraphCache> createX87CallGraphCache() {
  return createX87CallGraphCache(nullptr, nullptr);
}

std::shared_ptr<X87CallGraphCache>
createX87CallGraphCache(const NoReturnCalleeProver *NonReentrantProver,
                        const libc::NoReturnTargetIndex *NonReentrantTargets) {
  return createX87CallGraphCache(NonReentrantProver, NonReentrantTargets,
                                 false);
}

std::shared_ptr<X87CallGraphCache>
createX87CallGraphCache(const NoReturnCalleeProver *NonReentrantProver,
                        const libc::NoReturnTargetIndex *NonReentrantTargets,
                        bool RetainCallRegisterBodies) {
  return createX87CallGraphCache(NonReentrantProver, NonReentrantTargets,
                                 RetainCallRegisterBodies,
                                 X87CallGraphCacheLimits());
}

std::shared_ptr<X87CallGraphCache> createX87CallGraphCache(
    const NoReturnCalleeProver *NonReentrantProver,
    const libc::NoReturnTargetIndex *NonReentrantTargets,
    bool RetainCallRegisterBodies, const X87CallGraphCacheLimits &Limits,
    const std::set<va_t> *ImmutableEntries,
    std::map<std::string, uint16_t> DeclaredFloatingImports) {
  return std::make_shared<X87CallGraphCache>(
      NonReentrantProver, NonReentrantTargets, RetainCallRegisterBodies, Limits,
      ImmutableEntries, std::move(DeclaredFloatingImports));
}

std::array<size_t, 3> CFGBuilder::x87CallGraphCacheStatsForTesting() const {
  if (!SharedX87CallGraphs)
    return {};
  std::lock_guard<std::mutex> Lock(SharedX87CallGraphs->Mutex);
  return {SharedX87CallGraphs->Hits, SharedX87CallGraphs->Graphs.size(),
          SharedX87CallGraphs->RetainedBytes};
}

std::array<size_t, 2> CFGBuilder::callRegisterBodyCacheStatsForTesting() const {
  if (!SharedX87CallGraphs)
    return {};
  std::lock_guard<std::mutex> Lock(SharedX87CallGraphs->Mutex);
  return {SharedX87CallGraphs->Summaries.size(),
          SharedX87CallGraphs->SummaryBytes};
}

std::array<size_t, 5> CFGBuilder::x87CallGraphFlightStatsForTesting() const {
  if (!SharedX87CallGraphs)
    return {};
  std::lock_guard<std::mutex> Lock(SharedX87CallGraphs->Mutex);
  return {SharedX87CallGraphs->Leaders, SharedX87CallGraphs->Waits,
          SharedX87CallGraphs->NonWaitingMisses,
          SharedX87CallGraphs->Pending.size(),
          SharedX87CallGraphs->PendingBytes};
}

bool CFGBuilder::waitForX87CallGraphWaitersForTesting(size_t Count) const {
  if (!SharedX87CallGraphs)
    return false;
  std::unique_lock<std::mutex> Lock(SharedX87CallGraphs->Mutex);
  return SharedX87CallGraphs->PendingChanged.wait_for(
      Lock,
      std::chrono::seconds(
          ::neverd::limits::ir::
              kCFGBuilderX86FpuCFGBuilderWaitForX87CallGraphWaitersForTestingSeconds),
      [&] { return SharedX87CallGraphs->Waits >= Count; });
}

/// The index lives for one CFG build of an unchanged image. Its cached local
/// graphs contain machine facts, not answers obtained under a caller's budget.
/// Each query pays for its complete call closure and every dataflow transfer.
struct X87CallContract {
  int Delta = 0;
  bool ConsumesInput = false;
  unsigned ScratchSlots = 0;
  unsigned ImportArguments = 0;
};

class X87CallEffectIndex {
  friend class CFGBuilder;
  using Step = X87CallGraphStep;
  using Block = X87CallGraphBlock;
  using Graph = X87CallGraph;
  struct Query {
    size_t Remaining = limits::kMaxX87CallProofWork;
    std::set<va_t> Charged, Active;
    std::map<std::pair<va_t, unsigned>, std::optional<X87CallContract>> Results;
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
  std::map<va_t, std::optional<X87CallContract>> Answers;
  /// Set when this index read an incomplete lift whose no-return answers
  /// went unrecorded, so nothing derived from it can name what it depends on.
  bool UnrecordedLift = false;

  std::optional<int> importEffect(llvm::StringRef Name) const {
    if (Image.Arch == Arch::X86 && Settings.SharedX87CallGraphs) {
      const auto &Declared =
          Settings.SharedX87CallGraphs->DeclaredFloatingImports;
      if (const auto It = Declared.find(Name.str());
          It != Declared.end() &&
          (It->second == sizeof(float) || It->second == sizeof(double)))
        return -1;
    }
    bool Floating = false, LongDouble = false, Complex = false;
    if (const auto *Prototype =
            libc::libcPrototype(Name.str(), Image.abiFormat())) {
      Floating = libc::isFloatingType(Prototype->Return);
      LongDouble = Prototype->Return == "long double";
    } else if (const auto Arity =
                   importNamesAreCNames(Image.Format)
                       ? libc::libcArity(Name.str(), libc::libcVariantOf(Image))
                       : libc::libcArityForSymbol(Name.str(),
                                                  libc::libcVariantOf(Image))) {
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

  static bool modeledIntrinsic(const LowOp &Op, Arch Architecture) {
    if (Op.NumInputs == 0 || !Op.Inputs[0].isConst())
      return false;
    if (isArchitecturalNoReturn(Op))
      return true;
    // SSE arithmetic under MXCSR touches no x87 register: i386 code computes
    // a double in an XMM register and returns it on the x87 stack.
    if (isX86FPStateIntrinsic(static_cast<Intrinsic>(Op.Inputs[0].Offset)))
      return true;
    if (static_cast<Intrinsic>(Op.Inputs[0].Offset) == Intrinsic::X86AMXState)
      return x86AmxShapeIsValid(x86AmxLowShape(Op, Architecture));
    if (isX86BoundedLeafIntrinsic(
            static_cast<Intrinsic>(Op.Inputs[0].Offset)) ||
        isX86BoundedLeafPending(static_cast<Intrinsic>(Op.Inputs[0].Offset)))
      return x86BoundedLeafShapeIsValid(
          static_cast<Intrinsic>(Op.Inputs[0].Offset),
          x86BoundedLeafLowShape(Op, Architecture));
    if (isX86SgxIntrinsic(static_cast<Intrinsic>(Op.Inputs[0].Offset)))
      return x86SgxShapeIsValid(static_cast<Intrinsic>(Op.Inputs[0].Offset),
                                x86SgxLowShape(Op, Architecture));
    // Bounded system operations do not change the x87 stack on completion.
    if (isX86SystemStateIntrinsic(static_cast<Intrinsic>(Op.Inputs[0].Offset)))
      return x86SystemStateShapeIsValid(
          static_cast<Intrinsic>(Op.Inputs[0].Offset),
          x86SystemStateLowShape(Op, Architecture));
    // These VMX/SVM operations leave x87 state unchanged on fallthrough.
    // VMLAUNCH/VMRESUME reach it only on VM-entry failure. VMRUN, whose guest
    // can change unsaved registers and floating state, is not in this family.
    if (isX86VirtualizationIntrinsic(
            static_cast<Intrinsic>(Op.Inputs[0].Offset)))
      return x86VirtualizationShapeIsValid(
          static_cast<Intrinsic>(Op.Inputs[0].Offset),
          x86VirtualizationLowShape(Op, Architecture));
    // The decoder already accounts for the numeric x87 operations' pops and
    // pushes. Environment restores, tag changes and unknown intrinsics have
    // additional state effects and cannot acquire a net-stack summary here.
    switch (static_cast<Intrinsic>(Op.Inputs[0].Offset)) {
    case Intrinsic::X86RequireDivPrecondition:
      // On its returning path the integer divide guard changes no x87
      // state. Keep the guard itself: this grants only a normal-return stack
      // effect, never permission to suppress a divide fault.
      return Op.Inputs[0].Offset ==
                 static_cast<uint64_t>(Intrinsic::X86RequireDivPrecondition) &&
             intrinsicX86DivPreconditionShapeIsValid(
                 Intrinsic::X86RequireDivPrecondition,
                 x86DivPreconditionLowShape(Op, Architecture));
    case Intrinsic::X87ReadControl:
    case Intrinsic::X87WriteControl:
    case Intrinsic::X87LoadConstant:
    case Intrinsic::X87LoadPackedBCD:
    case Intrinsic::X87StorePackedBCD:
    case Intrinsic::X87FptanPair:
    case Intrinsic::X87FsincosPair:
    case Intrinsic::X87FxtractPair:
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

  /// Whether a request would read \p G's no-return answers again: always for
  /// a complete projection or an immutable refusal, and for an
  /// incomplete-lift refusal when the prover answers each question as before.
  bool answersHold(const Graph &G) const {
    if (G.Rejection != Graph::ProjectionRejection::IncompleteLift)
      return true;
    for (const X87NoReturnAnswer &Answer : G.NoReturnAnswers)
      if (!Settings.NoReturnCallees ||
          Settings.NoReturnCallees->neverReturns(Answer.Target, Answer.Depth) !=
              Answer.NeverReturns)
        return false;
    return true;
  }

  const Graph &graph(va_t Entry) {
    if (const auto It = Graphs.find(Entry); It != Graphs.end())
      return *It->second;
    const bool HasCodeOwner = Image.hasExecutableCodeOwnerAt(Entry);
    auto *RegistrationCallees = Settings.BorrowedRegistrationCallees
                                    ? Settings.BorrowedRegistrationCallees
                                    : Settings.RegistrationCallees.get();
    // Registration and floating-call proofs can request one another. Keep
    // the same in-progress entries, depth and budget across that boundary.
    // These query-local answers cannot be published in the shared graph cache.
    const auto SharedGraphs =
        RegistrationCallees ? nullptr : Settings.SharedX87CallGraphs;
    std::shared_ptr<X87CallGraphCache::PendingGraph> Flight;
    if (HasCodeOwner && SharedGraphs) {
      auto Request = SharedGraphs->acquire(Settings, Entry);
      // A refusal whose answers no longer hold is built again; the build
      // replaces it.  Replay runs outside the cache's lock.
      if (Request.Value && answersHold(*Request.Value))
        return *Graphs.emplace(Entry, std::move(Request.Value)).first->second;
      Flight = std::move(Request.Leader);
    }
    X87CallGraphCache::Completion Complete(SharedGraphs.get(),
                                           std::move(Flight));
    auto Built = std::make_shared<Graph>();
    const auto It = Graphs.emplace(Entry, Built).first;
    Graph &G = *Built;
    if (!HasCodeOwner)
      return G;
    // This register-only projection has no dispatcher invocation domain.
    // Do not construct a registration closure just to discover that its
    // callback graph cannot supply an ordinary floating-call return proof.
    if (const auto *EH = Image.ExceptionMetadata.findFunction(Entry);
        EH && EH->Registration && !EH->Registration->Scopes.empty())
      return G;
    Decoder Dec;
    if (!Dec.init(Image))
      return G;
    CFGBuilder Builder;
    Builder.SkipX87StackFixup = true;
    Builder.setRegistrationCalleeIndex(RegistrationCallees);
    Builder.setKnownFuncEntries(Settings.KnownFuncEntries);
    Builder.setNoReturnTargetIndex(Settings.NoReturnTargets);
    Builder.setNoReturnCalleeProver(Settings.NoReturnCallees,
                                    Settings.NoReturnCalleeDepth + 1);
    Builder.setAbsoluteRelocationRootIndex(Settings.AbsoluteRelocationRoots);
    Builder.setExecutableCodeOwnerIndex(Settings.ExecutableCodeOwners);
    Builder.ProtectedJumpTableRelocationSlots =
        Settings.ProtectedJumpTableRelocationSlots;
    Builder.UnsafeJumpTableBranches = Settings.UnsafeJumpTableBranches;
    CFGBuilder::CallRegisterBodyWitness Witness;
    // Every no-return answer the build reads, with its depth, for a refusal
    // of an incomplete lift; mayNeverReturn screens the image alone.
    std::vector<X87NoReturnAnswer> Answers;
    bool AnswersComplete = true;
    struct RecordingProver : NoReturnCalleeProver {
      const NoReturnCalleeProver &Original;
      CFGBuilder::CallRegisterBodyWitness &Witness;
      std::vector<X87NoReturnAnswer> &Answers;
      bool &AnswersComplete;
      RecordingProver(const NoReturnCalleeProver &P,
                      CFGBuilder::CallRegisterBodyWitness &W,
                      std::vector<X87NoReturnAnswer> &A, bool &Complete)
          : Original(P), Witness(W), Answers(A), AnswersComplete(Complete) {}
      bool mayNeverReturn(va_t Target) const override {
        return Original.mayNeverReturn(Target);
      }
      bool neverReturns(va_t Target, unsigned Depth) const override {
        const bool Result = Original.neverReturns(Target, Depth);
        if (Witness.Complete) {
          if (Witness.NoReturnAnswers.size() >= Witness.MaxDependencies)
            Witness.Complete = false;
          else
            Witness.NoReturnAnswers.emplace_back(Target, Result);
        }
        if (AnswersComplete) {
          if (Answers.size() >= Witness.MaxDependencies)
            AnswersComplete = false;
          else
            Answers.push_back({Target, Depth, Result});
        }
        return Result;
      }
    };
    const bool RetainBody =
        SharedGraphs && SharedGraphs->canRetainSummary(Settings);
    std::optional<RecordingProver> Recorder;
    if (RetainBody)
      Builder.CollectCallRegisterBody = &Witness;
    if (SharedGraphs && Settings.NoReturnCallees) {
      Recorder.emplace(*Settings.NoReturnCallees, Witness, Answers,
                       AnswersComplete);
      Builder.setNoReturnCalleeProver(&*Recorder,
                                      Settings.NoReturnCalleeDepth + 1);
    }
    LowFunc F = Builder.build(Image, Dec, Entry);
    const llvm::scope_exit Retain([&] {
      if (RetainBody)
        SharedGraphs->retainSummary(Settings, Entry, F, Witness);
    });
    auto RejectProjection =
        [&](Graph::ProjectionRejection Reason) -> const Graph & {
      // These exclusions are immutable facts of a completely lifted CFG, not
      // failures of a caller's proof allowance: its blocks and its size.
      // Retain only an empty refusal marker, which the next request reads
      // instead of building the CFG again (a 32-bit printf engine's jump
      // tables take seconds); prove() still rejects !Complete before
      // charging Cost.
      G = Graph{};
      G.Rejection = Reason;
      if (SharedGraphs)
        It->second = SharedGraphs->publish(Settings, Entry, Built);
      Complete.completed(It->second);
      return *It->second;
    };
    // An incomplete lift can follow from a no-return proof that ran out,
    // which another request may finish.  It is retained with the answers its
    // build read, and holds only while a request reads the same (a proof
    // that threw leaves the build above).  Without them, it is not retained.
    if (!F.hasCompleteInstructionLift() || !F.TruncatedPathAddresses.empty() ||
        F.Blocks.empty() || F.Blocks.front().StartAddr != Entry) {
      if (!SharedGraphs || !AnswersComplete) {
        UnrecordedLift = true;
        return G;
      }
      G.Rejection = Graph::ProjectionRejection::IncompleteLift;
      G.NoReturnAnswers = std::move(Answers);
      It->second = SharedGraphs->publish(Settings, Entry, Built);
      Complete.completed(It->second);
      return *It->second;
    }
    G.Blocks.resize(F.Blocks.size());
    for (size_t I = 0; I < F.Blocks.size(); ++I) {
      const LowBlock &Source = F.Blocks[I];
      Block &B = G.Blocks[I];
      if (Source.Id != static_cast<int>(I) ||
          Source.InstructionBoundaries.empty())
        return RejectProjection(Graph::ProjectionRejection::InconsistentBlocks);
      if (!Source.ExceptionalSuccs.empty() || !Source.ExceptionalPreds.empty())
        return RejectProjection(Graph::ProjectionRejection::ExceptionalEdges);
      G.Cost += 1 + Source.Ops.size() + Source.Succs.size();
      if (G.Cost > limits::kMaxX87CallProofWork)
        return RejectProjection(Graph::ProjectionRejection::ExceedsProofWork);
      B.Successors = Source.Succs;
      for (int S : B.Successors)
        if (S < 0 || static_cast<size_t>(S) >= F.Blocks.size())
          return RejectProjection(
              Graph::ProjectionRejection::InconsistentBlocks);
      for (const LowInstructionBoundary &Boundary :
           Source.InstructionBoundaries) {
        const auto RecIt = Builder.Insns.find(Boundary.Address);
        if (RecIt == Builder.Insns.end() ||
            RecIt->second.Size != Boundary.Size ||
            Boundary.FirstOp > Source.Ops.size() ||
            Boundary.OpCount > Source.Ops.size() - Boundary.FirstOp)
          return RejectProjection(
              Graph::ProjectionRejection::InconsistentBlocks);
        const auto &Rec = RecIt->second;
        Step S;
        S.Delta = demaskDelta(Rec.FpuTopOut - Rec.FpuTopIn);
        S.Unknown = Rec.FpuReset;
        for (size_t O = Boundary.FirstOp;
             O < Boundary.FirstOp + Boundary.OpCount; ++O) {
          const LowOp &Op = Source.Ops[O];
          S.ChangesControl |=
              Op.Output.isReg() && Op.Output.Offset == x86reg::FPU_CW;
          if (Op.Opcode == NdOp::INTRINSIC) {
            G.UsesContext |= Op.NumInputs && Op.Inputs[0].isConst() &&
                             isX87ContextIntrinsic(
                                 static_cast<Intrinsic>(Op.Inputs[0].Offset));
            S.Unknown |= !modeledIntrinsic(Op, Image.Arch);
            S.ChangesControl |=
                Op.NumInputs && Op.Inputs[0].isConst() &&
                Op.Inputs[0].Offset ==
                    static_cast<uint64_t>(Intrinsic::X87WriteControl);
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
        if (S.Delta || S.Reads || S.Writes || S.Callee || S.Unknown ||
            S.ChangesControl)
          B.Steps.push_back(S);
      }
      if (B.Successors.empty() && !B.Returns && !B.Stops)
        return RejectProjection(Graph::ProjectionRejection::MissingTerminator);
    }
    G.Complete = true;
    if (SharedGraphs)
      It->second = SharedGraphs->publish(Settings, Entry, Built);
    Complete.completed(It->second);
    return *It->second;
  }

  std::optional<X87CallContract> prove(va_t Entry, unsigned Depth, Query &Q) {
    if (!Q.pay(1) || Depth >= limits::kMaxX87CallProofDepth)
      return std::nullopt;
    const auto Key = std::make_pair(Entry, Depth);
    if (auto It = Q.Results.find(Key); It != Q.Results.end())
      return It->second;
    if (!Q.Active.insert(Entry).second)
      return std::nullopt;
    auto Compute = [&]() -> std::optional<X87CallContract> {
      if (Q.Charged.insert(Entry).second &&
          Q.Charged.size() > limits::kMaxX87CallProofFunctions)
        return std::nullopt;
      if (const auto Name = importCalleeName(Image, Entry); !Name.empty()) {
        if (isX87CRTImportName(Name)) {
          const unsigned Arguments = x87CRTImportArguments(Image, Entry);
          return Arguments
                     ? std::optional<X87CallContract>(X87CallContract{
                           int(Arguments) - 1, false, 8 - Arguments, Arguments})
                     : std::nullopt;
        }
        if (const auto Effect = importEffect(Name))
          return X87CallContract{*Effect, false, unsigned(*Effect == -1)};
        return std::nullopt;
      }
      const Graph &G = graph(Entry);
      if (!G.Complete || !Q.pay(G.Cost))
        return std::nullopt;
      std::map<va_t, X87CallContract> Callees;
      for (const Block &B : G.Blocks)
        for (const Step &S : B.Steps) {
          if (S.Unknown)
            return std::nullopt;
          if (S.Callee && !Callees.count(*S.Callee)) {
            const auto Effect = prove(*S.Callee, Depth + 1, Q);
            if (!Effect)
              return std::nullopt;
            Callees.emplace(*S.Callee, *Effect);
          }
        }
      struct State {
        int Top = 0;
        uint8_t Defined = 0;
        bool InputAlive = true;
      };
      bool NeedsInput = false, TouchesBelowInput = false;
      bool OpaqueStackCall = false, ChangesControl = false;
      int MinimumTop = 0;
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
        auto read = [&](int Position) {
          TouchesBelowInput |= Position > 0;
          if (Value.Defined & (1u << (Position & 7)))
            return true;
          if (Image.Arch == Arch::X86 && Position == 0 && Value.InputAlive) {
            NeedsInput = true;
            return true;
          }
          return false;
        };
        auto discard = [&](int Position) {
          Value.Defined &= uint8_t(~(1u << (Position & 7)));
          if ((Position & 7) == 0)
            Value.InputAlive = false;
        };
        for (const Step &S : B.Steps) {
          ChangesControl |= S.ChangesControl;
          for (int K = 0; K != 8; ++K)
            if ((S.Reads & (1u << K)) && !read(Value.Top + K))
              return std::nullopt;
          const X87CallContract *Call =
              S.Callee ? &Callees.at(*S.Callee) : nullptr;
          if (Call) {
            if (Call->ConsumesInput && !read(Value.Top))
              return std::nullopt;
            for (unsigned K = 0; K < Call->ImportArguments; ++K)
              if (!read(Value.Top + K))
                return std::nullopt;
            // Ordinary calls still clobber ST values in typed SSA. They
            // cannot acquire preservation merely from this TOP summary;
            // only checked consumers transport the incoming argument.
            OpaqueStackCall |= !Call->ConsumesInput;
            MinimumTop =
                std::min(MinimumTop, Value.Top - int(Call->ScratchSlots));
          }
          const int Delta = S.Delta + (Call ? Call->Delta : 0);
          MinimumTop = std::min(MinimumTop, Value.Top + std::min(Delta, 0));
          // Pushes discard stale slots before their new definitions; pops
          // discard them after the instruction's reads and writes.
          for (int K = Delta; K < 0; ++K)
            discard(Value.Top + K);
          for (int K = 0; K != 8; ++K)
            if (S.Writes & (1u << K)) {
              // A push's new destination is below its incoming TOP, even
              // though the decoder records its modulo-eight physical slot.
              const int Position =
                  Value.Top + (S.Delta < 0 && K >= 8 + S.Delta ? K - 8 : K);
              TouchesBelowInput |= Position > 0;
              if ((Position & 7) == 0)
                Value.InputAlive = false;
              Value.Defined |= uint8_t(1u << ((Value.Top + K) & 7));
            }
          for (int K = 0; K < Delta; ++K) {
            TouchesBelowInput |= Value.Top + K > 0;
            discard(Value.Top + K);
          }
          Value.Top += Delta;
          if (Value.Top < -8 || Value.Top > 8)
            return std::nullopt;
          if (Call && Call->Delta == -1)
            Value.Defined |= uint8_t(1u << (Value.Top & 7));
          if (Call && Call->ImportArguments) {
            Value.Defined = uint8_t(1u << (Value.Top & 7));
            if ((Value.Top & 7) == 0)
              Value.InputAlive = false;
          }
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
            const bool InputAlive = Entries[S]->InputAlive && Value.InputAlive;
            if (Meet != Entries[S]->Defined ||
                InputAlive != Entries[S]->InputAlive) {
              Entries[S]->Defined = Meet;
              Entries[S]->InputAlive = InputAlive;
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
        if ((Result && *Result != Out.Top) ||
            (Out.Top != 0 && Out.Top != -1 && Out.Top != 1) ||
            (Out.Top == -1 && !(Out.Defined & (1u << 7))))
          return std::nullopt;
        Result = Out.Top;
      }
      if (!Result || (*Result == 1) != NeedsInput ||
          (NeedsInput && (TouchesBelowInput || OpaqueStackCall ||
                          ChangesControl || MinimumTop < -7)))
        return std::nullopt;
      return X87CallContract{*Result, NeedsInput, unsigned(-MinimumTop)};
    };
    const auto Result = Compute();
    Q.Active.erase(Entry);
    Q.Results.emplace(Key, Result);
    return Result;
  }

public:
  explicit X87CallEffectIndex(const CFGBuilder &Builder)
      : Settings(Builder), Image(*Builder.CurrentImg) {}
  /// The no-return answers of every incomplete lift this index's queries
  /// read: their answers hold while these do.  nullopt when a lift's answers
  /// went unrecorded.
  std::optional<std::vector<X87NoReturnAnswer>> liftDependencies() const {
    if (UnrecordedLift)
      return std::nullopt;
    std::vector<X87NoReturnAnswer> Dependencies;
    for (const auto &[Address, G] : Graphs)
      if (G && G->Rejection == Graph::ProjectionRejection::IncompleteLift)
        Dependencies.insert(Dependencies.end(), G->NoReturnAnswers.begin(),
                            G->NoReturnAnswers.end());
    return Dependencies;
  }
  std::optional<bool> usesContext(va_t Entry) {
    Query Q;
    std::set<va_t> Active;
    std::map<va_t, bool> Done;
    std::function<std::optional<bool>(va_t, unsigned)> Visit =
        [&](va_t Address, unsigned Depth) -> std::optional<bool> {
      if (!Q.pay(1) || Depth >= limits::kMaxX87CallProofDepth ||
          Active.size() >= limits::kMaxX87CallProofFunctions)
        return std::nullopt;
      if (auto It = Done.find(Address); It != Done.end())
        return It->second;
      if (!importCalleeName(Image, Address).empty())
        return false;
      if (!Active.insert(Address).second)
        return std::nullopt;
      llvm::scope_exit Leave([&] { Active.erase(Address); });
      const Graph &G = graph(Address);
      if (!G.Complete || !Q.pay(G.Cost))
        return std::nullopt;
      if (G.UsesContext) {
        Done[Address] = true;
        return true;
      }
      bool Unresolved = false;
      for (const Block &B : G.Blocks)
        for (const Step &S : B.Steps)
          if (S.Callee) {
            const auto Callee = Visit(*S.Callee, Depth + 1);
            // Ownership needs one definite witness, unlike a net TOP proof
            // that must understand every call. An unresolved sibling cannot
            // erase an observed requirement or decide it absent.
            if (Callee && *Callee) {
              Done[Address] = true;
              return true;
            }
            Unresolved |= !Callee;
          }
      if (Unresolved)
        return std::nullopt;
      Done[Address] = false;
      return false;
    };
    return Visit(Entry, 0);
  }
  void prepareCalls(const LowFunc &F, bool ContextOwnership = false) {
    const auto &Cache = Settings.SharedX87CallGraphs;
    if (Settings.X87CallProofThreads <= 1 || workerThreadCount() <= 1 ||
        !Cache || CurrentX87CacheBuild ||
        Settings.BorrowedRegistrationCallees || Settings.RegistrationCallees ||
        (Settings.NoReturnCallees &&
         Settings.NoReturnCallees != Cache->NonReentrantProver) ||
        (Settings.NoReturnTargets &&
         Settings.NoReturnTargets != Cache->NonReentrantTargets))
      return;
    // These are the same queries the normalization walk will make. Collect
    // them before rebasing any operations; loadedCallSlot stops at every call
    // and intrinsic, so rebasing earlier ST registers cannot change its slot.
    std::set<va_t> Seen;
    std::vector<va_t> Targets;
    std::vector<uint64_t> Weights;
    uint64_t Bytes = 0;
    for (const auto &Block : F.Blocks)
      for (const auto &Boundary : Block.InstructionBoundaries) {
        if (!Settings.Insns.count(Boundary.Address) ||
            hasLowInstructionControlFlag(Boundary.ControlFlags,
                                         LowInstructionControlFlag::NoReturn))
          continue;
        for (size_t O = Boundary.FirstOp;
             O < Boundary.FirstOp + Boundary.OpCount && O < Block.Ops.size();
             ++O) {
          const LowOp &Op = Block.Ops[O];
          if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
              !Op.NumInputs)
            continue;
          const auto Target = Op.Inputs[0].isConst()
                                  ? std::optional<va_t>(Op.Inputs[0].Offset)
                                  : loadedCallSlot(F, Block, O, Op.Inputs[0]);
          if (!Target || Answers.count(*Target) || !Seen.insert(*Target).second)
            continue;
          const auto End = Image.getFunctionMetadataEnd(
              *Target, Settings.ExecutableCodeOwners);
          const uint64_t Weight =
              End != InvalidVA && End > *Target
                  ? std::min<uint64_t>(End - *Target, 1u << 20)
                  : 0;
          Bytes += Weight;
          Targets.push_back(*Target);
          Weights.push_back(Weight);
        }
      }
    if (Targets.size() < 2 ||
        Bytes < ::neverd::limits::ir::kCFGBuilderX86FpuPrepareCallsBytesLimit)
      return;
    std::vector<std::optional<X87CallContract>> Values(Targets.size());
    std::vector<std::exception_ptr> Failures(Targets.size());
    parallelForEachWeighted(
        Weights,
        [&](auto Claim, size_t N) {
          X87CallEffectIndex Worker(Settings);
          for (size_t I; (I = Claim()) < N;)
            try {
              if (ContextOwnership)
                Worker.usesContext(Targets[I]);
              else
                Values[I] = Worker.contract(Targets[I]);
            } catch (...) {
              Failures[I] = std::current_exception();
            }
        },
        2, std::min(Settings.X87CallProofThreads, 4u));
    // Each query kept its own recursion state and full work allowance. Only
    // completed answers are published, in the original instruction order,
    // after every worker joins. A failed query retains its original exception.
    for (size_t I = 0; I < Targets.size(); ++I) {
      if (Failures[I])
        std::rethrow_exception(Failures[I]);
      if (!ContextOwnership)
        Answers.emplace(Targets[I], Values[I]);
    }
  }
  std::optional<X87CallContract> contract(va_t Entry) {
    if (auto It = Answers.find(Entry); It != Answers.end())
      return It->second;
    Query Q;
    const auto Result = prove(Entry, 0, Q);
    Answers.emplace(Entry, Result);
    return Result;
  }
};

std::shared_ptr<const X87CallGraphCache::SummaryFacts>
X87CallGraphCache::validatedSummary(
    const std::shared_ptr<X87CallGraphCache> &Cache, const BinaryImage &Img,
    va_t Entry, const std::set<va_t> *Entries,
    const libc::NoReturnTargetIndex *NoReturnTargets,
    const NoReturnCalleeProver *NoReturnCallees,
    const detail::AbsoluteRelocationRootIndex *RelocationRoots,
    const ExecutableCodeOwnerIndex *CodeOwners, bool RequireFunction) {
  if (Img.Arch != Arch::X64 || ReplayingCallRegisterBody)
    return {};
  CFGBuilder Request;
  Request.CurrentImg = &Img;
  Request.setKnownFuncEntries(Entries);
  Request.setNoReturnTargetIndex(NoReturnTargets);
  Request.setNoReturnCalleeProver(NoReturnCallees);
  Request.setAbsoluteRelocationRootIndex(RelocationRoots);
  Request.setExecutableCodeOwnerIndex(CodeOwners);
  Request.setX87CallGraphCache(Cache);
  const auto Facts = summary(Request, Entry);
  if (!Facts || (RequireFunction && !Facts->Function))
    return {};
  // A registered callback can synchronously request a summary itself. Leave
  // nested requests to their ordinary build instead of replaying recursively.
  llvm::SaveAndRestore<bool> Replaying(ReplayingCallRegisterBody, true);
  // Raw floating proofs build one no-return level deeper. Replaying every
  // observed decision at the ordinary depth must reproduce the same CFG,
  // including decisions from earlier speculative stages and build retries.
  for (const auto &[Target, Answer] : Facts->NoReturnAnswers)
    if (!NoReturnCallees || NoReturnCallees->neverReturns(Target, 0) != Answer)
      return {};
  // A wrapper without local stack use can still require context lifetime for
  // its callee. Raw machine bodies must not bypass that normalization. Every
  // query retains the ordinary depth and its full private allowance.
  for (va_t Target : Facts->Calls)
    if (Cache->refusesCall(Request, Target))
      return {};
  return Facts;
}

bool X87CallGraphCache::refusesCall(const CFGBuilder &B, va_t Target) {
  std::optional<CallRefusal> Kept;
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    if (RefusalInputs && matches(*RefusalInputs, B))
      if (const auto Found = Refusals.find(Target); Found != Refusals.end())
        Kept = Found->second;
  }
  // The answer holds while the prover answers as it did for the incomplete
  // lifts the queries read, as answersHold has a refusal graph hold.
  if (Kept && llvm::all_of(Kept->Dependencies, [&](const auto &Answer) {
        return B.NoReturnCallees &&
               B.NoReturnCallees->neverReturns(Answer.Target, Answer.Depth) ==
                   Answer.NeverReturns;
      }))
    return Kept->Refuses;
  // Nested summary requests take their ordinary build, as in validatedSummary.
  llvm::SaveAndRestore<bool> Replaying(ReplayingCallRegisterBody, true);
  X87CallEffectIndex Effects(B);
  // Raw cached bodies precede call operand/result binding. A zero net TOP
  // change can still require x87 arguments, so it cannot bypass fixup either.
  bool Refuses = (ownsX87Context(*B.CurrentImg) &&
                  Effects.usesContext(Target).value_or(false));
  if (!Refuses) {
    const auto Contract = Effects.contract(Target);
    Refuses = (Contract && (Contract->Delta == -1 || Contract->ConsumesInput ||
                            Contract->ImportArguments)) ||
              isX87CRTImportName(importCalleeName(*B.CurrentImg, Target));
  }
  auto Dependencies = Effects.liftDependencies();
  if (!Dependencies || Dependencies->size() > MaxRefusalDependencies)
    return Refuses;
  try {
    std::lock_guard<std::mutex> Lock(Mutex);
    if (!RefusalInputs || !matches(*RefusalInputs, B)) {
      Context Next = freezeContext(B);
      if (!contextBytes(Next))
        return Refuses;
      Refusals.clear();
      RefusalInputs = std::move(Next);
    }
    if (Refusals.size() < MaxRefusals || Refusals.count(Target))
      Refusals.insert_or_assign(Target,
                                CallRefusal{Refuses, std::move(*Dependencies)});
  } catch (const std::bad_alloc &) {
    // Remembering is optional; the answer itself stands.
  }
  return Refuses;
}

CFGBuilder::CallGraphWarmup CFGBuilder::warmCallGraphs(
    const std::shared_ptr<X87CallGraphCache> &Cache, const BinaryImage &Img,
    llvm::ArrayRef<va_t> Functions, const std::set<va_t> *Entries,
    const libc::NoReturnTargetIndex *NoReturnTargets,
    const NoReturnCalleeProver *NoReturnCallees,
    const detail::AbsoluteRelocationRootIndex *RelocationRoots,
    const ExecutableCodeOwnerIndex *CodeOwners, unsigned MaxThreads) {
  CallGraphWarmup Result;
  if (!Cache || Img.Arch != Arch::X64 || Functions.empty())
    return Result;
  // The settings a pipeline build hands its call proofs, so that each graph
  // and body is the one such a proof would build and publish itself.
  CFGBuilder Settings;
  Settings.CurrentImg = &Img;
  Settings.setKnownFuncEntries(Entries);
  Settings.setNoReturnTargetIndex(NoReturnTargets);
  Settings.setNoReturnCalleeProver(NoReturnCallees);
  Settings.setAbsoluteRelocationRootIndex(RelocationRoots);
  Settings.setExecutableCodeOwnerIndex(CodeOwners);
  Settings.setX87CallGraphCache(Cache);
  {
    std::lock_guard<std::mutex> Lock(Cache->Mutex);
    Cache->KeepResident = true;
    Cache->RefusedResident = false;
  }
  const llvm::scope_exit Release([&] {
    std::lock_guard<std::mutex> Lock(Cache->Mutex);
    Cache->KeepResident = false;
  });
  std::vector<uint64_t> Weights(Functions.size(), 0);
  for (size_t I = 0; I < Functions.size(); ++I) {
    const va_t End = Img.getFunctionMetadataEnd(Functions[I], CodeOwners);
    if (End != InvalidVA && End > Functions[I])
      Weights[I] = std::min<uint64_t>(End - Functions[I], 1u << 20);
  }
  std::vector<std::exception_ptr> Failures(Functions.size());
  parallelForEachWeighted(
      Weights,
      [&](auto Claim, size_t N) {
        X87CallEffectIndex Worker(Settings);
        for (size_t I; (I = Claim()) < N;)
          try {
            (void)Worker.graph(Functions[I]);
            // A body that calls the function asks this when it is reused.
            // The answer stays after the cache has no room for more bodies.
            (void)Cache->refusesCall(Settings, Functions[I]);
          } catch (...) {
            Failures[I] = std::current_exception();
          }
      },
      1, std::max(1u, std::min(MaxThreads, 4u)));
  // As prepareCalls does, every worker joins before the first failure is
  // reported.
  for (const std::exception_ptr &Failure : Failures)
    if (Failure)
      std::rethrow_exception(Failure);
  for (va_t Function : Functions)
    Result.Retained += Cache->summary(Settings, Function) != nullptr;
  std::lock_guard<std::mutex> Lock(Cache->Mutex);
  Result.Full = Cache->RefusedResident;
  return Result;
}

std::shared_ptr<const CallRegisterBody> CFGBuilder::cachedCallRegisterBody(
    const std::shared_ptr<X87CallGraphCache> &Cache, const BinaryImage &Img,
    va_t Entry, const std::set<va_t> *Entries,
    const libc::NoReturnTargetIndex *NoReturnTargets,
    const NoReturnCalleeProver *NoReturnCallees,
    const detail::AbsoluteRelocationRootIndex *RelocationRoots,
    const ExecutableCodeOwnerIndex *CodeOwners) {
  if (!Cache)
    return {};
  const auto Facts = Cache->validatedSummary(
      Cache, Img, Entry, Entries, NoReturnTargets, NoReturnCallees,
      RelocationRoots, CodeOwners, /*RequireFunction=*/false);
  return Facts ? std::shared_ptr<const CallRegisterBody>(Facts, &Facts->Body)
               : nullptr;
}

std::optional<LowFunc> CFGBuilder::cachedDefaultFunction(
    const std::shared_ptr<X87CallGraphCache> &Cache, const BinaryImage &Img,
    va_t Entry, const std::string &Name, const std::set<va_t> *Entries,
    const libc::NoReturnTargetIndex *NoReturnTargets,
    const NoReturnCalleeProver *NoReturnCallees,
    const detail::AbsoluteRelocationRootIndex *RelocationRoots,
    const ExecutableCodeOwnerIndex *CodeOwners) {
  if (!Cache)
    return std::nullopt;
  const auto Facts = Cache->validatedSummary(
      Cache, Img, Entry, Entries, NoReturnTargets, NoReturnCallees,
      RelocationRoots, CodeOwners, /*RequireFunction=*/true);
  // Only a complete lift stands in for a selected function's own build.
  if (!Facts || !Facts->Body.Complete)
    return std::nullopt;
  LowFunc Result = *Facts->Function;
  Result.Blocks = Facts->Body.Blocks;
  if (Facts->Exception)
    Result.ExceptionMetadata = *Facts->Exception;
  Result.Name =
      Name.empty() ? (kAutoFuncPrefix + llvm::utohexstr(Entry)).str() : Name;
  return Result;
}

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
  return Result && !Result->ConsumesInput && !Result->ImportArguments
             ? std::optional<int>(Result->Delta)
             : std::nullopt;
}

bool CFGBuilder::hasLocalFpuStackUse() const {
  for (const auto &[Address, Rec] : Insns) {
    if (Rec.FpuReset || Rec.FpuTopIn != Rec.FpuTopOut)
      return true;
    for (const LowOp &Op : Rec.Ops) {
      if (isStReg(Op.Output))
        return true;
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        if (isStReg(Op.Inputs[I]))
          return true;
      if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
          Op.Inputs[0].isConst() &&
          (Op.Inputs[0].Offset == static_cast<uint64_t>(Intrinsic::X87Fxam) ||
           isX87ContextIntrinsic(static_cast<Intrinsic>(Op.Inputs[0].Offset))))
        return true;
    }
  }
  return false;
}

bool CFGBuilder::buildNoReturnProof(const BinaryImage &Img, Decoder &Dec,
                                    va_t Entry, const std::string &Name) {
  // The callee's tail calls are judged as its calls are, at this build's
  // proof depth.
  const auto TailCalleeNeverReturns = [&](va_t Target) {
    return calleeNeverReturns(Img, Target, NoReturnTargets, NoReturnCallees,
                              NoReturnCalleeDepth);
  };
  if (Img.Arch != Arch::X86 && Img.Arch != Arch::X64)
    return lowFunctionNeverReturns(build(Img, Dec, Entry, Name), Img.Arch,
                                   TailCalleeNeverReturns);
  LowFunc Callee;
  {
    llvm::SaveAndRestore<bool> RawStack(SkipX87StackFixup, true);
    Callee = build(Img, Dec, Entry, Name);
  }
  // Without a local floating-stack consumer, normalizing a callee's returned
  // ST(0) changes only its value definition and the unused TOP bookkeeping.
  // Neither is an input to the terminating-path proof. In particular, asking
  // whether a GPR-only wrapper returns must not recursively recover every
  // callee's floating return value. A local stack operation or FXAM can expose
  // that state to the rest of the CFG; retain its ordinary build unchanged.
  if (hasLocalFpuStackUse())
    Callee = build(Img, Dec, Entry, Name);
  return lowFunctionNeverReturns(Callee, Img.Arch, TailCalleeNeverReturns);
}

LowFunc CFGBuilder::buildIntegerFrameProof(const BinaryImage &Img, Decoder &Dec,
                                           va_t Entry,
                                           const std::string &Name) {
  // Frame, stack-cleanup and integer-result proofs read no x87 state, and a
  // call clobbers the same integer registers whichever register carries its
  // result. The caller's registration analysis also runs before its own x87
  // normalization, so its calls keep their decoded EAX results. Normalizing
  // this callee would start a floating call proof whose graphs reach the
  // caller's registration index again, with a fresh budget at each level.
  llvm::SaveAndRestore<bool> RawStack(SkipX87StackFixup, true);
  return build(Img, Dec, Entry, Name);
}

LowFunc CFGBuilder::buildCallSignatureProof(const BinaryImage &Img,
                                            Decoder &Dec, va_t Entry,
                                            const std::string &Name) {
  // Win64 carries scalar FP arguments/results in XMM registers. An owned
  // x87 context transported only by calls changes none of those ABI values,
  // so a signature read need not prove its entire descendant call graph.
  // A local x87/MMX observer can expose that state through ordinary values;
  // keep its full normalization, and retain other ABIs' typed ST0 returns.
  if (Img.abiFormat() != BinaryFormat::COFF || !ownsX87Context(Img))
    return build(Img, Dec, Entry, Name);
  LowFunc Callee = buildIntegerFrameProof(Img, Dec, Entry, Name);
  return hasLocalFpuStackUse() ? build(Img, Dec, Entry, Name)
                               : std::move(Callee);
}

void CFGBuilder::recordSkippedFpuFixup(const LowFunc &Func) {
  if (!CollectCallRegisterBody || !CollectCallRegisterBody->Complete)
    return;
  auto &Witness = *CollectCallRegisterBody;
  for (const LowBlock &Block : Func.Blocks) {
    // Also check operations outside instruction slices: the ordinary fixup's
    // final HasFpu scan sees those, even in an intermediate CFG.
    for (const LowOp &Op : Block.Ops) {
      bool LocalStack = isStReg(Op.Output);
      for (unsigned K = 0; K < Op.NumInputs; ++K)
        LocalStack |= isStReg(Op.Inputs[K]);
      LocalStack |=
          Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
          Op.Inputs[0].isConst() &&
          (Op.Inputs[0].Offset == static_cast<uint64_t>(Intrinsic::X87Fxam) ||
           isX87ContextIntrinsic(static_cast<Intrinsic>(Op.Inputs[0].Offset)));
      if (LocalStack) {
        Witness.Complete = false;
        return;
      }
    }
    for (const LowInstructionBoundary &Boundary : Block.InstructionBoundaries) {
      const auto It = Insns.find(Boundary.Address);
      if (It == Insns.end())
        continue;
      if (It->second.FpuReset || It->second.FpuTopIn != It->second.FpuTopOut) {
        Witness.Complete = false;
        return;
      }
      if (hasLowInstructionControlFlag(Boundary.ControlFlags,
                                       LowInstructionControlFlag::NoReturn))
        continue;
      for (size_t O = Boundary.FirstOp;
           O < Boundary.FirstOp + Boundary.OpCount && O < Block.Ops.size();
           ++O) {
        const LowOp &Op = Block.Ops[O];
        if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
            !Op.NumInputs)
          continue;
        const auto Target = Op.Inputs[0].isConst()
                                ? std::optional<va_t>(Op.Inputs[0].Offset)
                                : loadedCallSlot(Func, Block, O, Op.Inputs[0]);
        if (!Target || Witness.Calls.count(*Target))
          continue;
        if (Witness.Calls.size() >= Witness.MaxDependencies) {
          Witness.Complete = false;
          return;
        }
        Witness.Calls.insert(*Target);
      }
    }
  }
}

void CFGBuilder::fixupFpuStack(
    LowFunc &Func, const std::set<va_t> &SuppressedRelocationRoots) {
  if (SkipX87StackFixup) {
    recordSkippedFpuFixup(Func);
    return;
  }
  const auto IsRoot = [&](const LowBlock &Block) {
    const auto RootWithin = [&](const std::set<va_t> &Roots,
                                bool ApplySuppression) {
      for (auto It = Roots.lower_bound(Block.StartAddr);
           It != Roots.end() && (*It == Block.StartAddr || *It < Block.EndAddr);
           ++It)
        if (!ApplySuppression || !SuppressedRelocationRoots.count(*It))
          return true;
      return false;
    };
    // The image-wide target union retains addresses originally recorded for
    // relocated table slots. Only the final CFG can prove all such sources
    // consumed. Independent address-taking instructions and exception roots
    // remain authoritative even when a table also names the same label.
    return Block.StartAddr == Func.Entry || Block.Preds.empty() ||
           !Block.ExceptionalPreds.empty() ||
           RootWithin(Func.ModuleAnalysisRoots, false) ||
           (CurrentImg && RootWithin(CurrentImg->CodeRefTargets, true));
  };
  // Neither native-context ownership nor a no-return annotation supplies
  // the PE32 CRT argument contract. These paths bypass typed stack fixup.
  if (CurrentImg)
    for (const auto &Block : Func.Blocks)
      for (const auto &Boundary : Block.InstructionBoundaries) {
        if (!ownsX87Context(*CurrentImg) &&
            !hasLowInstructionControlFlag(Boundary.ControlFlags,
                                          LowInstructionControlFlag::NoReturn))
          continue;
        for (size_t I = Boundary.FirstOp;
             I < Boundary.FirstOp + Boundary.OpCount && I < Block.Ops.size();
             ++I) {
          const auto &Op = Block.Ops[I];
          if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
              !Op.NumInputs)
            continue;
          const auto Target =
              Op.Inputs[0].isConst()
                  ? std::optional<va_t>(Op.Inputs[0].Offset)
                  : loadedCallSlot(Func, Block, I, Op.Inputs[0]);
          if (Target &&
              isX87CRTImportName(importCalleeName(*CurrentImg, *Target)))
            UnsupportedInstructionAddresses.insert(Op.Addr);
        }
      }
  if (CurrentImg && ownsX87Context(*CurrentImg)) {
    const auto ContextId = [](const LowOp &Op) {
      return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
                     Op.Inputs[0].isConst()
                 ? static_cast<Intrinsic>(Op.Inputs[0].Offset)
                 : Intrinsic::None;
    };
    bool HasContext = false, Prepared = false;
    const auto MMXSlot = [](const NdVar &V) -> std::optional<unsigned> {
      if (isStReg(V) && V.Size && V.Size <= 8)
        return x86reg::stRegIndex(V.Offset);
      return std::nullopt;
    };
    uint64_t Next = 0;
    bool InvalidTemporary = false;
    const auto ReserveTemporary = [&](uint64_t Offset, uint64_t Bytes) {
      if (Offset > UINT64_MAX - Bytes) {
        InvalidTemporary = true;
        return;
      }
      Next = std::max(Next, Offset + Bytes);
    };
    for (const auto &B : Func.Blocks)
      for (const auto &Op : B.Ops) {
        HasContext |= ContextId(Op) == Intrinsic::X87ContextStep;
        HasContext |= MMXSlot(Op.Output).has_value();
        Prepared |= ContextId(Op) == Intrinsic::X87ContextBegin;
        if (Op.Output.isTemp())
          ReserveTemporary(Op.Output.Offset, Op.Output.Size);
        for (unsigned I = 0; I < Op.NumInputs; ++I) {
          HasContext |= MMXSlot(Op.Inputs[I]).has_value();
          if (Op.Inputs[I].isTemp())
            ReserveTemporary(Op.Inputs[I].Offset, Op.Inputs[I].Size);
        }
      }
    // A returning wrapper can transport x87 state without a local floating
    // instruction. Reuse the bounded raw call graph's ownership question,
    // never manufacture a scalar FP return from a partial stack projection.
    if (!HasContext)
      for (const auto &B : Func.Blocks) {
        if (HasContext)
          break;
        for (size_t I = 0; I < B.Ops.size() && !HasContext; ++I) {
          const auto &Op = B.Ops[I];
          if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
              !Op.NumInputs)
            continue;
          const auto Target = Op.Inputs[0].isConst()
                                  ? std::optional<va_t>(Op.Inputs[0].Offset)
                                  : loadedCallSlot(Func, B, I, Op.Inputs[0]);
          if (!Target)
            continue;
          if (!X87CallEffects)
            X87CallEffects = std::make_shared<X87CallEffectIndex>(*this);
          HasContext |= X87CallEffects->usesContext(*Target).value_or(false);
        }
      }
    if (HasContext) {
      if (Prepared)
        return;
      if (SharedX87CallGraphs && X87CallProofThreads > 1) {
        if (!X87CallEffects)
          X87CallEffects = std::make_shared<X87CallEffectIndex>(*this);
        X87CallEffects->prepareCalls(Func, /*ContextOwnership=*/true);
      }
      // Validate complete coverage before rewriting any block. Inconsistent
      // boundaries cannot silently drop an operation or publish half a graph.
      for (const auto &B : Func.Blocks) {
        size_t End = 0;
        for (const auto &Boundary : B.InstructionBoundaries) {
          if (Boundary.FirstOp != End || End > B.Ops.size() ||
              Boundary.OpCount > B.Ops.size() - End) {
            UnsupportedInstructionAddresses.insert(B.StartAddr);
            return;
          }
          End += Boundary.OpCount;
        }
        if (End != B.Ops.size() || B.InstructionBoundaries.empty()) {
          UnsupportedInstructionAddresses.insert(B.StartAddr);
          return;
        }
      }
      for (const auto &Range : Func.FunctionTemporaries)
        ReserveTemporary(Range.Offset, Range.Bytes);
      if (InvalidTemporary || Next > UINT64_MAX - 8) {
        UnsupportedInstructionAddresses.insert(Func.Entry);
        return;
      }
      const NdVar Handle = NdVar::tmp(Next, 8);
      Func.FunctionTemporaries.push_back({Next, 8});
      std::set<int> RecapturedRoots;
      for (const auto &B : Func.Blocks)
        if (B.StartAddr != Func.Entry && IsRoot(B))
          RecapturedRoots.insert(B.Id);
      const auto Rewrite = [&](NdVar &V) {
        if (V.isReg() && V.Offset == x86reg::X87ContextHandle)
          V = Handle;
      };
      for (auto &B : Func.Blocks) {
        const bool Root = IsRoot(B);
        const bool PublishEdge =
            std::any_of(B.Succs.begin(), B.Succs.end(), [&](int Successor) {
              return RecapturedRoots.count(Successor);
            });
        std::vector<LowOp> Ops;
        const auto emit = [&](Intrinsic Id, va_t Address) {
          LowOp New;
          New.Opcode = NdOp::INTRINSIC;
          New.Addr = Address;
          New.addInput(NdVar::cst(unsigned(Id), 2));
          if (Id == Intrinsic::X87ContextBegin)
            New.Output = Handle;
          else
            New.addInput(Handle);
          Ops.push_back(New);
        };
        // Set while the previous operation was a call whose RETURN follows
        // directly (a tail jump, or a call right before RET).
        bool ReturnsCallState = false;
        for (auto &Boundary : B.InstructionBoundaries) {
          const auto First = Boundary.FirstOp, End = First + Boundary.OpCount;
          if (First > B.Ops.size() || Boundary.OpCount > B.Ops.size() - First) {
            UnsupportedInstructionAddresses.insert(Boundary.Address);
            continue;
          }
          Boundary.FirstOp = Ops.size();
          if (Root && First == 0)
            emit(Intrinsic::X87ContextBegin, Boundary.Address);
          std::set<unsigned> MMXReads, MMXWrites;
          for (size_t I = First; I < End; ++I) {
            const auto &Op = B.Ops[I];
            if (auto Slot = MMXSlot(Op.Output))
              MMXWrites.insert(*Slot);
            for (unsigned J = 0; J < Op.NumInputs; ++J)
              if (auto Slot = MMXSlot(Op.Inputs[J]))
                MMXReads.insert(*Slot);
          }
          const bool MMX = !MMXReads.empty() || !MMXWrites.empty();
          if (MMX) {
            emit(Intrinsic::X87ContextStep, Boundary.Address);
            auto &Wait = Ops.back();
            Wait.addInput(NdVar::cst(0x9b, 4));
            Wait.addInput(NdVar::cst(0, 8));
            Wait.addInput(NdVar::cst(0, 8));
            Wait.addInput(NdVar::cst(Boundary.Address, 8));
            for (unsigned Slot : MMXReads) {
              emit(Intrinsic::X87ContextMMXRead, Boundary.Address);
              Ops.back().addInput(NdVar::cst(Slot, 4));
              Ops.back().Output = NdVar::reg(x86reg::stReg(Slot), 8);
            }
          }
          for (size_t I = First; I < End; ++I) {
            LowOp Op = B.Ops[I];
            Rewrite(Op.Output);
            for (unsigned J = 0; J < Op.NumInputs; ++J)
              Rewrite(Op.Inputs[J]);
            const bool Call =
                Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL;
            const bool ExternalState =
                x87ContextSynchronizesExternalState(ContextId(Op));
            // Such a call hands the callee's x87 state straight back to this
            // function's caller. Nothing reads the context in between, so no
            // refresh/export pair separates the CALL from its RETURN, whose
            // adjacency the ABI passes rely on.
            const bool Returned = Call && I + 1 < B.Ops.size() &&
                                  B.Ops[I + 1].Opcode == NdOp::RETURN;
            if (Call || ExternalState ||
                (Op.Opcode == NdOp::RETURN && !ReturnsCallState) ||
                Op.Opcode == NdOp::INDIR_BR ||
                (PublishEdge &&
                 (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR)))
              emit(Intrinsic::X87ContextExport, Op.Addr);
            Ops.push_back(Op);
            if (ExternalState)
              emit(Intrinsic::X87ContextRefresh, Op.Addr);
            if (Call && !Returned &&
                !hasLowInstructionControlFlag(
                    Boundary.ControlFlags, LowInstructionControlFlag::NoReturn))
              emit(Intrinsic::X87ContextRefresh, Op.Addr);
            ReturnsCallState = Returned;
          }
          if (MMX) {
            emit(Intrinsic::X87ContextMMXEnter, Boundary.Address);
            for (unsigned Slot : MMXWrites) {
              emit(Intrinsic::X87ContextMMXWrite, Boundary.Address);
              Ops.back().addInput(NdVar::cst(Slot, 4));
              Ops.back().addInput(NdVar::reg(x86reg::stReg(Slot), 8));
            }
          }
          if (PublishEdge && End == B.Ops.size() &&
              (Ops.empty() || (Ops.back().Opcode != NdOp::BRANCH &&
                               Ops.back().Opcode != NdOp::COND_BR &&
                               Ops.back().Opcode != NdOp::INDIR_BR)))
            emit(Intrinsic::X87ContextExport, Boundary.Address);
          Boundary.OpCount = Ops.size() - Boundary.FirstOp;
        }
        B.Ops = std::move(Ops);
      }
      return;
    }
  }
  if (SkipX87StackFixup) {
    recordSkippedFpuFixup(Func);
    return;
  }
  if (!CurrentImg)
    return;
  if (CurrentImg->Arch != Arch::X86 && CurrentImg->Arch != Arch::X64)
    return;
  if (Func.Blocks.empty())
    return;

  if (SharedX87CallGraphs && X87CallProofThreads > 1) {
    if (!X87CallEffects)
      X87CallEffects = std::make_shared<X87CallEffectIndex>(*this);
    X87CallEffects->prepareCalls(Func);
  }

  const size_t N = Func.Blocks.size();
  const auto PriorUnsupported = UnsupportedInstructionAddresses;
  std::set<va_t> PriorConsumerRefusals;
  std::vector<int> LiftedIn(N, 0), LiftedOut(N, 0), BlockDelta(N, 0);
  std::vector<bool> HasReset(N, false), HasFpu(N, false);
  std::vector<std::map<va_t, uint8_t>> PoppedSlots(N);
  bool HasTagOperations = false;

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
        if (isX87SignOperation(Op) || isX87IntegerLoad(Op) ||
            isX87GuardedValueOperation(Op)) {
          HasTagOperations = true;
          UnsupportedInstructionAddresses.insert(Op.Addr);
        }
        if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
            Op.Inputs[0].isConst()) {
          const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
          HasTagOperations |= Id == Intrinsic::X87Fxam;
          if (Id == Intrinsic::X87Fxam)
            UnsupportedInstructionAddresses.insert(Op.Addr);
          if (const auto Stack = x87StackEffect(Id))
            RotateOnly |= Stack->TopDelta != 0 && !Stack->Pops;
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
        const auto Contract = X87CallEffects->contract(*Target);
        const bool CRTStackCall =
            isX87CRTImportName(importCalleeName(*CurrentImg, *Target));
        if (CRTStackCall) {
          HasTagOperations = true;
          if (PriorUnsupported.count(Op.Addr))
            PriorConsumerRefusals.insert(Op.Addr);
          UnsupportedInstructionAddresses.insert(Op.Addr);
          if (!Contract || !Contract->ImportArguments || Op.NumInputs != 1)
            continue;
          const unsigned Top = (Rec.FpuTopIn + CallShift) & 7;
          Op.X87ImportArguments = uint8_t(Contract->ImportArguments);
          for (unsigned K = 0; K < Contract->ImportArguments; ++K)
            Op.addInput(
                NdVar::reg(x86reg::stReg((Top + K) & 7), x86reg::FPURegSize));
          for (int K = 0; K < Contract->Delta; ++K)
            PoppedSlots[I][Boundary.Address] |= uint8_t(1u << ((Top + K) & 7));
          Op.Output = NdVar::reg(x86reg::stReg((Top + Contract->Delta) & 7),
                                 x86reg::FPURegSize);
          CallShift += Contract->Delta;
          BlockDelta[I] += Contract->Delta;
          continue;
        }
        if (Contract && Contract->ConsumesInput) {
          HasTagOperations = true;
          if (PriorUnsupported.count(Op.Addr))
            PriorConsumerRefusals.insert(Op.Addr);
          UnsupportedInstructionAddresses.insert(Op.Addr);
          if (Op.NumInputs != 1)
            continue;
          const unsigned Top = (Rec.FpuTopIn + CallShift) & 7;
          Op.ConsumesX87Argument = true;
          Op.X87ScratchSlots = uint8_t(Contract->ScratchSlots);
          Op.addInput(NdVar::reg(x86reg::stReg(Top), x86reg::FPURegSize));
          PoppedSlots[I][Boundary.Address] |= uint8_t(1u << Top);
          ++CallShift;
          ++BlockDelta[I];
          continue;
        }
        if (!Contract || Contract->ConsumesInput || Contract->Delta != -1)
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
  const auto EntryContract = X87CallEffects->contract(Func.Entry);
  Func.ConsumesX87Argument = EntryContract && EntryContract->ConsumesInput;
  const bool ReturnsX87 = EntryContract && !EntryContract->ConsumesInput &&
                          EntryContract->Delta == -1;
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

  auto lowerTagOperations = [&](const std::vector<std::pair<int, int>>
                                    &Origins) {
    if (!HasTagOperations)
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
          const bool SignOperation = isX87SignOperation(Op);
          if (Op.X87ImportArguments) {
            uint8_t Arguments = 0;
            for (unsigned K = 1; K <= Op.X87ImportArguments; ++K)
              Arguments |=
                  uint8_t(1u << x86reg::stRegIndex(Op.Inputs[K].Offset));
            if (Publish &&
                (State.Full & State.Payload & Arguments) == Arguments &&
                (State.Empty | Arguments) == 0xff)
              ProvenTags[{Id, O}] = false;
            // No preservation claim for the CRT's scratch space or control
            // state. Only its explicitly returned value has a known payload.
            const uint8_t Result =
                uint8_t(1u << x86reg::stRegIndex(Op.Output.Offset));
            State = Tags{Result, uint8_t(~Result), Result};
          }
          if (Op.ConsumesX87Argument && Op.NumInputs == 2 &&
              isStReg(Op.Inputs[1])) {
            const unsigned Slot = x86reg::stRegIndex(Op.Inputs[1].Offset);
            uint8_t Scratch = 0;
            for (unsigned K = 1; K <= Op.X87ScratchSlots; ++K)
              Scratch |= uint8_t(1u << ((Slot + 8 - K) & 7));
            if (Publish && ((State.Full & State.Payload) & (1u << Slot)) &&
                (State.Empty & Scratch) == Scratch)
              ProvenTags[{Id, O}] = false;
            // The callee can overwrite its argument before consuming it.
            // Its scratch slots are empty again but their retained payloads
            // also differ; an empty-tag FXAM cannot reuse pre-call bits.
            const uint8_t Clobbered = Scratch | uint8_t(1u << Slot);
            State.Full &= uint8_t(~Clobbered);
            State.Payload &= uint8_t(~Clobbered);
            State.Empty |= Clobbered;
          }
          if (isX87IntegerLoad(Op) && Publish) {
            const unsigned Bit = 1u << x86reg::stRegIndex(Op.Output.Offset);
            if (State.Empty & Bit)
              ProvenTags[{Id, O}] = false;
          }
          if (isX87GuardedValueOperation(Op) && Publish &&
              x87GuardedValueShapeIsValid(
                  x87GuardedValueLowShape(Op, CurrentImg->Arch))) {
            const bool Pair = isX87PushPairIntrinsic(
                static_cast<Intrinsic>(Op.Inputs[0].Offset));
            const bool Load =
                Op.Inputs[0].Offset == unsigned(Intrinsic::X87LoadPackedBCD);
            const NdVar &Slot = Load ? Op.Output : Op.Inputs[1];
            if (isStReg(Slot)) {
              const unsigned Bit = 1u << x86reg::stRegIndex(Slot.Offset);
              const unsigned PushBit =
                  1u << ((x86reg::stRegIndex(Slot.Offset) + 7) & 7);
              if (((Load ? State.Empty : State.Full & State.Payload) & Bit) &&
                  (!Pair || (State.Empty & PushBit)))
                ProvenTags[{Id, O}] = false;
            }
          }
          if (SignOperation && Publish) {
            const unsigned Bit = 1u << x86reg::stRegIndex(Op.Inputs[0].Offset);
            if ((State.Full & State.Payload) & Bit)
              ProvenTags[{Id, O}] = false;
          }
          // A callee returns under its calling convention, the contract the
          // TOP pass above already applies to every call it cannot prove:
          // the slots the call found empty are empty again (a callee that
          // "pops" with FINCSTP would break that), and those it found full
          // are its arguments or clobbers, so unknown. A proven return
          // definition below fills one slot.
          if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
              !Op.ConsumesX87Argument && !Op.X87ImportArguments)
            State = Tags{0, State.Empty, 0};
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
              } else if (const auto Stack = x87StackEffect(Kind);
                         Stack && Stack->FreesOperand) {
                if (Op.NumInputs == 3 && isStReg(Op.Inputs[1]))
                  makeEmpty(x86reg::stRegIndex(Op.Inputs[1].Offset));
                else
                  State = {};
              } else if (replacesX87Tags(Kind)) {
                // A subsequent explicit definition can recover a slot.
                State = {};
              }
            }
          }
          if (isStReg(Op.Output) && !SignOperation) {
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
      // An ordinary call boundary has empty tags but no defined payloads.
      // A checked consumer additionally owns its incoming ST0 parameter;
      // its graph forbids accesses below it and callers prove scratch space.
      // FXAM and sign changes still need a defined payload.
      // Other roots, including exception landings, keep an unknown state.
      const bool CallBoundary =
          Block.StartAddr == Func.Entry && Block.ExceptionalPreds.empty();
      if (IsRoot(Block))
        seed(Block.Id, !CallBoundary              ? Tags{}
                       : Func.ConsumesX87Argument ? Tags{1, 0xfe, 1}
                                                  : Tags{0, 0xff, 0});
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
        if (Op.ConsumesX87Argument || Op.X87ImportArguments ||
            isX87GuardedValueOperation(Op)) {
          if (ProvenTags.count({B.Id, O}))
            Expanded.insert(Op.Addr);
          else {
            Pending.insert(Op.Addr);
            UnsupportedInstructionAddresses.insert(Op.Addr);
          }
        }
        const bool SignOperation = isX87SignOperation(Op);
        const bool IntegerLoad = isX87IntegerLoad(Op);
        if (SignOperation || IntegerLoad ||
            (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
             Op.Inputs[0].isConst() &&
             Op.Inputs[0].Offset ==
                 static_cast<uint64_t>(Intrinsic::X87Fxam))) {
          if (auto It = ProvenTags.find({B.Id, O}); It != ProvenTags.end()) {
            auto Lowered = IntegerLoad ? lowerX87IntegerLoad(Op, NextTemp)
                           : SignOperation
                               ? lowerX87Sign(Op, NextTemp)
                               : lowerX87Examine(Op, It->second, NextTemp);
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
      if (!Pending.count(Address) && !PriorConsumerRefusals.count(Address))
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
    lowerTagOperations(Origins);
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
  lowerTagOperations(Origins);
}

} // namespace neverd

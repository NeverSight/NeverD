//===- HuntEngine.cpp - Dangerous-copy overflow hunt ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/HuntEngine.h"

#include "CopySemantics.h"
#include "SourceSemantics.h"

#include "neverd/debug/DebugContext.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/safety/ArgSlicer.h"
#include "neverd/safety/ObjectModel.h"
#include "neverd/safety/SinkScanner.h"
#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymExec.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSwitch.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>

using namespace neverd;
using namespace neverd::safety;
using namespace neverd::symbolic;
using namespace neverd::solver;

namespace {

void stampSite(Finding &F, const AnalysisInput &In, const MedFunc &Fn,
               const SinkSite &Site) {
  F.Origin = Track::Hunt;
  F.Class = Site.Class;
  F.Function = Fn.Name;
  F.FuncEntry = Site.FuncEntry;
  F.Name = Site.Sink;
  F.Source = Site.Source;
  F.CallVA = Site.CallVA;
  F.Sink = Site.Sink;
  F.ArgIndex = Site.ArgIndex;
  if (In.Dbg) {
    if (auto Loc = In.Dbg->sourceLocation(Site.CallVA);
        Loc && !Loc->File.empty())
      F.SourceLoc = Loc->File + ":" + std::to_string(Loc->Line);
  }
}

solver::SolverOptions solverOpts(const SafetyBudgets &Budgets) {
  solver::SolverOptions Opts;
  if (Budgets.SolverConflicts)
    Opts.Sat.MaxConflicts = Budgets.SolverConflicts;
  return Opts;
}

bool pathFeasible(SymContext &Ctx, const std::vector<SymRef> &Constraints,
                  const SafetyBudgets &Budgets, bool &Unknown) {
  Unknown = false;
  if (Constraints.empty())
    return true;
  SymRef Pred =
      Constraints.size() == 1 ? Constraints[0] : Ctx.mkAnd(Constraints);
  if (std::optional<llvm::APInt> C = Ctx.asConst(Pred))
    return !C->isZero();
  SatResult R = checkSat(Ctx, Pred, nullptr, solverOpts(Budgets));
  if (R == SatResult::Unsat)
    return false;
  if (R == SatResult::Sat)
    return true;
  Unknown = true;
  return true;
}

std::optional<uint64_t> abstractOverflowLen(uint64_t Capacity,
                                            const SafetyBudgets &Budgets) {
  SymContext Ctx;
  const uint32_t W = 64;
  SymRef N = Ctx.mkVar("copy_len", W);
  SymRef Cap = Ctx.mkConst(W, Capacity);
  SymRef Over = Ctx.mkUgt(N, Cap);
  SymRef Prop = Over;
  if (Capacity <= UINT64_MAX - 1) {
    SymRef Small = Ctx.mkUle(N, Ctx.mkConst(W, Capacity + 1));
    SymRef Conj[] = {Over, Small};
    Prop = Ctx.mkAnd(Conj);
  }
  BitVectorModel Model;
  SatResult R = checkSat(Ctx, Prop, &Model, solverOpts(Budgets));
  if (R != SatResult::Sat)
    return std::nullopt;
  if (auto V = Model.value(Ctx, N))
    return V->getZExtValue();
  return Capacity + 1;
}

void addWitness(Finding &F, const SinkEntry &E, uint64_t Length,
                const std::string &TaintSource) {
  F.Witness.push_back({"copy_length", std::to_string(Length)});
  const bool ImplicitLen = E.LenArg < 0 && E.SrcArg >= 0;
  std::string InputName =
      !TaintSource.empty() ? (TaintSource == "argv" ? "argv[1]" : TaintSource)
                           : (ImplicitLen ? "source" : "length");
  if (ImplicitLen || !TaintSource.empty())
    F.Witness.push_back({InputName, std::to_string(Length) + " bytes"});
}

const LowBlock *blockById(const LowFunc &LF, int Id) {
  for (const LowBlock &B : LF.Blocks)
    if (B.Id == Id)
      return &B;
  return nullptr;
}

int blockIdAt(const LowFunc &LF, va_t Addr) {
  if (!Addr)
    return -1;
  for (const LowBlock &B : LF.Blocks)
    for (const LowOp &Op : B.Ops)
      if (Op.Addr == Addr)
        return B.Id;
  for (const LowBlock &B : LF.Blocks)
    if (Addr >= B.StartAddr && Addr < B.EndAddr)
      return B.Id;
  for (const LowBlock &B : LF.Blocks)
    if (B.StartAddr == Addr)
      return B.Id;
  return -1;
}

const MedOp *medOpAt(const MedFunc &F, int BlockId, int OpIdx) {
  for (const MedBlock &B : F.Blocks)
    if (B.Id == BlockId)
      return (OpIdx >= 0 && OpIdx < static_cast<int>(B.Ops.size()))
                 ? &B.Ops[OpIdx]
                 : nullptr;
  return nullptr;
}

const MedCallInfo *medCallAt(const MedFunc &F, va_t Addr) {
  if (!Addr)
    return nullptr;
  for (const MedCallInfo &CI : F.CallInfos)
    if (const MedOp *Op = medOpAt(F, CI.BlockId, CI.OpIdx);
        Op && Op->Addr == Addr)
      return &CI;
  return nullptr;
}

bool isStringLengthCall(llvm::StringRef Name) {
  return llvm::StringSwitch<bool>(stripLeadingUnderscores(Name))
#define SAFETY_CALL_TRAIT(NAME, IS_LENGTH, IS_BOUNDED)                         \
  .Case(NAME, IS_LENGTH != 0)
#include "neverd/safety/SafetyCallTraits.inc"
#undef SAFETY_CALL_TRAIT
      .Default(false);
}

std::optional<uint64_t> literalFormattedBytes(llvm::StringRef Format) {
  uint64_t Bytes = 1; // terminating NUL
  for (size_t I = 0; I < Format.size(); ++I) {
    if (Format[I] != '%') {
      ++Bytes;
      continue;
    }
    if (++I >= Format.size() || Format[I] != '%')
      return std::nullopt;
    ++Bytes;
  }
  return Bytes;
}

struct CountedSourceReturn {
  int BoundArg = -1;
  bool AllowsMinusOne = false;
  uint32_t ReturnBits = 0;
  uint32_t BoundBits = 0;
  bool BoundIsSigned = false;
  int RequiredZeroArg = -1;
  int ZeroResultIfArgZero = -1;
  int ObjectSizeArg = -1;
  int ElementSizeArg = -1;
};

struct CountedSourceOutput {
  int BoundArg = -1;
  int CountPointerArg = -1;
  uint32_t CountBits = 0;
  uint32_t BoundBits = 0;
  uint32_t ReturnBits = 0;
  bool ZeroOnFailure = false;
};

struct BoundedStringOutput {
  int BufferArg = -1;
  int BoundArg = -1;
  uint32_t BoundBits = 0;
  bool BoundIsSigned = false;
  int ObjectSizeArg = -1;
};

struct ReturnedStringOutput {
  int BufferArg = -1;
  int BoundArg = -1;
  uint32_t ReturnBits = 0;
  uint32_t BoundBits = 0;
  bool ProvidesNarrowCStringExtent = false;
};

CountedSourceReturn countedSourceReturn(llvm::StringRef Name,
                                        BinaryFormat Format) {
  const std::string Normalized = SinkCatalog::normalize(Name);
  if (Normalized == "read" || Normalized == "pread" ||
      Normalized == "read_chk" || Normalized == "pread_chk") {
    const bool WindowsRead =
        Normalized == "read" && Format == BinaryFormat::COFF;
    CountedSourceReturn Result{2, true,
                               WindowsRead ? uint32_t(32) : uint32_t(0),
                               WindowsRead ? uint32_t(32) : uint32_t(0)};
    if (Normalized == "read_chk")
      Result.ObjectSizeArg = 3;
    else if (Normalized == "pread_chk")
      Result.ObjectSizeArg = 4;
    return Result;
  }
  if (Normalized == "fread" || Normalized == "fread_unlocked")
    return {2, false, 0, 0, false, -1, 1, -1, 1};
  if (Normalized == "fread_chk" || Normalized == "fread_unlocked_chk")
    return {3, false, 0, 0, false, -1, 2, 1, 2};
  if (Normalized == "recv" || Normalized == "recvfrom" ||
      Normalized == "recv_chk" || Normalized == "recvfrom_chk") {
    const bool WindowsSocket = Format == BinaryFormat::COFF;
    CountedSourceReturn Result{2,
                               true,
                               WindowsSocket ? uint32_t(32) : uint32_t(0),
                               WindowsSocket ? uint32_t(32) : uint32_t(0),
                               WindowsSocket,
                               Normalized.ends_with("_chk") ? 4 : 3};
    if (Normalized.ends_with("_chk"))
      Result.ObjectSizeArg = 3;
    return Result;
  }
  return {};
}

CountedSourceOutput countedSourceOutput(llvm::StringRef Name,
                                        BinaryFormat Format) {
  if (Format == BinaryFormat::COFF &&
      SinkCatalog::normalize(Name) == "ReadFile")
    return {2, 3, 32, 32, 32, true};
  return {};
}

BoundedStringOutput boundedStringOutput(llvm::StringRef Name) {
  const std::string Normalized = SinkCatalog::normalize(Name);
  if (Normalized == "fgets" || Normalized == "fgets_unlocked")
    return {0, 1, 32, true};
  if (Normalized == "fgets_chk" || Normalized == "fgets_unlocked_chk")
    return {0, 2, 32, true, 1};
  return {};
}

ReturnedStringOutput returnedStringOutput(llvm::StringRef Name,
                                          BinaryFormat Format) {
  if (Format != BinaryFormat::COFF)
    return {};
  const std::string Normalized = SinkCatalog::normalize(Name);
  if (Normalized == "GetEnvironmentVariableA")
    return {1, 2, 32, 32, true};
  if (Normalized == "GetEnvironmentVariableW")
    return {1, 2, 32, 32, false};
  return {};
}

uint32_t outputSourceReturnBits(llvm::StringRef Name, BinaryFormat Format) {
  if (Format != BinaryFormat::COFF)
    return 0;
  return llvm::StringSwitch<uint32_t>(SinkCatalog::normalize(Name))
      .Cases({"GetEnvironmentVariableA", "GetEnvironmentVariableW"}, 32)
      .Default(0);
}

bool sourceReturnRequiresPointerWidth(llvm::StringRef Name) {
  return llvm::StringSwitch<bool>(SinkCatalog::normalize(Name))
      .Cases({"getenv", "secure_getenv", "GetCommandLineA", "GetCommandLineW",
              "argv"},
             true)
      .Default(false);
}

bool mayWriteMemory(NdOp Opcode) {
  return Opcode == NdOp::STORE || Opcode == NdOp::ATOMIC_XCHG ||
         Opcode == NdOp::ATOMIC_ADD || Opcode == NdOp::ATOMIC_CMPXCHG;
}

SymRef readLowValue(SymContext &Ctx, SymState &State, const NdVar &Value) {
  if (Value.Size == 0)
    return {};
  const uint16_t Bytes = Value.Size;
  if (Value.isConst())
    return Ctx.mkConst(uint32_t(Bytes) * 8, Value.Offset);
  if (Value.isRam())
    return State.load(Ctx.mkConst(64, Value.Offset), Bytes);
  return State.read(Value.isTemp() ? SymSpace::Temporary : SymSpace::Register,
                    Value.Offset, Bytes);
}

bool writeIsProvablyBeforeSource(SymContext &Ctx, SymState &State,
                                 const LowOp &Op, SymRef Source,
                                 const std::vector<SymRef> &Constraints,
                                 const SafetyBudgets &Budgets) {
  if (!Source.isValid() || Op.NumInputs < 2)
    return false;
  const bool HasAddressSpace = Op.Opcode == NdOp::STORE && Op.NumInputs >= 3;
  const unsigned AddressIndex = HasAddressSpace ? 1 : 0;
  const unsigned ValueIndex = HasAddressSpace ? 2 : 1;
  const SymRef RawAddress = readLowValue(Ctx, State, Op.Inputs[AddressIndex]);
  if (!RawAddress.isValid())
    return false;
  const SymRef Address = Ctx.mkZExtOrTrunc(RawAddress, 64);
  const uint64_t Bytes =
      Op.Inputs[ValueIndex].Size ? Op.Inputs[ValueIndex].Size : 1;
  const SymRef NoOverflow =
      Ctx.mkUle(Address, Ctx.mkConst(64, UINT64_MAX - Bytes));
  const SymRef End = Ctx.mkAdd(Address, Ctx.mkConst(64, Bytes));
  const SymRef Before = Ctx.mkUle(End, Ctx.mkZExtOrTrunc(Source, 64));
  std::vector<SymRef> Counterexample = Constraints;
  Counterexample.push_back(Ctx.mkNot(Ctx.mkAnd(NoOverflow, Before)));
  bool Unknown = false;
  return !pathFeasible(Ctx, Counterexample, Budgets, Unknown) && !Unknown;
}

bool expressionsMustEqual(SymContext &Ctx, SymRef Left, SymRef Right,
                          const std::vector<SymRef> &Constraints,
                          const SafetyBudgets &Budgets) {
  if (!Left.isValid() || !Right.isValid())
    return false;
  Left = Ctx.mkZExtOrTrunc(Left, 64);
  Right = Ctx.mkZExtOrTrunc(Right, 64);
  if (Left == Right)
    return true;
  std::vector<SymRef> Counterexample = Constraints;
  Counterexample.push_back(Ctx.mkNe(Left, Right));
  bool Unknown = false;
  return !pathFeasible(Ctx, Counterexample, Budgets, Unknown) && !Unknown;
}

std::optional<uint64_t>
constantForwardOffset(SymContext &Ctx, SymRef Base, SymRef Address,
                      const std::vector<SymRef> &Constraints,
                      const SafetyBudgets &Budgets) {
  if (!Base.isValid() || !Address.isValid())
    return std::nullopt;
  Base = Ctx.mkZExtOrTrunc(Base, 64);
  Address = Ctx.mkZExtOrTrunc(Address, 64);
  const std::optional<llvm::APInt> Delta =
      Ctx.asConst(Ctx.mkSub(Address, Base));
  if (!Delta)
    return std::nullopt;
  std::vector<SymRef> Wrapped = Constraints;
  Wrapped.push_back(Ctx.mkUlt(Address, Base));
  bool Unknown = false;
  if (pathFeasible(Ctx, Wrapped, Budgets, Unknown) || Unknown)
    return std::nullopt;
  return Delta->getZExtValue();
}

bool expressionsShareInput(SymContext &Ctx, SymRef Left, SymRef Right) {
  if (!Left.isValid() || !Right.isValid())
    return false;
  llvm::SmallVector<uint32_t, 8> LeftVars;
  llvm::SmallVector<uint32_t, 8> RightVars;
  Ctx.collectVars(Left, LeftVars);
  Ctx.collectVars(Right, RightVars);
  size_t LeftIndex = 0;
  size_t RightIndex = 0;
  while (LeftIndex < LeftVars.size() && RightIndex < RightVars.size()) {
    if (LeftVars[LeftIndex] == RightVars[RightIndex])
      return true;
    if (LeftVars[LeftIndex] < RightVars[RightIndex])
      ++LeftIndex;
    else
      ++RightIndex;
  }
  return false;
}

std::optional<uint64_t> storedStringTerminatorBound(
    SymContext &Ctx, SymState &State, const LowOp &Op, SymRef Source,
    const std::vector<SymRef> &Constraints, const SafetyBudgets &Budgets) {
  if (Op.Opcode != NdOp::STORE || Op.NumInputs < 2 || !Source.isValid())
    return std::nullopt;
  const bool HasAddressSpace = Op.NumInputs >= 3;
  const unsigned AddressIndex = HasAddressSpace ? 1 : 0;
  const unsigned ValueIndex = HasAddressSpace ? 2 : 1;
  const SymRef Value = readLowValue(Ctx, State, Op.Inputs[ValueIndex]);
  if (!Value.isValid())
    return std::nullopt;
  const std::optional<llvm::APInt> Constant = Ctx.asConst(Value);
  if (!Constant || Constant->getBitWidth() == 0 ||
      Constant->getBitWidth() % 8 != 0 || !Constant->isZero())
    return std::nullopt;

  const SymRef RawAddress = readLowValue(Ctx, State, Op.Inputs[AddressIndex]);
  if (!RawAddress.isValid())
    return std::nullopt;
  const SymRef Address = Ctx.mkZExtOrTrunc(RawAddress, 64);
  Source = Ctx.mkZExtOrTrunc(Source, 64);
  const uint64_t Bytes = Constant->getBitWidth() / 8;
  const SymRef StoreDoesNotWrap =
      Ctx.mkUle(Address, Ctx.mkConst(64, UINT64_MAX - (Bytes - 1)));
  std::vector<SymRef> Wrapped = Constraints;
  Wrapped.push_back(
      Ctx.mkNot(Ctx.mkAnd(Ctx.mkUge(Address, Source), StoreDoesNotWrap)));
  bool Unknown = false;
  if (pathFeasible(Ctx, Wrapped, Budgets, Unknown) || Unknown)
    return std::nullopt;
  const std::optional<llvm::APInt> Delta =
      Ctx.asConst(Ctx.mkSub(Address, Source));
  if (!Delta || Delta->isNegative())
    return std::nullopt;
  const uint64_t BaseOffset = Delta->getZExtValue();
  if (BaseOffset == UINT64_MAX)
    return std::nullopt;
  return BaseOffset + 1;
}

bool predicateMustHold(SymContext &Ctx, SymRef Predicate,
                       const std::vector<SymRef> &Constraints,
                       const SafetyBudgets &Budgets) {
  if (!Predicate.isValid())
    return true;
  std::vector<SymRef> Counterexample = Constraints;
  Counterexample.push_back(Ctx.mkNot(Predicate));
  bool Unknown = false;
  return !pathFeasible(Ctx, Counterexample, Budgets, Unknown) && !Unknown;
}

bool predicateMayHold(SymContext &Ctx, SymRef Predicate,
                      const std::vector<SymRef> &Constraints,
                      const SafetyBudgets &Budgets) {
  if (!Predicate.isValid())
    return true;
  std::vector<SymRef> Candidate = Constraints;
  Candidate.push_back(Predicate);
  bool Unknown = false;
  return pathFeasible(Ctx, Candidate, Budgets, Unknown) && !Unknown;
}

bool valuesShareUniqueLoadOrigin(const SymState &State, SymRef Left,
                                 SymRef Right) {
  if (!Left.isValid() || !Right.isValid())
    return false;
  const SymState::LoadOrigin *LeftOrigin = State.loadOrigin(Left);
  const SymState::LoadOrigin *RightOrigin = State.loadOrigin(Right);
  return LeftOrigin && RightOrigin && *LeftOrigin == *RightOrigin;
}

std::optional<SymState::LoadOrigin>
uniqueLoadOriginInExpression(const SymContext &Ctx, const SymState &State,
                             SymRef Value) {
  if (!Value.isValid())
    return std::nullopt;

  llvm::SmallVector<SymRef, 16> Worklist{Value};
  llvm::DenseSet<uint32_t> Seen;
  std::optional<SymState::LoadOrigin> Unique;
  while (!Worklist.empty()) {
    const SymRef Current = Worklist.pop_back_val();
    if (!Seen.insert(Current.index()).second)
      continue;
    // A literal can share its interned expression with a value read from
    // memory elsewhere.  Only non-constant data-flow nodes carry occurrence
    // evidence into this use.
    if (!Ctx.isConst(Current))
      for (const SymState::LoadOrigin &Origin : State.loadOrigins(Current)) {
        if (Unique && *Unique != Origin)
          return std::nullopt;
        Unique = Origin;
      }
    for (SymRef Operand : Ctx.operands(Current))
      Worklist.push_back(Operand);
  }
  return Unique;
}

SymRef captureReturn(const LowOp &Op, SymState &State,
                     const AnalysisInput &In) {
  if (Op.Output.isReg())
    return Op.Output.Size ? State.read(SymSpace::Register, Op.Output.Offset,
                                       Op.Output.Size)
                          : SymRef();
  if (Op.Output.isTemp())
    return Op.Output.Size ? State.read(SymSpace::Temporary, Op.Output.Offset,
                                       Op.Output.Size)
                          : SymRef();
  if (!In.Img)
    return {};
  const TargetRegInfo &TRI = getTargetRegInfo(In.Img->Arch);
  const uint16_t Bytes = TRI.PointerSize ? TRI.PointerSize : 8;
  return State.read(SymSpace::Register, TRI.IntReturnReg, Bytes);
}

bool hasSemanticWidth(const AnalysisInput &In, uint32_t ActualBits,
                      uint32_t ExplicitBits = 0) {
  if (ActualBits == 0)
    return false;
  const uint16_t PointerBytes = safety::detail::targetPointerBytes(In.Img);
  const uint32_t CarrierBits = uint32_t(PointerBytes) * 8;
  if (ExplicitBits != 0)
    return ActualBits == ExplicitBits ||
           (CarrierBits > ExplicitBits && ActualBits == CarrierBits);
  return CarrierBits == 0 || ActualBits == CarrierBits;
}

bool returnHasSemanticWidth(const AnalysisInput &In, SymContext &Ctx,
                            SymRef Ret, uint32_t ExplicitBits = 0) {
  return Ret.isValid() && hasSemanticWidth(In, Ctx.width(Ret), ExplicitBits);
}

bool callArgumentHasSemanticWidth(const AnalysisInput &In,
                                  const MedCallInfo &Call, int ArgIndex,
                                  uint32_t ExplicitBits = 0) {
  return ArgIndex >= 0 && ArgIndex < static_cast<int>(Call.Args.size()) &&
         hasSemanticWidth(In, uint32_t(Call.Args[ArgIndex].Size) * 8,
                          ExplicitBits);
}

llvm::DenseSet<int> reverseReachable(const LowFunc &LF, int SinkId) {
  llvm::DenseMap<int, llvm::SmallVector<int, 4>> Pred;
  llvm::DenseSet<int> Ids;
  for (const LowBlock &B : LF.Blocks) {
    Ids.insert(B.Id);
    for (int S : B.Succs)
      Pred[S].push_back(B.Id);
    for (int P : B.Preds)
      Pred[B.Id].push_back(P);
  }
  llvm::DenseSet<int> Reach;
  std::deque<int> Work;
  if (SinkId >= 0) {
    Reach.insert(SinkId);
    Work.push_back(SinkId);
  }
  while (!Work.empty()) {
    int Cur = Work.front();
    Work.pop_front();
    auto It = Pred.find(Cur);
    if (It == Pred.end())
      continue;
    for (int P : It->second)
      if (Ids.count(P) && Reach.insert(P).second)
        Work.push_back(P);
  }
  return Reach;
}

std::optional<int> takenSucc(const LowFunc &LF, const LowBlock &B,
                             const LowOp &Br, SymContext &Ctx, SymExec &Exec) {
  if (Br.NumInputs < 1)
    return std::nullopt;
  SymRef T = Exec.branchTarget();
  if (!T.isValid() && Br.Inputs[0].isConst())
    T = Ctx.mkConst(64, Br.Inputs[0].Offset);
  if (T.isValid())
    if (std::optional<llvm::APInt> Addr = Ctx.asConst(T)) {
      int Id = blockIdAt(LF, Addr->getZExtValue());
      if (Id >= 0 && B.hasSucc(Id))
        return Id;
    }
  return std::nullopt;
}

std::optional<int> entryBlockId(const LowFunc &LF) {
  for (const LowBlock &B : LF.Blocks)
    if (B.StartAddr == LF.Entry)
      return B.Id;
  for (const LowBlock &B : LF.Blocks)
    if (LF.Entry >= B.StartAddr && LF.Entry < B.EndAddr)
      return B.Id;

  llvm::DenseSet<int> HasIncoming;
  for (const LowBlock &B : LF.Blocks) {
    if (!B.Preds.empty())
      HasIncoming.insert(B.Id);
    for (int Succ : B.Succs)
      HasIncoming.insert(Succ);
  }
  std::optional<int> Entry;
  for (const LowBlock &B : LF.Blocks) {
    if (HasIncoming.count(B.Id))
      continue;
    if (Entry)
      return std::nullopt;
    Entry = B.Id;
  }
  return Entry;
}

int otherSucc(const LowBlock &B, int Taken) {
  for (int S : B.Succs)
    if (S != Taken)
      return S;
  return -1;
}

SymRef readCallArgument(const AnalysisInput &In, const MedFunc &Fn,
                        int ArgIndex, const MedCallInfo &CI, SymState &State) {
  SymContext &Ctx = State.context();
  auto fromVar = [&](const MedVar &V) -> SymRef {
    if (V.isConst()) {
      if (V.Size == 0 || V.Size > sizeof(uint64_t))
        return {};
      const uint32_t BitWidth = uint32_t(V.Size) * 8;
      return Ctx.mkConst(BitWidth, V.ConstVal);
    }
    if (V.Kind == MedVar::Reg && V.Size > 0 && V.Size <= sizeof(uint64_t))
      return State.read(SymSpace::Register, V.RegOff, V.Size);
    if (V.Kind == MedVar::Param && V.Size > 0 && V.Size <= sizeof(uint64_t))
      return Ctx.mkVar("param$" + std::to_string(V.Id), uint32_t(V.Size) * 8);
    return SymRef();
  };

  const MedVar *RecoveredArg = nullptr;
  if (ArgIndex >= 0 && ArgIndex < static_cast<int>(CI.Args.size())) {
    const MedVar &Arg = CI.Args[ArgIndex];
    RecoveredArg = &Arg;
    if (Arg.Size == 0 || Arg.Size > sizeof(uint64_t))
      return {};
    if (SymRef R = fromVar(Arg); R.isValid())
      return Ctx.mkZExtOrTrunc(R, 64);
    if (Arg.isConst() || Arg.Kind == MedVar::Reg || Arg.Kind == MedVar::Param)
      return {};
  }

  if (!In.Img)
    return SymRef();
  const TargetRegInfo &TRI = getTargetRegInfo(In.Img->Arch);
  if (TRI.PointerSize == 0 || TRI.PointerSize > sizeof(uint64_t) ||
      ArgIndex < 0)
    return SymRef();

  const bool IsCdecl = In.Img->Arch == Arch::X86 && Fn.CC == CallingConv::CDECL;
  llvm::ArrayRef<uint64_t> Params = IsCdecl ? llvm::ArrayRef<uint64_t>()
                                    : Fn.CC == CallingConv::Win64
                                        ? TRI.Win64ParamRegs
                                        : TRI.IntParamRegs;
  if (ArgIndex < static_cast<int>(Params.size()))
    return Ctx.mkZExtOrTrunc(
        State.read(SymSpace::Register, Params[ArgIndex], TRI.PointerSize), 64);

  const uint64_t StackIndex =
      uint64_t(ArgIndex - static_cast<int>(Params.size()));
  const uint64_t ShadowBytes =
      Fn.CC == CallingConv::Win64
          ? uint64_t(TRI.Win64ParamRegs.size()) * TRI.PointerSize
          : 0;
  if (StackIndex >
      (std::numeric_limits<uint64_t>::max() - ShadowBytes) / TRI.PointerSize)
    return {};
  const uint64_t StackOffset =
      ShadowBytes + StackIndex * uint64_t(TRI.PointerSize);
  const uint16_t Bytes = RecoveredArg ? RecoveredArg->Size : TRI.PointerSize;
  const SymRef StackPointer =
      State.read(SymSpace::Register, TRI.StackPointer, TRI.PointerSize);
  if (!StackPointer.isValid())
    return {};
  const SymRef Address = Ctx.mkAdd(Ctx.mkZExtOrTrunc(StackPointer, 64),
                                   Ctx.mkConst(64, StackOffset));
  const SymRef Loaded = State.load(Address, Bytes);
  return Ctx.mkZExtOrTrunc(Loaded, 64);
}

SymRef readPointerCallArgument(const AnalysisInput &In, const MedFunc &Fn,
                               int ArgIndex, const MedCallInfo &CI,
                               SymState &State) {
  if (!In.Img)
    return {};
  const uint16_t PointerBytes = safety::detail::targetPointerBytes(In.Img);
  if (PointerBytes == 0 || PointerBytes > sizeof(uint64_t))
    return {};
  if (ArgIndex >= 0 && ArgIndex < static_cast<int>(CI.Args.size()) &&
      !safety::detail::hasTargetPointerWidth(In.Img, CI.Args[ArgIndex]))
    return {};
  return readCallArgument(In, Fn, ArgIndex, CI, State);
}

struct ExploreHit {
  enum Kind { Miss, Overflow, InBound, Unresolved } Kind = Miss;
  uint64_t WitnessLen = 0;
  std::string PredText;
  bool Budget = false;
  bool SolverUnknown = false;
  bool SemanticUnknown = false;
  bool MissingLength = false;
  bool Reached = false;
  bool SourceEvidence = false;
};

void considerSink(ExploreHit &Best, SymContext &Ctx, SymRef Pred, SymRef Len,
                  SymRef RuntimeCap, uint64_t Capacity,
                  const SafetyBudgets &Budgets) {
  if (!Len.isValid()) {
    Best.MissingLength = true;
    if (Best.Kind == ExploreHit::Miss)
      Best.Kind = ExploreHit::Unresolved;
    return;
  }

  BitVectorModel Model;
  SymRef WideLen = Ctx.mkZExtOrTrunc(Len, 64);
  SymRef Cap = Ctx.mkConst(64, Capacity);
  SymRef Over = Ctx.mkUgt(WideLen, Cap);
  llvm::SmallVector<SymRef, 4> Parts;
  if (!Ctx.isConstOnes(Pred))
    Parts.push_back(Pred);
  Parts.push_back(Over);
  if (RuntimeCap.isValid())
    Parts.push_back(Ctx.mkUle(WideLen, Ctx.mkZExtOrTrunc(RuntimeCap, 64)));
  // A compact witness for an unconstrained length.  Do not add it when a path
  // predicate already forces a larger overflow — the bound would unsat a real
  // defect.
  if (Ctx.isConstOnes(Pred) && !Ctx.isConst(WideLen) &&
      Capacity <= UINT64_MAX - 1)
    Parts.push_back(Ctx.mkUle(WideLen, Ctx.mkConst(64, Capacity + 1)));
  SymRef Query = Parts.size() == 1 ? Parts[0] : Ctx.mkAnd(Parts);

  SatResult R = checkSat(Ctx, Query, &Model, solverOpts(Budgets));
  if (R == SatResult::Sat) {
    Best.Kind = ExploreHit::Overflow;
    Best.PredText = Ctx.toString(Pred);
    if (auto C = Ctx.asConst(WideLen))
      Best.WitnessLen = C->getZExtValue();
    else if (auto V = Model.value(Ctx, WideLen))
      Best.WitnessLen = V->getZExtValue();
    else {
      std::vector<llvm::APInt> Vals = Model.asVarValues(Ctx);
      Best.WitnessLen = Ctx.eval(WideLen, Vals).getZExtValue();
    }
    return;
  }
  if (R == SatResult::Unsat) {
    if (Best.Kind == ExploreHit::Miss || Best.Kind == ExploreHit::Unresolved)
      Best.Kind = ExploreHit::InBound;
    return;
  }
  Best.SolverUnknown = true;
  if (Best.Kind == ExploreHit::Miss)
    Best.Kind = ExploreHit::Unresolved;
}

ExploreHit exploreSink(const AnalysisInput &In, const SinkCatalog &Cat,
                       const MedFunc &Fn, const SinkSite &Site,
                       const SinkEntry &E, uint64_t Capacity,
                       const SafetyBudgets &Budgets,
                       llvm::StringRef RequiredSource,
                       std::optional<uint64_t> FixedLength = std::nullopt,
                       bool UseImplicitLength = true) {
  ExploreHit Best;
  const LowFunc *LF = In.findLowFunc(Fn.Entry);
  if (!LF || LF->Blocks.empty())
    return Best;

  if (!LF->hasCompleteInstructionLift()) {
    Best.SemanticUnknown = true;
    return Best;
  }

  if (Site.CallVA == 0) {
    Best.SemanticUnknown = true;
    return Best;
  }
  int SinkBlk = blockIdAt(*LF, Site.CallVA);
  if (SinkBlk < 0) {
    Best.SemanticUnknown = true;
    return Best;
  }
  llvm::DenseSet<int> Reach = reverseReachable(*LF, SinkBlk);
  if (Reach.empty())
    for (const LowBlock &B : LF->Blocks)
      Reach.insert(B.Id);

  const unsigned MaxPaths = Budgets.MaxPaths ? Budgets.MaxPaths : 64;
  const unsigned MaxSteps = Budgets.MaxSteps ? Budgets.MaxSteps : (1u << 16);
  const unsigned MaxLoop = Budgets.MaxLoop ? Budgets.MaxLoop : 3;

  const MedCallInfo *CI = Site.CallInfoIndex < Fn.CallInfos.size()
                              ? &Fn.CallInfos[Site.CallInfoIndex]
                              : nullptr;
  const bool Implicit = E.LenArg < 0;

  struct SourceEvent {
    std::string Name;
    SymRef Buffer;
    SymRef Success;
    SymRef ImplicitLen;
    bool ImplicitLenIncludesTerminator = false;
    uint32_t RequiredAssignments = 0;
    SymRef ImplicitLenSuccess;
    bool WritesBuffer = false;
    SymRef ProducedValue;
    SymRef ProducedValueSuccess;
    uint16_t ScalarOutputBytes = 0;
    SymRef PriorScalarValue;
    SymRef WrittenBytes;
  };

  struct Frontier {
    SymState State;
    std::vector<SymRef> Constraints;
    int BlockId = -1;
    unsigned Steps = 0;
    llvm::DenseMap<int, unsigned> Visits;
    SymRef ImplicitLen;
    SymRef ImplicitSource;
    SymRef ImplicitSuccess;
    std::vector<SourceEvent> SourceEvents;
    bool ImplicitFromLength = false;
    bool ImplicitLenIncludesTerminator = false;
    bool SemanticUnknown = false;
    explicit Frontier(SymContext &C) : State(C) {}
  };

  std::vector<SymRegisterRange> Preserved;
  if (In.Img) {
    const TargetRegInfo &TRI = getTargetRegInfo(In.Img->Arch);
    for (const TargetRegisterRange &R : TRI.callPreservedRanges(In.Img->Format))
      Preserved.push_back({R.Offset, R.Bytes});
  }

  SymContext Ctx;
  llvm::SmallVector<Frontier, 8> Pending;
  {
    std::optional<int> Entry = entryBlockId(*LF);
    if (!Entry) {
      Best.SemanticUnknown = true;
      return Best;
    }
    Frontier Start(Ctx);
    Start.BlockId = *Entry;
    Pending.push_back(std::move(Start));
  }

  unsigned Finished = 0;
  while (!Pending.empty() && Finished < MaxPaths) {
    Frontier Cur = std::move(Pending.back());
    Pending.pop_back();
    if (!Reach.count(Cur.BlockId))
      continue;

    const LowBlock *Block = blockById(*LF, Cur.BlockId);
    if (!Block)
      continue;
    if (++Cur.Visits[Cur.BlockId] > MaxLoop) {
      Best.Budget = true;
      continue;
    }

    llvm::SmallVector<Frontier, 2> Forks;
    bool Stopped = false;
    bool HitSink = false;
    {
      SymExec Exec(Ctx, Cur.State);
      Exec.setCallPreservedRegisters(Preserved);
      for (size_t Oi = 0; Oi < Block->Ops.size(); ++Oi) {
        const LowOp &Op = Block->Ops[Oi];
        constexpr size_t LowInputCapacity =
            sizeof(Op.Inputs) / sizeof(Op.Inputs[0]);
        if (Op.NumInputs > LowInputCapacity) {
          Best.SemanticUnknown = true;
          Best.Reached = true;
          ++Finished;
          HitSink = true;
          Stopped = true;
          break;
        }
        const bool IsCall =
            Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL;
        if (++Cur.Steps > MaxSteps) {
          Best.Budget = true;
          Stopped = true;
          break;
        }
        if (std::any_of(Op.Inputs, Op.Inputs + Op.NumInputs,
                        [](const NdVar &Input) { return Input.Size == 0; }))
          Cur.SemanticUnknown = true;

        const bool IsSinkCall = IsCall && Op.Addr == Site.CallVA;
        bool DeferredSinkCall = false;
        if (IsSinkCall) {
          bool Unknown = false;
          if (!pathFeasible(Ctx, Cur.Constraints, Budgets, Unknown)) {
            Stopped = true;
            break;
          }
          if (Unknown) {
            Best.SolverUnknown = true;
            Best.Reached = true;
            ++Finished;
            HitSink = true;
            Stopped = true;
            break;
          }
          if (Cur.SemanticUnknown) {
            Best.SemanticUnknown = true;
            Best.Reached = true;
            ++Finished;
            HitSink = true;
            Stopped = true;
            break;
          }
          SymRef Pred =
              Cur.Constraints.empty()
                  ? Ctx.mkTrue()
                  : (Cur.Constraints.size() == 1 ? Cur.Constraints[0]
                                                 : Ctx.mkAnd(Cur.Constraints));
          SymRef Len =
              FixedLength
                  ? Ctx.mkConst(64, *FixedLength)
                  : (CI ? readCallArgument(In, Fn, E.LenArg, *CI, Cur.State)
                        : SymRef());
          SymRef SinkSource =
              CI ? readCallArgument(In, Fn, E.SrcArg, *CI, Cur.State)
                 : SymRef();
          const std::optional<SymState::LoadOrigin> LengthOrigin =
              !Implicit ? uniqueLoadOriginInExpression(Ctx, Cur.State, Len)
                        : std::nullopt;
          SymRef EventImplicitLen;
          SymRef EventImplicitSuccess;
          SymRef ConditionalLenSuccess;
          bool EventLenIncludesTerminator = false;
          bool EventLenConflict = false;
          bool HaveEventImplicitLen = false;
          bool EventLenUnconditional = false;
          bool HasRequiredSourceEvidence = RequiredSource.empty();
          bool SourceEvidenceUnconditional = false;
          SymRef SourceEvidenceSuccess;
          const bool SameImplicitSource =
              expressionsMustEqual(Ctx, Cur.ImplicitSource, SinkSource,
                                   Cur.Constraints, Budgets) ||
              (Cur.ImplicitFromLength &&
               valuesShareUniqueLoadOrigin(Cur.State, Cur.ImplicitSource,
                                           SinkSource));
          for (const SourceEvent &Event : Cur.SourceEvents) {
            SymRef SameBufferSuccess;
            bool SameBuffer = false;
            if (std::optional<uint64_t> Offset = constantForwardOffset(
                    Ctx, Event.Buffer, SinkSource, Cur.Constraints, Budgets)) {
              if (Event.WrittenBytes.isValid()) {
                const SymRef HasInputByte = Ctx.mkUgt(
                    Event.WrittenBytes,
                    Ctx.mkConst(Ctx.width(Event.WrittenBytes), *Offset));
                SameBufferSuccess = Event.Success.isValid()
                                        ? Ctx.mkAnd(Event.Success, HasInputByte)
                                        : HasInputByte;
                SameBuffer = true;
              } else if (*Offset == 0) {
                SameBufferSuccess = Event.Success;
                SameBuffer = true;
              }
            }
            if (!SameBuffer && SameImplicitSource &&
                expressionsMustEqual(Ctx, Event.Buffer, Cur.ImplicitSource,
                                     Cur.Constraints, Budgets)) {
              SameBufferSuccess = Event.Success;
              SameBuffer = true;
            }
            const bool SameProducedValue =
                !Implicit && Event.ProducedValue.isValid() && Len.isValid() &&
                (expressionsMustEqual(Ctx, Event.ProducedValue, Len,
                                      Cur.Constraints, Budgets) ||
                 expressionsShareInput(Ctx, Event.ProducedValue, Len));
            const bool SameDerivedLength =
                !Implicit && Cur.ImplicitFromLength &&
                Cur.ImplicitLen.isValid() && Len.isValid() && SameBuffer &&
                (expressionsMustEqual(Ctx, Cur.ImplicitLen, Len,
                                      Cur.Constraints, Budgets) ||
                 expressionsShareInput(Ctx, Cur.ImplicitLen, Len));
            SymRef WrittenLoadSuccess;
            bool SameWrittenLoad = false;
            std::optional<uint64_t> WrittenLoadOffset;
            if (LengthOrigin && Event.WrittenBytes.isValid())
              WrittenLoadOffset = constantForwardOffset(
                  Ctx, Event.Buffer, LengthOrigin->Address, Cur.Constraints,
                  Budgets);
            if (WrittenLoadOffset &&
                *WrittenLoadOffset <=
                    UINT64_MAX - uint64_t(LengthOrigin->Bytes)) {
              // Address equality does not establish order.  Re-read the
              // current memory epoch so a value loaded before the source call
              // cannot borrow provenance from the later write.
              const SymRef CurrentLoaded =
                  Cur.State.load(LengthOrigin->Address, LengthOrigin->Bytes);
              SameWrittenLoad = expressionsShareInput(Ctx, CurrentLoaded, Len);
            }
            if (SameWrittenLoad) {
              const uint64_t RequiredBytes =
                  *WrittenLoadOffset + LengthOrigin->Bytes;
              const SymRef CoversLoad = Ctx.mkUge(
                  Event.WrittenBytes,
                  Ctx.mkConst(Ctx.width(Event.WrittenBytes), RequiredBytes));
              WrittenLoadSuccess = Event.Success.isValid()
                                       ? Ctx.mkAnd(Event.Success, CoversLoad)
                                       : CoversLoad;
            }
            const bool SameSource =
                Implicit ? SameBuffer
                         : (SameProducedValue || SameDerivedLength ||
                            SameWrittenLoad);
            if (!SameSource)
              continue;
            SymRef EvidenceSuccess;
            bool EvidenceUnconditional = false;
            auto MergeEvidenceSuccess = [&](SymRef Success) {
              if (!Success.isValid()) {
                EvidenceUnconditional = true;
                EvidenceSuccess = SymRef();
              } else if (!EvidenceUnconditional) {
                EvidenceSuccess = EvidenceSuccess.isValid()
                                      ? Ctx.mkOr(EvidenceSuccess, Success)
                                      : Success;
              }
            };
            if (Implicit)
              MergeEvidenceSuccess(SameBufferSuccess);
            else {
              if (SameProducedValue)
                MergeEvidenceSuccess(Event.ProducedValueSuccess);
              if (SameDerivedLength)
                MergeEvidenceSuccess(SameBufferSuccess);
              if (SameWrittenLoad)
                MergeEvidenceSuccess(WrittenLoadSuccess);
            }
            if (predicateMayHold(Ctx, EvidenceSuccess, Cur.Constraints,
                                 Budgets) &&
                !RequiredSource.empty() && Event.Name == RequiredSource) {
              Best.SourceEvidence = true;
              HasRequiredSourceEvidence = true;
              if (EvidenceUnconditional) {
                SourceEvidenceUnconditional = true;
                SourceEvidenceSuccess = SymRef();
              } else if (!SourceEvidenceUnconditional) {
                SourceEvidenceSuccess =
                    SourceEvidenceSuccess.isValid()
                        ? Ctx.mkOr(SourceEvidenceSuccess, EvidenceSuccess)
                        : EvidenceSuccess;
              }
            }
            if (!UseImplicitLength || !Event.ImplicitLen.isValid() ||
                !predicateMayHold(Ctx, Event.ImplicitLenSuccess,
                                  Cur.Constraints, Budgets))
              continue;
            if (HaveEventImplicitLen &&
                !expressionsMustEqual(Ctx, EventImplicitLen, Event.ImplicitLen,
                                      Cur.Constraints, Budgets)) {
              EventLenConflict = true;
              continue;
            }
            if (!HaveEventImplicitLen) {
              EventImplicitLen = Event.ImplicitLen;
              EventImplicitSuccess = Event.ImplicitLenSuccess;
              EventLenUnconditional = !Event.ImplicitLenSuccess.isValid();
              EventLenIncludesTerminator = Event.ImplicitLenIncludesTerminator;
              HaveEventImplicitLen = true;
            } else if (!EventLenUnconditional) {
              if (!Event.ImplicitLenSuccess.isValid()) {
                EventImplicitSuccess = SymRef();
                EventLenUnconditional = true;
              } else {
                EventImplicitSuccess =
                    Ctx.mkOr(EventImplicitSuccess, Event.ImplicitLenSuccess);
              }
            }
            if (HaveEventImplicitLen)
              EventLenIncludesTerminator &= Event.ImplicitLenIncludesTerminator;
          }
          if (!HasRequiredSourceEvidence) {
            Best.Reached = true;
            DeferredSinkCall = true;
          } else {
            bool ConditionalBoundNotProven = false;
            if (!Implicit && !SourceEvidenceUnconditional &&
                SourceEvidenceSuccess.isValid()) {
              SymRef Parts[] = {Pred, SourceEvidenceSuccess};
              Pred = Ctx.mkAnd(Parts);
            }
            if (!Len.isValid() && EventImplicitLen.isValid() &&
                !EventLenConflict) {
              Len = Ctx.mkZExtOrTrunc(EventImplicitLen, 64);
              if (Implicit && !EventLenIncludesTerminator)
                Len = Ctx.mkAdd(Len, Ctx.mkConst(64, 1));
              ConditionalLenSuccess = EventImplicitSuccess;
            } else if (UseImplicitLength && !Len.isValid() &&
                       Cur.ImplicitLen.isValid() && SameImplicitSource) {
              Len = Ctx.mkZExtOrTrunc(Cur.ImplicitLen, 64);
              if (Implicit && !Cur.ImplicitLenIncludesTerminator)
                Len = Ctx.mkAdd(Len, Ctx.mkConst(64, 1));
              ConditionalLenSuccess = Cur.ImplicitSuccess;
            }
            if (ConditionalLenSuccess.isValid()) {
              ConditionalBoundNotProven = !predicateMustHold(
                  Ctx, ConditionalLenSuccess, Cur.Constraints, Budgets);
              SymRef Parts[] = {Pred, ConditionalLenSuccess};
              Pred = Ctx.mkAnd(Parts);
            }
            SymRef RuntimeCap;
            if (E.CapArg >= 0) {
              RuntimeCap =
                  CI ? readCallArgument(In, Fn, E.CapArg, *CI, Cur.State)
                     : SymRef();
              if (!RuntimeCap.isValid()) {
                Best.SemanticUnknown = true;
                Best.Reached = true;
                ++Finished;
                HitSink = true;
                Stopped = true;
                break;
              }
            }
            Best.Reached = true;
            considerSink(Best, Ctx, Pred, Len, RuntimeCap, Capacity, Budgets);
            if (ConditionalBoundNotProven)
              Best.SemanticUnknown = true;
            ++Finished;
            HitSink = true;
            Stopped = true;
            break;
          }
        }

        const MedCallInfo *Callee = IsCall ? medCallAt(Fn, Op.Addr) : nullptr;
        const std::string CalleeName =
            Callee ? resolveCallName(In, *Callee) : "";
        const bool LengthCall = Callee && isStringLengthCall(CalleeName);
        const CountedSourceReturn CountedReturn =
            Callee ? countedSourceReturn(CalleeName,
                                         In.Img ? In.Img->Format
                                                : BinaryFormat::Unknown)
                   : CountedSourceReturn{};
        const CountedSourceOutput CountedOutput =
            Callee ? countedSourceOutput(CalleeName,
                                         In.Img ? In.Img->Format
                                                : BinaryFormat::Unknown)
                   : CountedSourceOutput{};
        const BoundedStringOutput BoundedString =
            Callee ? boundedStringOutput(CalleeName) : BoundedStringOutput{};
        const ReturnedStringOutput ReturnedString =
            Callee ? returnedStringOutput(CalleeName,
                                          In.Img ? In.Img->Format
                                                 : BinaryFormat::Unknown)
                   : ReturnedStringOutput{};
        const bool CountedReturnRecognized =
            Callee && CountedReturn.BoundArg >= 0;
        bool CountedReturnApplies = CountedReturnRecognized;
        if (CountedReturnApplies && CountedReturn.RequiredZeroArg >= 0) {
          const bool FlagsHaveSemanticWidth = callArgumentHasSemanticWidth(
              In, *Callee, CountedReturn.RequiredZeroArg, 32);
          const SymRef Flags =
              FlagsHaveSemanticWidth
                  ? readCallArgument(In, Fn, CountedReturn.RequiredZeroArg,
                                     *Callee, Cur.State)
                  : SymRef();
          if (!FlagsHaveSemanticWidth)
            Cur.SemanticUnknown = true;
          CountedReturnApplies =
              Flags.isValid() &&
              predicateMustHold(
                  Ctx, Ctx.mkEq(Ctx.mkZExtOrTrunc(Flags, 64), Ctx.mkZero(64)),
                  Cur.Constraints, Budgets);
        }
        const bool CountedOutputApplies =
            Callee && CountedOutput.BoundArg >= 0 &&
            CountedOutput.CountPointerArg >= 0 && CountedOutput.CountBits > 0;
        const SourceEntry *CountedSource =
            CountedReturnApplies || CountedOutputApplies
                ? Cat.matchSource(CalleeName)
                : nullptr;
        const SourceEntry *CalleeSource =
            Callee ? Cat.matchSource(CalleeName) : nullptr;
        std::vector<SourceEvent> NewSourceEvents;
        bool HasFormattedSourceOutput = false;
        if (Callee && CalleeSource && CalleeSource->OutArg >= 0 &&
            CalleeSource->OutArg < static_cast<int>(Callee->Args.size())) {
          SymRef Output = readPointerCallArgument(In, Fn, CalleeSource->OutArg,
                                                  *Callee, Cur.State);
          if (Output.isValid()) {
            NewSourceEvents.push_back(
                {SinkCatalog::normalize(CalleeName), Output, SymRef()});
            NewSourceEvents.back().WritesBuffer = true;
          }
        }
        if (Callee && CalleeSource)
          if (std::optional<safety::detail::FormattedSourceOutputs> Outputs =
                  safety::detail::recoverFormattedSourceOutputs(
                      In.Img, CalleeName, Callee->Args);
              Outputs) {
            bool InputIsTainted =
                Outputs->Kind ==
                safety::detail::FormattedSourceKind::ExternalInput;
            SymRef InputSuccess;
            bool InputIsUnconditionallyTainted = false;
            if (Outputs->Kind ==
                    safety::detail::FormattedSourceKind::DerivedInput &&
                Outputs->InputArg >= 0) {
              SymRef Input = readPointerCallArgument(In, Fn, Outputs->InputArg,
                                                     *Callee, Cur.State);
              for (const SourceEvent &Event : Cur.SourceEvents) {
                const std::optional<uint64_t> InputOffset =
                    constantForwardOffset(Ctx, Event.Buffer, Input,
                                          Cur.Constraints, Budgets);
                if (!InputOffset)
                  continue;
                SymRef EventInputSuccess = Event.Success;
                if (Event.WrittenBytes.isValid()) {
                  const SymRef HasInputByte = Ctx.mkUgt(
                      Event.WrittenBytes,
                      Ctx.mkConst(Ctx.width(Event.WrittenBytes), *InputOffset));
                  EventInputSuccess =
                      EventInputSuccess.isValid()
                          ? Ctx.mkAnd(EventInputSuccess, HasInputByte)
                          : HasInputByte;
                } else if (*InputOffset != 0) {
                  continue;
                }
                if (!predicateMayHold(Ctx, EventInputSuccess, Cur.Constraints,
                                      Budgets))
                  continue;
                InputIsTainted = true;
                if (!EventInputSuccess.isValid()) {
                  InputIsUnconditionallyTainted = true;
                  InputSuccess = SymRef();
                } else if (!InputIsUnconditionallyTainted) {
                  InputSuccess = InputSuccess.isValid()
                                     ? Ctx.mkOr(InputSuccess, EventInputSuccess)
                                     : EventInputSuccess;
                }
              }
            }
            if (InputIsTainted) {
              HasFormattedSourceOutput = true;
              for (const safety::detail::FormattedOutput &Formatted :
                   Outputs->UnboundedTextArgs) {
                SymRef Output = readPointerCallArgument(
                    In, Fn, Formatted.ArgIndex, *Callee, Cur.State);
                if (Output.isValid()) {
                  NewSourceEvents.push_back(
                      {SinkCatalog::normalize(CalleeName), Output, InputSuccess,
                       SymRef(), false, Formatted.RequiredAssignments});
                  NewSourceEvents.back().WritesBuffer = true;
                }
              }
              for (const safety::detail::BoundedTextOutput &Bounded :
                   Outputs->BoundedTextArgs) {
                SymRef Output = readPointerCallArgument(
                    In, Fn, Bounded.ArgIndex, *Callee, Cur.State);
                if (Output.isValid()) {
                  NewSourceEvents.push_back(
                      {SinkCatalog::normalize(CalleeName), Output, InputSuccess,
                       Ctx.mkConst(64, Bounded.MaxChars + 1), true,
                       Bounded.RequiredAssignments});
                  NewSourceEvents.back().WritesBuffer = true;
                }
              }
              for (const safety::detail::FormattedOutput &Scalar :
                   Outputs->ScalarArgs) {
                if (Scalar.Bytes == 0 || !Scalar.AssignmentExecutionObservable)
                  continue;
                SymRef Output = readPointerCallArgument(In, Fn, Scalar.ArgIndex,
                                                        *Callee, Cur.State);
                if (!Output.isValid())
                  continue;
                NewSourceEvents.push_back({SinkCatalog::normalize(CalleeName),
                                           Output, InputSuccess, SymRef(),
                                           false, Scalar.RequiredAssignments});
                SourceEvent &Event = NewSourceEvents.back();
                Event.WritesBuffer = true;
                Event.ScalarOutputBytes = Scalar.Bytes;
                Event.PriorScalarValue =
                    Cur.State.load(Output, Event.ScalarOutputBytes);
              }
            }
          }
        auto readSemanticArgument = [&](int ArgIndex,
                                        uint32_t ExplicitBits = 0) {
          if (!callArgumentHasSemanticWidth(In, *Callee, ArgIndex,
                                            ExplicitBits)) {
            Cur.SemanticUnknown = true;
            return SymRef();
          }
          return readCallArgument(In, Fn, ArgIndex, *Callee, Cur.State);
        };
        SymRef ReturnBound;
        if (CountedReturnApplies)
          ReturnBound = readSemanticArgument(CountedReturn.BoundArg,
                                             CountedReturn.BoundBits);
        SymRef ZeroResultArg;
        if (CountedReturnApplies && CountedReturn.ZeroResultIfArgZero >= 0)
          ZeroResultArg =
              readSemanticArgument(CountedReturn.ZeroResultIfArgZero);
        SymRef CountedObjectSize;
        if (CountedReturnApplies && CountedReturn.ObjectSizeArg >= 0)
          CountedObjectSize = readSemanticArgument(CountedReturn.ObjectSizeArg);
        SymRef CountedElementSize;
        if (CountedReturnApplies && CountedReturn.ElementSizeArg >= 0)
          CountedElementSize =
              readSemanticArgument(CountedReturn.ElementSizeArg);
        SymRef OutputBound;
        SymRef OutputCountPointer;
        if (CountedOutputApplies) {
          OutputBound = readSemanticArgument(CountedOutput.BoundArg,
                                             CountedOutput.BoundBits);
          OutputCountPointer = readPointerCallArgument(
              In, Fn, CountedOutput.CountPointerArg, *Callee, Cur.State);
        }
        SymRef NextImplicitSource;
        SymRef BoundedStringLimit;
        SymRef BoundedStringObjectSize;
        SymRef ReturnedStringBuffer;
        SymRef ReturnedStringLimit;
        const bool CaptureLength = LengthCall && Callee;
        const bool CaptureCountedSource =
            E.LenArg >= 0 && Callee && CountedSource &&
            CountedSource->OutArg >= 0 &&
            CountedSource->OutArg < static_cast<int>(Callee->Args.size());
        const bool CaptureBoundedString =
            Implicit && Callee && CalleeSource &&
            BoundedString.BufferArg >= 0 && BoundedString.BoundArg >= 0 &&
            BoundedString.BufferArg < static_cast<int>(Callee->Args.size()) &&
            BoundedString.BoundArg < static_cast<int>(Callee->Args.size());
        if (CaptureLength)
          NextImplicitSource =
              readPointerCallArgument(In, Fn, 0, *Callee, Cur.State);
        else if (CaptureCountedSource)
          NextImplicitSource = readPointerCallArgument(
              In, Fn, CountedSource->OutArg, *Callee, Cur.State);
        else if (CaptureBoundedString) {
          NextImplicitSource = readPointerCallArgument(
              In, Fn, BoundedString.BufferArg, *Callee, Cur.State);
          BoundedStringLimit = readSemanticArgument(BoundedString.BoundArg,
                                                    BoundedString.BoundBits);
          if (BoundedString.ObjectSizeArg >= 0)
            BoundedStringObjectSize =
                readSemanticArgument(BoundedString.ObjectSizeArg);
        }
        if (Callee && ReturnedString.BufferArg >= 0 &&
            ReturnedString.BoundArg >= 0 &&
            ReturnedString.BufferArg < static_cast<int>(Callee->Args.size()) &&
            ReturnedString.BoundArg < static_cast<int>(Callee->Args.size())) {
          ReturnedStringBuffer = readPointerCallArgument(
              In, Fn, ReturnedString.BufferArg, *Callee, Cur.State);
          ReturnedStringLimit = readSemanticArgument(ReturnedString.BoundArg,
                                                     ReturnedString.BoundBits);
        }
        size_t FirstNewSourceEvent = Cur.SourceEvents.size();
        if (IsCall) {
          bool Summarized = LengthCall || DeferredSinkCall;
          if (Callee && Cat.matchSource(CalleeName))
            Summarized = true;
          if (Callee)
            if (const SinkEntry *Known = Cat.matchSink(CalleeName))
              if (!debugSinkSummaryConflicts(In, *Callee, *Known))
                Summarized = Summarized || Known->Kind == SinkKind::Alloc ||
                             Known->Kind == SinkKind::StackAlloc ||
                             Known->Kind == SinkKind::Realloc;
          if (!Summarized)
            Cur.SemanticUnknown = true;
        }
        StepResult SR = Exec.step(Op);
        SymRef CountedReturnValue;
        SymRef CountedReturnSuccess;
        if (CountedReturnRecognized && !CountedReturnApplies) {
          const SymRef Ret = captureReturn(Op, Cur.State, In);
          if (!returnHasSemanticWidth(In, Ctx, Ret, CountedReturn.ReturnBits)) {
            Cur.SemanticUnknown = true;
            NewSourceEvents.clear();
          } else {
            SymRef SemanticRet = Ret;
            if (CountedReturn.ReturnBits != 0 &&
                CountedReturn.ReturnBits < Ctx.width(Ret))
              SemanticRet = Ctx.mkExtract(Ret, 0, CountedReturn.ReturnBits);
            CountedReturnSuccess =
                CountedReturn.AllowsMinusOne
                    ? Ctx.mkSgt(SemanticRet, Ctx.mkZero(Ctx.width(SemanticRet)))
                    : Ctx.mkUgt(SemanticRet,
                                Ctx.mkZero(Ctx.width(SemanticRet)));
          }
        }
        if (CountedReturnApplies) {
          SymRef Ret = captureReturn(Op, Cur.State, In);
          if (!returnHasSemanticWidth(In, Ctx, Ret, CountedReturn.ReturnBits)) {
            Cur.SemanticUnknown = true;
            NewSourceEvents.clear();
          } else if (!ReturnBound.isValid()) {
            Cur.SemanticUnknown = true;
          } else {
            SymRef ConstraintRet = Ret;
            if (CountedReturn.ReturnBits != 0 &&
                CountedReturn.ReturnBits < Ctx.width(Ret))
              ConstraintRet = Ctx.mkExtract(Ret, 0, CountedReturn.ReturnBits);
            const uint32_t ReturnWidth = Ctx.width(ConstraintRet);
            const SymRef IsError =
                Ctx.mkEq(ConstraintRet, Ctx.mkOnes(ReturnWidth));
            CountedReturnSuccess =
                CountedReturn.AllowsMinusOne
                    ? Ctx.mkSgt(ConstraintRet, Ctx.mkZero(ReturnWidth))
                    : Ctx.mkUgt(ConstraintRet, Ctx.mkZero(ReturnWidth));
            if (CountedReturn.ReturnBits != 0 &&
                CountedReturn.ReturnBits < Ctx.width(Ret)) {
              const uint16_t PointerBytes =
                  In.Img ? getTargetRegInfo(In.Img->Arch).PointerSize : 0;
              const uint32_t SemanticBits =
                  PointerBytes ? uint32_t(PointerBytes) * 8 : Ctx.width(Ret);
              CountedReturnValue =
                  Ctx.mkIte(IsError, Ctx.mkOnes(SemanticBits),
                            Ctx.mkZExtOrTrunc(ConstraintRet, SemanticBits));
            } else {
              CountedReturnValue = Ret;
            }
            SymRef SemanticBound = ReturnBound;
            if (CountedReturn.BoundBits != 0 &&
                CountedReturn.BoundBits < Ctx.width(SemanticBound))
              SemanticBound =
                  Ctx.mkExtract(SemanticBound, 0, CountedReturn.BoundBits);
            const SymRef WideRet = Ctx.mkZExtOrTrunc(ConstraintRet, 64);
            const SymRef WideBound = Ctx.mkZExtOrTrunc(SemanticBound, 64);
            const SymRef WithinBound = Ctx.mkUle(WideRet, WideBound);
            if (CountedReturn.AllowsMinusOne) {
              const SymRef IsNonnegative =
                  Ctx.mkSge(ConstraintRet, Ctx.mkZero(ReturnWidth));
              SymRef Success = Ctx.mkAnd(IsNonnegative, WithinBound);
              if (CountedReturn.BoundIsSigned)
                Success = Ctx.mkAnd(
                    Success, Ctx.mkSge(SemanticBound,
                                       Ctx.mkZero(Ctx.width(SemanticBound))));
              Cur.Constraints.push_back(Ctx.mkOr(IsError, Success));
            } else {
              Cur.Constraints.push_back(WithinBound);
            }
            if (CountedReturn.ZeroResultIfArgZero >= 0) {
              if (!ZeroResultArg.isValid()) {
                Cur.SemanticUnknown = true;
              } else {
                const SymRef SemanticArg = Ctx.mkZExtOrTrunc(ZeroResultArg, 64);
                Cur.Constraints.push_back(
                    Ctx.mkOr(Ctx.mkNe(SemanticArg, Ctx.mkZero(64)),
                             Ctx.mkEq(ConstraintRet, Ctx.mkZero(ReturnWidth))));
              }
            }
            if (CountedReturn.ObjectSizeArg >= 0) {
              if (!CountedObjectSize.isValid() ||
                  (CountedReturn.ElementSizeArg >= 0 &&
                   !CountedElementSize.isValid())) {
                Cur.SemanticUnknown = true;
              } else {
                constexpr uint32_t ProductBits = 128;
                SymRef Requested = Ctx.mkZExtOrTrunc(ReturnBound, ProductBits);
                if (CountedReturn.ElementSizeArg >= 0)
                  Requested =
                      Ctx.mkMul(Requested, Ctx.mkZExtOrTrunc(CountedElementSize,
                                                             ProductBits));
                Cur.Constraints.push_back(
                    Ctx.mkUle(Requested, Ctx.mkZExtOrTrunc(CountedObjectSize,
                                                           ProductBits)));
              }
            }
          }
        }
        if (CountedReturnSuccess.isValid())
          for (SourceEvent &Event : NewSourceEvents) {
            Event.Success = CountedReturnSuccess;
            if (CountedReturnValue.isValid()) {
              Event.ProducedValue = CountedReturnValue;
              Event.WrittenBytes = Ctx.mkZExtOrTrunc(CountedReturnValue, 128);
              if (CountedReturn.ElementSizeArg >= 0) {
                if (CountedElementSize.isValid())
                  Event.WrittenBytes =
                      Ctx.mkMul(Event.WrittenBytes,
                                Ctx.mkZExtOrTrunc(CountedElementSize, 128));
                else
                  Event.WrittenBytes = SymRef();
              }
            }
          }
        SymRef CountedOutputValue;
        SymRef CountedOutputSuccess;
        if (CountedOutputApplies) {
          const SymRef Ret = captureReturn(Op, Cur.State, In);
          if (!returnHasSemanticWidth(In, Ctx, Ret, CountedOutput.ReturnBits)) {
            Cur.SemanticUnknown = true;
            NewSourceEvents.clear();
          } else {
            SymRef SemanticRet = Ret;
            if (CountedOutput.ReturnBits != 0 &&
                CountedOutput.ReturnBits < Ctx.width(SemanticRet))
              SemanticRet =
                  Ctx.mkExtract(SemanticRet, 0, CountedOutput.ReturnBits);
            CountedOutputSuccess =
                Ctx.mkNe(SemanticRet, Ctx.mkZero(Ctx.width(SemanticRet)));
          }
          const std::optional<llvm::APInt> CountPointerConstant =
              OutputCountPointer.isValid() ? Ctx.asConst(OutputCountPointer)
                                           : std::optional<llvm::APInt>();
          if (!CountedOutputSuccess.isValid() || !OutputBound.isValid() ||
              !OutputCountPointer.isValid() ||
              (CountPointerConstant && CountPointerConstant->isZero())) {
            Cur.SemanticUnknown = true;
          } else {
            CountedOutputValue =
                Cur.State.freshInput("source_count", CountedOutput.CountBits);
            SymRef SemanticBound = OutputBound;
            if (CountedOutput.BoundBits != 0 &&
                CountedOutput.BoundBits < Ctx.width(SemanticBound))
              SemanticBound =
                  Ctx.mkExtract(SemanticBound, 0, CountedOutput.BoundBits);
            Cur.Constraints.push_back(
                Ctx.mkUle(Ctx.mkZExtOrTrunc(CountedOutputValue, 64),
                          Ctx.mkZExtOrTrunc(SemanticBound, 64)));
            if (CountedOutput.ZeroOnFailure && CountedOutputSuccess.isValid())
              Cur.Constraints.push_back(Ctx.mkOr(
                  CountedOutputSuccess,
                  Ctx.mkEq(CountedOutputValue,
                           Ctx.mkZero(Ctx.width(CountedOutputValue)))));
            Cur.State.store(OutputCountPointer, CountedOutputValue);
          }
        }
        if (CountedOutputApplies && CountedOutputSuccess.isValid() &&
            CountedOutputValue.isValid()) {
          const SymRef ProducedInput =
              Ctx.mkAnd(CountedOutputSuccess,
                        Ctx.mkNe(CountedOutputValue,
                                 Ctx.mkZero(Ctx.width(CountedOutputValue))));
          for (SourceEvent &Event : NewSourceEvents)
            Event.Success = Event.Success.isValid()
                                ? Ctx.mkAnd(Event.Success, ProducedInput)
                                : ProducedInput;
          for (SourceEvent &Event : NewSourceEvents)
            Event.ProducedValue = CountedOutputValue;
          for (SourceEvent &Event : NewSourceEvents)
            Event.WrittenBytes = Ctx.mkZExtOrTrunc(CountedOutputValue, 128);
        }
        if (CalleeSource && CalleeSource->OutArg >= 0 &&
            CalleeSource->returnCarriesInput()) {
          const SymRef Ret = captureReturn(Op, Cur.State, In);
          const uint32_t ReturnBits = outputSourceReturnBits(
              CalleeName, In.Img ? In.Img->Format : BinaryFormat::Unknown);
          if (!Ret.isValid() ||
              (ReturnBits != 0 &&
               !returnHasSemanticWidth(In, Ctx, Ret, ReturnBits))) {
            Cur.SemanticUnknown = true;
            NewSourceEvents.clear();
          } else {
            SymRef SemanticRet = Ret;
            if (ReturnBits != 0 && ReturnBits < Ctx.width(SemanticRet))
              SemanticRet = Ctx.mkExtract(SemanticRet, 0, ReturnBits);
            const SymRef ProducedInput =
                Ctx.mkNe(SemanticRet, Ctx.mkZero(Ctx.width(SemanticRet)));
            for (SourceEvent &Event : NewSourceEvents) {
              if (!Event.Success.isValid())
                Event.Success = ProducedInput;
              Event.ProducedValue = SemanticRet;
            }
          }
        }
        if (ReturnedStringBuffer.isValid() && ReturnedStringLimit.isValid()) {
          const SymRef Ret = captureReturn(Op, Cur.State, In);
          if (!returnHasSemanticWidth(In, Ctx, Ret,
                                      ReturnedString.ReturnBits)) {
            Cur.SemanticUnknown = true;
            NewSourceEvents.clear();
          } else {
            SymRef SemanticRet = Ret;
            if (ReturnedString.ReturnBits != 0 &&
                ReturnedString.ReturnBits < Ctx.width(SemanticRet))
              SemanticRet =
                  Ctx.mkExtract(SemanticRet, 0, ReturnedString.ReturnBits);
            SymRef SemanticLimit = ReturnedStringLimit;
            if (ReturnedString.BoundBits != 0 &&
                ReturnedString.BoundBits < Ctx.width(SemanticLimit))
              SemanticLimit =
                  Ctx.mkExtract(SemanticLimit, 0, ReturnedString.BoundBits);
            const SymRef ProducedString = Ctx.mkAnd(
                Ctx.mkUgt(SemanticRet, Ctx.mkZero(Ctx.width(SemanticRet))),
                Ctx.mkUlt(Ctx.mkZExtOrTrunc(SemanticRet, 64),
                          Ctx.mkZExtOrTrunc(SemanticLimit, 64)));
            const SymRef StringBytes = Ctx.mkAdd(
                Ctx.mkZExtOrTrunc(SemanticRet, 64), Ctx.mkConst(64, 1));
            for (SourceEvent &Event : NewSourceEvents) {
              if (!expressionsMustEqual(Ctx, Event.Buffer, ReturnedStringBuffer,
                                        Cur.Constraints, Budgets))
                continue;
              Event.Success = Event.Success.isValid()
                                  ? Ctx.mkAnd(Event.Success, ProducedString)
                                  : ProducedString;
              if (ReturnedString.ProvidesNarrowCStringExtent) {
                Event.ImplicitLen = StringBytes;
                Event.ImplicitLenIncludesTerminator = true;
              }
            }
          }
        } else if (ReturnedString.BufferArg >= 0) {
          Cur.SemanticUnknown = true;
        }
        if (HasFormattedSourceOutput) {
          const SymRef Ret = captureReturn(Op, Cur.State, In);
          if (!returnHasSemanticWidth(In, Ctx, Ret, 32)) {
            Cur.SemanticUnknown = true;
            NewSourceEvents.clear();
          } else {
            SymRef SemanticRet = Ret;
            if (Ctx.width(SemanticRet) > 32)
              SemanticRet = Ctx.mkExtract(SemanticRet, 0, 32);
            for (SourceEvent &Event : NewSourceEvents) {
              if (Event.RequiredAssignments == 0)
                continue;
              const SymRef ProducedInput = Ctx.mkSge(
                  SemanticRet, Ctx.mkConst(Ctx.width(SemanticRet),
                                           Event.RequiredAssignments));
              if (Event.Success.isValid())
                Event.Success = Ctx.mkAnd(Event.Success, ProducedInput);
              else
                Event.Success = ProducedInput;
            }
          }
        }
        for (SourceEvent &Event : NewSourceEvents) {
          if (Event.ScalarOutputBytes == 0 || !Event.PriorScalarValue.isValid())
            continue;
          SymRef Fresh = Cur.State.freshInput("formatted_source",
                                              Event.ScalarOutputBytes * 8);
          SymRef Stored =
              Event.Success.isValid()
                  ? Ctx.mkIte(Event.Success, Fresh, Event.PriorScalarValue)
                  : Fresh;
          Cur.State.store(Event.Buffer, Stored);
          Event.ProducedValue = Fresh;
          Event.ProducedValueSuccess = Event.Success;
        }
        for (SourceEvent &Event : NewSourceEvents)
          if (Event.ImplicitLen.isValid())
            Event.ImplicitLenSuccess = Event.Success;
        if (CalleeSource && CalleeSource->OutArg < 0 &&
            CalleeSource->returnCarriesInput()) {
          const SymRef Ret = captureReturn(Op, Cur.State, In);
          if (!Ret.isValid() || (sourceReturnRequiresPointerWidth(CalleeName) &&
                                 !returnHasSemanticWidth(In, Ctx, Ret))) {
            Cur.SemanticUnknown = true;
          } else {
            NewSourceEvents.push_back(
                {SinkCatalog::normalize(CalleeName), Ret,
                 Ctx.mkNe(Ret, Ctx.mkZero(Ctx.width(Ret)))});
            NewSourceEvents.back().ProducedValue = Ret;
          }
        }
        if (IsCall) {
          for (const SourceEvent &NewEvent : NewSourceEvents) {
            if (!NewEvent.WritesBuffer)
              continue;

            auto StartsAfterStringBound = [&](const SourceEvent &OldEvent) {
              if (!OldEvent.ImplicitLen.isValid() ||
                  !OldEvent.Buffer.isValid() || !NewEvent.Buffer.isValid())
                return false;
              const SymRef OldBuffer = Ctx.mkZExtOrTrunc(OldEvent.Buffer, 64);
              const SymRef Length = Ctx.mkZExtOrTrunc(OldEvent.ImplicitLen, 64);
              const SymRef NewBuffer = Ctx.mkZExtOrTrunc(NewEvent.Buffer, 64);
              const SymRef End = Ctx.mkAdd(OldBuffer, Length);
              const SymRef NoOverflow = Ctx.mkUge(End, OldBuffer);
              return predicateMustHold(
                  Ctx, Ctx.mkAnd(NoOverflow, Ctx.mkUge(NewBuffer, End)),
                  Cur.Constraints, Budgets);
            };

            auto WrittenRangesAreDisjoint = [&](const SourceEvent &OldEvent) {
              if (!OldEvent.Buffer.isValid() ||
                  !OldEvent.WrittenBytes.isValid() ||
                  !NewEvent.Buffer.isValid() ||
                  !NewEvent.WrittenBytes.isValid())
                return false;
              constexpr uint32_t RangeBits = 128;
              const SymRef OldBegin =
                  Ctx.mkZExtOrTrunc(OldEvent.Buffer, RangeBits);
              const SymRef NewBegin =
                  Ctx.mkZExtOrTrunc(NewEvent.Buffer, RangeBits);
              const SymRef OldEnd =
                  Ctx.mkAdd(OldBegin, Ctx.mkZExtOrTrunc(OldEvent.WrittenBytes,
                                                        RangeBits));
              const SymRef NewEnd =
                  Ctx.mkAdd(NewBegin, Ctx.mkZExtOrTrunc(NewEvent.WrittenBytes,
                                                        RangeBits));
              const SymRef OldBeforeNew = Ctx.mkAnd(
                  Ctx.mkUge(OldEnd, OldBegin), Ctx.mkUle(OldEnd, NewBegin));
              const SymRef NewBeforeOld = Ctx.mkAnd(
                  Ctx.mkUge(NewEnd, NewBegin), Ctx.mkUle(NewEnd, OldBegin));
              SymRef Disjoint = Ctx.mkOr(OldBeforeNew, NewBeforeOld);
              if (OldEvent.Success.isValid())
                Disjoint = Ctx.mkOr(Ctx.mkNot(OldEvent.Success), Disjoint);
              if (NewEvent.Success.isValid())
                Disjoint = Ctx.mkOr(Ctx.mkNot(NewEvent.Success), Disjoint);
              return predicateMustHold(Ctx, Disjoint, Cur.Constraints, Budgets);
            };

            for (auto It = Cur.SourceEvents.begin();
                 It != Cur.SourceEvents.end();) {
              SourceEvent &OldEvent = *It;
              const bool SameBuffer =
                  expressionsMustEqual(Ctx, OldEvent.Buffer, NewEvent.Buffer,
                                       Cur.Constraints, Budgets);
              const bool MayClobberBound = OldEvent.ImplicitLen.isValid() &&
                                           !StartsAfterStringBound(OldEvent);
              const bool MayClobberWritten =
                  OldEvent.WrittenBytes.isValid() &&
                  !WrittenRangesAreDisjoint(OldEvent);
              if (!NewEvent.Success.isValid()) {
                if (SameBuffer || MayClobberWritten) {
                  It = Cur.SourceEvents.erase(It);
                  continue;
                }
                if (MayClobberBound) {
                  OldEvent.ImplicitLen = SymRef();
                  OldEvent.ImplicitLenSuccess = SymRef();
                  OldEvent.ImplicitLenIncludesTerminator = false;
                }
                ++It;
                continue;
              }

              const SymRef NewFailed = Ctx.mkNot(NewEvent.Success);
              if (SameBuffer || MayClobberWritten) {
                const SymRef PriorSuccess = OldEvent.Success.isValid()
                                                ? OldEvent.Success
                                                : Ctx.mkTrue();
                OldEvent.Success = Ctx.mkAnd(PriorSuccess, NewFailed);
              }
              if (MayClobberBound) {
                const SymRef PriorLenSuccess =
                    OldEvent.ImplicitLenSuccess.isValid()
                        ? OldEvent.ImplicitLenSuccess
                        : Ctx.mkTrue();
                OldEvent.ImplicitLenSuccess =
                    Ctx.mkAnd(PriorLenSuccess, NewFailed);
              }
              ++It;
            }
          }
          FirstNewSourceEvent = Cur.SourceEvents.size();
          Cur.SourceEvents.insert(Cur.SourceEvents.end(),
                                  NewSourceEvents.begin(),
                                  NewSourceEvents.end());
        }
        if (IsCall) {
          if (CaptureLength) {
            const SymRef Ret = captureReturn(Op, Cur.State, In);
            if (!returnHasSemanticWidth(In, Ctx, Ret)) {
              Cur.SemanticUnknown = true;
              Cur.ImplicitLen = SymRef();
              Cur.ImplicitSource = SymRef();
              Cur.ImplicitSuccess = SymRef();
              Cur.ImplicitFromLength = false;
              Cur.ImplicitLenIncludesTerminator = false;
            } else {
              Cur.ImplicitLen = Ret;
              Cur.ImplicitSource = NextImplicitSource;
              Cur.ImplicitSuccess = SymRef();
              Cur.ImplicitFromLength = true;
              Cur.ImplicitLenIncludesTerminator = false;
            }
          } else if (CaptureCountedSource && (CountedReturnValue.isValid() ||
                                              CountedOutputValue.isValid())) {
            Cur.ImplicitLen = CountedReturnValue.isValid() ? CountedReturnValue
                                                           : CountedOutputValue;
            Cur.ImplicitSource = NextImplicitSource;
            Cur.ImplicitSuccess = SymRef();
            Cur.ImplicitFromLength = false;
            Cur.ImplicitLenIncludesTerminator = false;
          } else if (CaptureBoundedString && NextImplicitSource.isValid() &&
                     BoundedStringLimit.isValid()) {
            const SymRef Ret = captureReturn(Op, Cur.State, In);
            if (!returnHasSemanticWidth(In, Ctx, Ret)) {
              Cur.SemanticUnknown = true;
              Cur.ImplicitLen = SymRef();
              Cur.ImplicitSource = SymRef();
              Cur.ImplicitSuccess = SymRef();
              Cur.ImplicitFromLength = false;
              Cur.ImplicitLenIncludesTerminator = false;
            } else {
              const SymRef WideRet = Ctx.mkZExtOrTrunc(Ret, 64);
              SymRef SemanticLimit = BoundedStringLimit;
              if (BoundedString.BoundBits != 0 &&
                  BoundedString.BoundBits < Ctx.width(SemanticLimit))
                SemanticLimit =
                    Ctx.mkExtract(SemanticLimit, 0, BoundedString.BoundBits);
              const SymRef WideLimit = Ctx.mkZExtOrTrunc(SemanticLimit, 64);
              if (BoundedString.ObjectSizeArg >= 0) {
                if (!BoundedStringObjectSize.isValid()) {
                  Cur.SemanticUnknown = true;
                } else {
                  Cur.Constraints.push_back(Ctx.mkUle(
                      WideLimit,
                      Ctx.mkZExtOrTrunc(BoundedStringObjectSize, 64)));
                }
              }
              const SymRef PositiveLimit =
                  BoundedString.BoundIsSigned
                      ? Ctx.mkSgt(SemanticLimit,
                                  Ctx.mkZero(Ctx.width(SemanticLimit)))
                      : Ctx.mkUgt(SemanticLimit,
                                  Ctx.mkZero(Ctx.width(SemanticLimit)));
              const SymRef Returned = Ctx.mkNe(WideRet, Ctx.mkZero(64));
              Cur.Constraints.push_back(
                  Ctx.mkOr(Ctx.mkNot(Returned), PositiveLimit));
              Cur.ImplicitLen = WideLimit;
              Cur.ImplicitSource = NextImplicitSource;
              Cur.ImplicitSuccess = Ctx.mkAnd(Returned, PositiveLimit);
              Cur.ImplicitFromLength = false;
              Cur.ImplicitLenIncludesTerminator = true;
              for (size_t I = FirstNewSourceEvent; I < Cur.SourceEvents.size();
                   ++I) {
                SourceEvent &Event = Cur.SourceEvents[I];
                if (!expressionsMustEqual(Ctx, Event.Buffer, NextImplicitSource,
                                          Cur.Constraints, Budgets))
                  continue;
                Event.ImplicitLen = WideLimit;
                Event.ImplicitLenSuccess = Cur.ImplicitSuccess;
                Event.ImplicitLenIncludesTerminator = true;
              }
            }
          } else {
            Cur.ImplicitLen = SymRef();
            Cur.ImplicitSource = SymRef();
            Cur.ImplicitSuccess = SymRef();
            Cur.ImplicitFromLength = false;
            Cur.ImplicitLenIncludesTerminator = false;
          }
        } else if (mayWriteMemory(Op.Opcode)) {
          for (SourceEvent &Event : Cur.SourceEvents) {
            if (std::optional<uint64_t> Bound = storedStringTerminatorBound(
                    Ctx, Cur.State, Op, Event.Buffer, Cur.Constraints,
                    Budgets)) {
              const std::optional<llvm::APInt> Prior =
                  Event.ImplicitLen.isValid()
                      ? Ctx.asConst(Ctx.mkZExtOrTrunc(Event.ImplicitLen, 64))
                      : std::optional<llvm::APInt>();
              if (!Prior || Prior->getZExtValue() > *Bound) {
                Event.ImplicitLen = Ctx.mkConst(64, *Bound);
                Event.ImplicitLenSuccess = SymRef();
              }
              Event.ImplicitLenIncludesTerminator = true;
            } else if (!writeIsProvablyBeforeSource(Ctx, Cur.State, Op,
                                                    Event.Buffer,
                                                    Cur.Constraints, Budgets)) {
              Event.ImplicitLen = SymRef();
              Event.ImplicitLenSuccess = SymRef();
              Event.ImplicitLenIncludesTerminator = false;
            }
            if (Event.WrittenBytes.isValid() &&
                !writeIsProvablyBeforeSource(Ctx, Cur.State, Op, Event.Buffer,
                                             Cur.Constraints, Budgets))
              Event.WrittenBytes = SymRef();
          }
          if (Cur.ImplicitFromLength ||
              !writeIsProvablyBeforeSource(Ctx, Cur.State, Op,
                                           Cur.ImplicitSource, Cur.Constraints,
                                           Budgets)) {
            Cur.ImplicitLen = SymRef();
            Cur.ImplicitSource = SymRef();
            Cur.ImplicitSuccess = SymRef();
            Cur.ImplicitFromLength = false;
            Cur.ImplicitLenIncludesTerminator = false;
          }
        }
        if (SR == StepResult::Unmodelled) {
          Cur.SemanticUnknown = true;
          continue;
        }
        if (SR == StepResult::Continue)
          continue;
        if (SR == StepResult::Return) {
          Stopped = true;
          ++Finished;
          break;
        }
        if (SR == StepResult::Branch || SR == StepResult::IndirectBranch) {
          std::optional<int> Next = takenSucc(*LF, *Block, Op, Ctx, Exec);
          if (!Next) {
            Best.SemanticUnknown = true;
            Stopped = true;
            break;
          }
          if (Reach.count(*Next)) {
            Frontier N = Cur;
            N.BlockId = *Next;
            Forks.push_back(std::move(N));
          }
          Stopped = true;
          break;
        }
        if (SR == StepResult::CondBranch) {
          std::optional<int> Taken = takenSucc(*LF, *Block, Op, Ctx, Exec);
          SymRef Cond = Exec.branchCondition();
          if (!Taken || !Cond.isValid()) {
            Best.SemanticUnknown = true;
            Stopped = true;
            break;
          }
          int NotTaken = otherSucc(*Block, *Taken);
          auto fork = [&](int Next, SymRef Assume) {
            if (Next < 0 || !Reach.count(Next))
              return;
            Frontier F = Cur;
            F.BlockId = Next;
            F.Constraints.push_back(Assume);
            bool Unknown = false;
            if (!pathFeasible(Ctx, F.Constraints, Budgets, Unknown))
              return;
            if (Unknown)
              Best.SolverUnknown = true;
            Forks.push_back(std::move(F));
          };
          fork(*Taken, Cond);
          fork(NotTaken, Ctx.mkNot(Cond));
          Stopped = true;
          break;
        }
      }
    }
    for (Frontier &F : Forks)
      Pending.push_back(std::move(F));
    if (Stopped || HitSink)
      continue;
    if (Block->Succs.size() == 1 && Reach.count(Block->Succs.front())) {
      Cur.BlockId = Block->Succs.front();
      Pending.push_back(std::move(Cur));
    } else {
      ++Finished;
    }
  }
  if (!Pending.empty())
    Best.Budget = true;
  return Best;
}

} // namespace

std::optional<Finding> neverd::safety::huntSink(const AnalysisInput &In,
                                                const SinkCatalog &Cat,
                                                const SafetyBudgets &Budgets,
                                                const MedFunc &F,
                                                const SinkSite &Site) {
  const SinkEntry *E = Cat.matchSink(Site.Sink);
  if (!E)
    return std::nullopt;

  if ((E->Kind == SinkKind::Copy || E->Kind == SinkKind::Format) &&
      Site.CallInfoIndex < F.CallInfos.size() &&
      debugSinkSummaryConflicts(In, F.CallInfos[Site.CallInfoIndex], *E)) {
    Finding Out;
    stampSite(Out, In, F, Site);
    Out.TheVerdict = Verdict::Unknown;
    Out.TheConfidence = Confidence::Low;
    Out.Detail = "debug function signature conflicts with sink summary";
    return Out;
  }

  if (E->Kind == SinkKind::Format) {
    Finding Out;
    stampSite(Out, In, F, Site);
    if (Site.CallInfoIndex >= F.CallInfos.size() || E->FmtArg < 0 ||
        E->FmtArg >=
            static_cast<int>(F.CallInfos[Site.CallInfoIndex].Args.size())) {
      Out.TheVerdict = Verdict::Unknown;
      Out.TheConfidence = Confidence::Low;
      Out.Detail = "format-string argument was not recovered";
      return Out;
    }

    const MedCallInfo &SinkCall = F.CallInfos[Site.CallInfoIndex];
    if (!safety::detail::callArgumentHasTargetPointerWidth(In.Img, SinkCall,
                                                           E->FmtArg) ||
        (E->DstArg >= 0 && !safety::detail::callArgumentHasTargetPointerWidth(
                               In.Img, SinkCall, E->DstArg))) {
      Out.TheVerdict = Verdict::Unknown;
      Out.TheConfidence = Confidence::Low;
      Out.Detail = "formatting pointer argument has incompatible target width";
      return Out;
    }
    auto presentSizeArgumentHasTargetWidth = [&](int ArgIndex) {
      return ArgIndex < 0 ||
             ArgIndex >= static_cast<int>(SinkCall.Args.size()) ||
             safety::detail::callArgumentHasTargetSizeCarrierWidth(
                 In.Img, SinkCall, ArgIndex);
    };
    if (!presentSizeArgumentHasTargetWidth(E->LenArg) ||
        !presentSizeArgumentHasTargetWidth(E->CapArg)) {
      if (E->DstArg >= 0)
        Out.Class = VulnClass::BufferOverflow;
      Out.TheVerdict = Verdict::Unknown;
      Out.TheConfidence = Confidence::Low;
      Out.Detail = "formatting size argument has incompatible target width";
      return Out;
    }

    const MedVar &FormatArg = SinkCall.Args[E->FmtArg];
    ArgClassification Arg =
        classifyArgument(In, Cat, F, Site.CallInfoIndex, E->FmtArg);
    Out.Flow = Arg.Flow;
    const std::optional<uint64_t> FormatAddress =
        safety::detail::canonicalConstantValue(FormatArg);
    const Segment *FormatSegment = In.Img && FormatAddress
                                       ? In.Img->getSegmentFor(*FormatAddress)
                                       : nullptr;
    std::optional<std::string> ConstantFormat;
    if (Arg.Flow != ArgFlow::Tainted && FormatSegment &&
        !FormatSegment->isWritable())
      ConstantFormat = safety::detail::readMappedCString(In.Img, FormatArg);
    if (Arg.Flow != ArgFlow::Tainted && ConstantFormat) {
      if (E->DstArg < 0) {
        Out.TheVerdict = Verdict::Safe;
        Out.TheConfidence = Confidence::High;
        Out.SkipReason = "constant format string";
        Out.Detail = "format string is a mapped constant";
        return Out;
      }

      Out.Class = VulnClass::BufferOverflow;
      Out.ArgIndex = E->LenArg >= 0 ? E->LenArg : E->FmtArg;
      DestObject Dst =
          resolveDestination(In, Cat, F, Site.CallInfoIndex, E->DstArg);
      if (Dst.Capacity) {
        Out.Capacity = Dst.Capacity;
        Out.CapacityExact = Dst.CapacityExact;
      }

      auto classifyCallArg =
          [&](int Index) -> std::optional<ArgClassification> {
        if (Index < 0 || Site.CallInfoIndex >= F.CallInfos.size() ||
            Index >=
                static_cast<int>(F.CallInfos[Site.CallInfoIndex].Args.size()))
          return std::nullopt;
        return classifyArgument(In, Cat, F, Site.CallInfoIndex, Index);
      };

      if (E->CapArg >= 0) {
        std::optional<ArgClassification> ObjectBound =
            classifyCallArg(E->CapArg);
        if (!ObjectBound) {
          Out.TheVerdict = Verdict::Unknown;
          Out.TheConfidence = Confidence::Low;
          Out.Detail = "fortified destination bound was not recovered";
          return Out;
        }
        if (Dst.Capacity && Dst.CapacityExact && ObjectBound->UpperBound &&
            *ObjectBound->UpperBound <= *Dst.Capacity) {
          Out.TheVerdict = Verdict::Safe;
          Out.TheConfidence = Confidence::High;
          Out.SkipReason = "fortified runtime bound fits the destination";
          Out.Detail =
              "fortified formatting cannot write beyond the recovered object";
          return Out;
        }
        Out.TheVerdict = Verdict::Unknown;
        Out.TheConfidence = Confidence::Low;
        Out.Detail = "fortified destination bound does not prove the recovered "
                     "object is large enough";
        return Out;
      }

      std::optional<uint64_t> ExactWritten =
          literalFormattedBytes(*ConstantFormat);
      std::optional<uint64_t> MaxWritten = ExactWritten;
      if (E->LenArg >= 0) {
        std::optional<ArgClassification> WriteLimit =
            classifyCallArg(E->LenArg);
        if (!WriteLimit) {
          Out.TheVerdict = Verdict::Unknown;
          Out.TheConfidence = Confidence::Low;
          Out.Detail = "formatted write limit was not recovered";
          return Out;
        }
        if (WriteLimit->ConstValue) {
          MaxWritten = ExactWritten
                           ? std::min(*ExactWritten, *WriteLimit->ConstValue)
                           : WriteLimit->ConstValue;
          if (ExactWritten)
            ExactWritten = MaxWritten;
        } else if (WriteLimit->UpperBound) {
          MaxWritten = ExactWritten
                           ? std::min(*ExactWritten, *WriteLimit->UpperBound)
                           : WriteLimit->UpperBound;
          ExactWritten.reset();
        } else {
          MaxWritten.reset();
          ExactWritten.reset();
        }
      }

      if (MaxWritten && *MaxWritten == 0) {
        Out.TheVerdict = Verdict::Safe;
        Out.TheConfidence = Confidence::High;
        Out.SkipReason = "zero formatted write limit";
        Out.Detail = "formatting is configured to write no destination bytes";
        return Out;
      }
      if (!Dst.Capacity) {
        Out.TheVerdict = Verdict::Unknown;
        Out.TheConfidence = Confidence::Low;
        Out.Detail = "format string is constant, but destination extent is "
                     "unresolved";
        return Out;
      }
      if (MaxWritten && *MaxWritten <= *Dst.Capacity) {
        Out.TheVerdict = Dst.CapacityExact ? Verdict::Safe : Verdict::Unknown;
        Out.TheConfidence = Confidence::High;
        Out.SkipReason = "formatted output fits the destination";
        Out.Detail = Dst.CapacityExact
                         ? "formatted output cannot exceed destination capacity"
                         : "formatted output fits a containing-region bound, "
                           "but the object size is unresolved";
        return Out;
      }
      if (ExactWritten && *ExactWritten > *Dst.Capacity) {
        ExploreHit Hit = exploreSink(In, Cat, F, Site, *E, *Dst.Capacity,
                                     Budgets, llvm::StringRef(), ExactWritten);
        if (Hit.Kind == ExploreHit::Overflow) {
          Out.TheVerdict = Verdict::Unsafe;
          Out.TheConfidence = Confidence::High;
          Out.Detail = "formatted output exceeds destination capacity on a "
                       "reachable path";
          Out.Witness.push_back(
              {"formatted_bytes", std::to_string(*ExactWritten)});
          Out.Constraints = Hit.PredText;
          Out.Corroboration =
              "constant formatted output and overflow are jointly reachable";
          Out.BudgetHit = Hit.Budget || Hit.SolverUnknown;
          return Out;
        }
        Out.TheVerdict = Verdict::Unknown;
        Out.TheConfidence = Confidence::Low;
        Out.BudgetHit = Hit.Budget || Hit.SolverUnknown;
        if (Hit.Budget)
          Out.Detail = "exploration budget exhausted";
        else if (Hit.SemanticUnknown)
          Out.Detail = "path contains an operation or call without a summary";
        else if (Hit.SolverUnknown)
          Out.Detail = "solver could not establish path feasibility";
        else
          Out.Detail = "formatted overflow was not established on a reachable "
                       "path";
        return Out;
      }

      Out.TheVerdict = Verdict::Unknown;
      Out.TheConfidence = Confidence::Low;
      Out.Detail = "formatted output extent is unresolved";
      return Out;
    }

    if (Arg.Flow != ArgFlow::Tainted) {
      Out.TheVerdict = Verdict::Unknown;
      Out.TheConfidence = Confidence::Low;
      Out.Detail = "format-string provenance unresolved";
      return Out;
    }

    const std::string Source = SinkCatalog::normalize(Arg.TaintSource);
    const bool EntrySource = Source == "argv";
    SinkEntry Probe = *E;
    Probe.LenArg = -1;
    Probe.SrcArg = E->FmtArg;
    Probe.CapArg = -1;
    ExploreHit Hit = exploreSink(In, Cat, F, Site, Probe, UINT64_MAX, Budgets,
                                 EntrySource ? llvm::StringRef() : Source,
                                 std::nullopt, false);
    if (Hit.Reached && !Hit.Budget && !Hit.SolverUnknown &&
        !Hit.SemanticUnknown && (EntrySource || Hit.SourceEvidence)) {
      Out.TheVerdict = Verdict::Unsafe;
      Out.TheConfidence = Confidence::High;
      Out.Detail =
          "attacker-controlled format string reaches a formatting call";
      Out.Witness.push_back({"format_string", "%n"});
      if (!Source.empty())
        Out.Witness.push_back({"source", Source});
      Out.Corroboration =
          "source and formatting call occur on one feasible path";
      return Out;
    }

    Out.TheVerdict = Verdict::Unknown;
    Out.TheConfidence = Confidence::Low;
    Out.BudgetHit = Hit.Budget || Hit.SolverUnknown;
    if (Hit.Budget)
      Out.Detail = "exploration budget exhausted";
    else if (Hit.SemanticUnknown)
      Out.Detail = "path contains an operation or call without a summary";
    else if (Hit.SolverUnknown)
      Out.Detail = "solver could not establish path feasibility";
    else
      Out.Detail =
          "format-string source was not established on a reachable path";
    return Out;
  }

  if (E->Kind != SinkKind::Copy)
    return std::nullopt;

  Finding Out;
  stampSite(Out, In, F, Site);
  if (Site.CallInfoIndex >= F.CallInfos.size()) {
    Out.TheVerdict = Verdict::Unknown;
    Out.TheConfidence = Confidence::Low;
    Out.Detail = "copy arguments were not recovered";
    return Out;
  }
  const MedCallInfo &SinkCall = F.CallInfos[Site.CallInfoIndex];
  if (!safety::detail::callArgumentHasTargetPointerWidth(In.Img, SinkCall,
                                                         E->DstArg) ||
      (E->SrcArg >= 0 && !safety::detail::callArgumentHasTargetPointerWidth(
                             In.Img, SinkCall, E->SrcArg))) {
    Out.TheVerdict = Verdict::Unknown;
    Out.TheConfidence = Confidence::Low;
    Out.Detail = "copy pointer argument has incompatible target width";
    return Out;
  }
  auto presentSizeArgumentHasTargetWidth = [&](int ArgIndex) {
    return ArgIndex < 0 || ArgIndex >= static_cast<int>(SinkCall.Args.size()) ||
           safety::detail::callArgumentHasTargetSizeCarrierWidth(
               In.Img, SinkCall, ArgIndex);
  };
  if (!presentSizeArgumentHasTargetWidth(E->LenArg) ||
      !presentSizeArgumentHasTargetWidth(E->CapArg)) {
    Out.TheVerdict = Verdict::Unknown;
    Out.TheConfidence = Confidence::Low;
    Out.Detail = "copy size argument has incompatible target width";
    return Out;
  }
  const bool WideElements = safety::detail::usesWideElements(Site.Sink);
  const bool NeedsStringExtents =
      safety::detail::requiresStringExtents(Site.Sink);
  const bool UnboundedInput = E->UnboundedWrite;

  ArgClassification Arg =
      classifyArgument(In, Cat, F, Site.CallInfoIndex, Site.ArgIndex);
  if (UnboundedInput) {
    Arg.Flow = ArgFlow::Tainted;
    Arg.TaintSource = SinkCatalog::normalize(Site.Sink);
    Arg.Reason = "sink receives unbounded external input";
  }
  Out.Flow = Arg.Flow;

  const bool Fortified = E->CapArg >= 0;
  std::optional<ArgClassification> RuntimeBound;
  if (Fortified) {
    if (Site.CallInfoIndex >= F.CallInfos.size() ||
        E->CapArg >=
            static_cast<int>(F.CallInfos[Site.CallInfoIndex].Args.size())) {
      Out.TheVerdict = Verdict::Unknown;
      Out.TheConfidence = Confidence::Low;
      Out.Detail = "fortified destination bound was not recovered";
      return Out;
    }
    RuntimeBound = classifyArgument(In, Cat, F, Site.CallInfoIndex, E->CapArg);
  }

  DestObject Dst =
      resolveDestination(In, Cat, F, Site.CallInfoIndex, E->DstArg);
  if (Dst.Capacity) {
    Out.Capacity = Dst.Capacity;
    Out.CapacityExact = Dst.CapacityExact;
  }

  if (Fortified && E->LenArg >= 0 && Arg.ConstValue &&
      RuntimeBound->UpperBound && *Arg.ConstValue > *RuntimeBound->UpperBound) {
    Out.TheVerdict = Verdict::Safe;
    Out.TheConfidence = Confidence::High;
    Out.SkipReason = "fortified runtime bound rejects the constant length";
    Out.Detail = "fortified copy aborts before writing the requested length";
    return Out;
  }

  if (Fortified && Dst.CapacityExact && RuntimeBound->UpperBound &&
      *RuntimeBound->UpperBound <= *Dst.Capacity) {
    Out.TheVerdict = Verdict::Safe;
    Out.TheConfidence = Confidence::High;
    Out.SkipReason = "fortified runtime bound fits the destination";
    Out.Detail = "fortified copy cannot write beyond the recovered object";
    return Out;
  }

  if (safety::detail::usesTotalDestinationBound(Site.Sink) && Dst.Capacity &&
      Dst.CapacityExact && Arg.UpperBound && *Arg.UpperBound <= *Dst.Capacity) {
    Out.TheVerdict = Verdict::Safe;
    Out.TheConfidence = Confidence::High;
    Out.SkipReason = "destination-size argument fits the destination";
    Out.Detail = "size-limited string operation cannot write beyond the "
                 "recovered object";
    return Out;
  }

  const std::optional<uint64_t> WideElementBytes =
      safety::detail::countedWideElementBytes(
          Site.Sink, In.Img ? In.Img->Format : BinaryFormat::Unknown);
  if (WideElementBytes && Dst.Capacity && Dst.CapacityExact && Arg.UpperBound &&
      *Arg.UpperBound <= *Dst.Capacity / *WideElementBytes) {
    Out.TheVerdict = Verdict::Safe;
    Out.TheConfidence = Confidence::High;
    Out.SkipReason = "wide-element count fits the destination";
    Out.Detail = "wide-element copy cannot write beyond the recovered object";
    return Out;
  }

  std::optional<uint64_t> FixedExploreLength;
  if (WideElements && WideElementBytes && Arg.ConstValue)
    FixedExploreLength = safety::detail::exactCountedMemoryBytes(
        Site.Sink, In.Img ? In.Img->Format : BinaryFormat::Unknown,
        *Arg.ConstValue);
  if ((WideElements && !FixedExploreLength) || NeedsStringExtents) {
    Out.TheVerdict = Verdict::Unknown;
    Out.TheConfidence = Confidence::Low;
    Out.Detail = WideElements ? "wide-element byte extent is not established"
                              : "source and destination string extents are "
                                "unresolved";
    return Out;
  }

  if (!FixedExploreLength && E->LenArg >= 0 && Arg.ConstValue && Dst.Capacity) {
    if (*Arg.ConstValue <= *Dst.Capacity && Dst.CapacityExact) {
      Out.TheVerdict = Verdict::Safe;
      Out.TheConfidence = Confidence::High;
      Out.SkipReason = Arg.Reason.empty() ? "constant length" : Arg.Reason;
      Out.Detail = "constant length within destination capacity";
      return Out;
    }
    if (*Arg.ConstValue <= *Dst.Capacity) {
      Out.TheVerdict = Verdict::Unknown;
      Out.TheConfidence = Confidence::High;
      Out.SkipReason = Arg.Reason.empty() ? "constant length" : Arg.Reason;
      Out.Detail = "constant length is bounded, but the exact destination size "
                   "is unresolved";
      return Out;
    }
  }

  if (!Dst.Capacity) {
    Out.TheVerdict = Verdict::Unknown;
    Out.TheConfidence = Confidence::Low;
    Out.Detail = "destination capacity could not be recovered";
    return Out;
  }

  if (!FixedExploreLength && E->LenArg >= 0 && Arg.Flow == ArgFlow::Bounded &&
      Arg.UpperBound && *Arg.UpperBound <= *Dst.Capacity) {
    Out.TheVerdict = Dst.CapacityExact ? Verdict::Safe : Verdict::Unknown;
    Out.TheConfidence =
        Dst.CapacityExact ? Confidence::Medium : Confidence::High;
    Out.SkipReason = Arg.Reason.empty() ? "bounded length" : Arg.Reason;
    Out.Detail =
        Dst.CapacityExact
            ? "proven argument bound fits destination capacity"
            : "length is bounded, but the exact destination size is unresolved";
    return Out;
  }

  std::string RequiredSource;
  if (!UnboundedInput && Arg.Flow == ArgFlow::Tainted)
    if (const SourceEntry *Source = Cat.matchSource(Arg.TaintSource); Source) {
      const std::string NormalizedSource =
          SinkCatalog::normalize(Arg.TaintSource);
      if (NormalizedSource != "argv" &&
          (Source->OutArg >= 0 || Source->returnCarriesInput() ||
           safety::detail::formattedSourceName(Arg.TaintSource).has_value()))
        RequiredSource = NormalizedSource;
    }
  ExploreHit Hit = exploreSink(In, Cat, F, Site, *E, *Dst.Capacity, Budgets,
                               RequiredSource, FixedExploreLength);
  if (Hit.Kind == ExploreHit::Overflow) {
    Out.TheVerdict = Verdict::Unsafe;
    Out.TheConfidence = Confidence::High;
    Out.Detail =
        "copy length can exceed destination capacity on a reachable path";
    Out.Constraints = Hit.PredText;
    addWitness(Out, *E, Hit.WitnessLen, Arg.TaintSource);
    Out.Corroboration = "path predicate and overflow are jointly satisfiable";
    Out.BudgetHit = Hit.Budget || Hit.SolverUnknown;
    return Out;
  }
  if (Hit.Kind == ExploreHit::InBound && Hit.Reached && !Hit.Budget &&
      !Hit.SolverUnknown && !Hit.SemanticUnknown && !Hit.MissingLength) {
    Out.TheVerdict = Dst.CapacityExact ? Verdict::Safe : Verdict::Unknown;
    Out.TheConfidence = Confidence::High;
    Out.Detail = Dst.CapacityExact
                     ? "every explored path keeps the copy within capacity"
                     : "paths fit a containing-region upper bound, but the "
                       "object size is unresolved";
    Out.Constraints = Hit.PredText;
    return Out;
  }

  if (Arg.Flow == ArgFlow::Tainted && E->LenArg < 0 &&
      (E->SrcArg >= 0 || UnboundedInput) && Hit.Reached && !Hit.Budget &&
      !Hit.SolverUnknown && !Hit.SemanticUnknown &&
      (RequiredSource.empty() || Hit.SourceEvidence)) {
    const bool GuardAllowsOverflow =
        !Fortified ||
        (RuntimeBound->ConstValue && *RuntimeBound->ConstValue > *Dst.Capacity);
    if (GuardAllowsOverflow)
      if (auto Len = abstractOverflowLen(*Dst.Capacity, Budgets)) {
        Out.TheVerdict = Verdict::Unsafe;
        Out.TheConfidence = Confidence::High;
        Out.Detail =
            "attacker-controlled length can exceed destination capacity";
        addWitness(Out, *E, *Len, Arg.TaintSource);
        Out.Corroboration = "sink reachable on a summarized symbolic path";
        return Out;
      }
  }

  Out.TheVerdict = Verdict::Unknown;
  Out.TheConfidence = Confidence::Low;
  Out.BudgetHit = Hit.Budget || Hit.SolverUnknown;
  if (Hit.Budget)
    Out.Detail = "exploration budget exhausted";
  else if (Hit.SemanticUnknown)
    Out.Detail = "path contains an operation or call without a summary";
  else if (Hit.SolverUnknown)
    Out.Detail = "solver could not establish path feasibility";
  else
    Out.Detail = "length provenance unresolved";
  return Out;
}

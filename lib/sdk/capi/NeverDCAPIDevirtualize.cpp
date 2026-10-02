//===- NeverDCAPIDevirtualize.cpp - Interpreter recovery C API -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPIDevirtualize.h"

#include "SessionImpl.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighSourceFlow.h"
#include "neverd/ir/med/MedMutableSource.h"
#include "neverd/ir/med/MedSourceParameterUses.h"
#include "neverd/pipeline/NativeSourceHints.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/SHA256.h"

#include <map>
#include <set>

using namespace neverd;
using namespace neverd::sdk;

namespace {
const char *statusName(analysis::SpecializationStatus Status) {
  switch (Status) {
  case analysis::SpecializationStatus::Complete:
    return "complete";
  case analysis::SpecializationStatus::InvalidInput:
    return "invalid-input";
  case analysis::SpecializationStatus::Unsupported:
    return "unsupported";
  case analysis::SpecializationStatus::UnresolvedControl:
    return "unresolved-control";
  case analysis::SpecializationStatus::BudgetExceeded:
    return "budget-exceeded";
  }
  return "invalid-input";
}

std::string medRecoverySourceLimitation(const MedFunc &Function, Arch TheArch,
                                        const LowFunc *SourceFrame) {
  const auto &TRI = getTargetRegInfo(TheArch);
  SourceFunctionTypeHint Hint;
  if (Function.SourceTypeHint) {
    if (!Function.SourceParametersBound)
      return "recovered source parameters are not bound to their ABI";
    Hint = *Function.SourceTypeHint;
  } else {
    Hint.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Hint.Architecture = TheArch;
    Hint.HasExplicitABI = true;
    Hint.ReturnType = Function.ReturnType;
    if (!Hint.ReturnType || !Function.MultiReturn.empty())
      return "recovered source has no supported scalar return binding";
    if (Hint.ReturnType->Kind != NdTypeKind::Void) {
      const bool Floating = Hint.ReturnType->Kind == NdTypeKind::Float;
      Hint.ReturnLocation = {Floating ? SourceABICarrierKind::FloatingRegister
                                      : SourceABICarrierKind::IntegerRegister,
                             Floating ? TRI.FPReturnReg : TRI.IntReturnReg, 0,
                             Hint.ReturnType->Size};
    }
  }
  std::string Error;
  if (!validateSourceABI(Hint, Error))
    return "recovered source ABI cannot establish its input domain";
  std::optional<std::map<uint64_t, uint64_t>> Demand;
  if (Function.SkippedSSA) {
    const auto Plan = analyzeMedMutableSource(Function, TheArch);
    if (Plan)
      Demand = Plan->EntryBytes;
  } else {
    Demand = observedMedSourceEntryBytes(Function, Hint,
                                         SourceEntryDemand::EffectsAndReturns);
  }
  if (!Demand)
    return "recovered source entry-value proof is incomplete";

  // Source lowering may turn otherwise-unbound machine inputs into diagnostic
  // placeholders or freeze them. Demand must therefore be checked in MedIR,
  // before either backend loses that information. A source-frame projection
  // needs its own proof; callee-saved register values receive no exemption.
  std::map<uint64_t, uint64_t> BoundBytes;
  int64_t RequiredFrameSize = 0;
  if (SourceFrame && (Function.FrameSize > 0 || Function.FrameHeadroom > 0) &&
      certifiesPrivateNativeSourceFrame(*SourceFrame, TheArch, true,
                                        &RequiredFrameSize) &&
      RequiredFrameSize >= 0 && Function.FrameSize >= RequiredFrameSize)
    BoundBytes[TRI.StackPointer] = 0xff;
  const auto Layout =
      TRI.integerArgumentLayout(Function.CC == CallingConv::Win64);
  const auto Declared = sourceABIParameters(Hint);
  for (size_t I = 0; I < Function.Params.size(); ++I) {
    const auto &Parameter = Function.Params[I];
    // A declared argument in mutable MedIR may have no synthetic SSA seed
    // (Id == -1). It still occupies its declared ABI position; the separate
    // mutable plan, rather than that SSA bookkeeping ID, owns entry demand.
    if (Parameter.RegOff == kNoParamReg ||
        (Parameter.Id < 0 && !(Function.SkippedSSA && Function.SourceTypeHint)))
      continue;
    unsigned Bytes = Parameter.Size;
    if (Function.SourceTypeHint) {
      if (I >= Declared.size() ||
          Declared[I].Location.RegisterOffset != Parameter.RegOff ||
          (Declared[I].Location.Kind != SourceABICarrierKind::IntegerRegister &&
           Declared[I].Location.Kind != SourceABICarrierKind::FloatingRegister))
        return "recovered source parameter carrier is inconsistent";
      Bytes = std::min<unsigned>(Bytes, Declared[I].Location.ValueBytes);
    } else if (Layout.registerIndex(Parameter.RegOff) < 0 &&
               !TRI.isFPArgReg(Parameter.RegOff)) {
      return "recovered source parameter has no ordinary ABI carrier";
    }
    if (!Bytes || Bytes > 64)
      return "recovered source parameter width is unsupported";
    BoundBytes[Parameter.RegOff] |=
        Bytes == 64 ? ~uint64_t(0) : (uint64_t(1) << Bytes) - 1;
  }
  for (const auto &[Register, Bytes] : *Demand)
    for (unsigned I = 0; I < 64; ++I)
      if ((Bytes & (uint64_t(1) << I)) &&
          std::none_of(BoundBytes.begin(), BoundBytes.end(),
                       [&](const auto &Bound) {
                         const auto [Base, Mask] = Bound;
                         return Register >= Base && Register - Base < 64 &&
                                I < 64 - (Register - Base) &&
                                (Mask & (uint64_t(1) << (Register - Base + I)));
                       }))
        return "recovered source observes an entry register byte without a "
               "source parameter; use the explicit machine-state ABI";
  return {};
}

std::string highRecoverySourceLimitation(const HighFunc &Function) {
  const bool NeedsReturn =
      !Function.ReturnType || Function.ReturnType->Kind != NdTypeKind::Void;
  const auto Flow = analyzeHighSourceFlow(Function, NeedsReturn);
  if (!Flow.Complete || !Flow.Items.empty())
    return Flow.Items.empty()
               ? "recovered source flow is incomplete"
               : "recovered source flow: " + Flow.Items.front().Reason;

  // Flow owns CFG and definite assignment. Its expression traversal does not
  // reject an explicit Undef leaf or a missing non-void return expression.
  // Check those publication failures without changing ordinary decompilation,
  // whose diagnostic placeholders are still useful to a human reader.
  size_t Budget = 1000000;
  const auto Spend = [&](size_t Count) {
    if (Count > Budget)
      return false;
    Budget -= Count;
    return true;
  };
  std::vector<std::pair<const HighStmt *, unsigned>> Statements;
  std::vector<std::pair<const HighExpr *, unsigned>> Expressions;
  std::map<const HighExpr *, unsigned> SeenExpressions;
  const auto AddStatements = [&](const std::vector<HighStmt> &Body,
                                 unsigned Depth) {
    if (!Spend(Body.size()))
      return false;
    for (const auto &Statement : Body)
      Statements.emplace_back(&Statement, Depth);
    return true;
  };
  if (Function.Body.empty())
    return "recovered source has no body";
  if (!AddStatements(Function.Body, 1))
    return "recovered source publication budget exhausted";
  while (!Statements.empty()) {
    const auto [Statement, Depth] = Statements.back();
    Statements.pop_back();
    if (Depth > 200)
      return "recovered source exceeds the statement-depth limit";
    if (Statement->Kind == StmtKind::Return && NeedsReturn &&
        !Statement->RetVal)
      return "recovered source has a return without a defined value";
    bool RootBudget = true;
    forEachExpr(*Statement, [&](const ExprPtr &Expression) {
      if (Spend(1))
        Expressions.emplace_back(Expression.get(), 1);
      else
        RootBudget = false;
    });
    if (!RootBudget)
      return "recovered source publication budget exhausted";
    if (!AddStatements(Statement->Body, Depth + 1) ||
        !AddStatements(Statement->ElseBody, Depth + 1) ||
        !AddStatements(Statement->DefaultBody, Depth + 1) ||
        !Spend(Statement->Cases.size() + Statement->EHClauseBodies.size()))
      return "recovered source publication budget exhausted";
    for (const auto &Case : Statement->Cases)
      if (!AddStatements(Case.Body, Depth + 1))
        return "recovered source publication budget exhausted";
    for (const auto &Body : Statement->EHClauseBodies)
      if (!AddStatements(Body, Depth + 1))
        return "recovered source publication budget exhausted";
  }
  while (!Expressions.empty()) {
    if (!Spend(1))
      return "recovered source publication budget exhausted";
    const auto [Expression, Depth] = Expressions.back();
    Expressions.pop_back();
    if (!Expression)
      return "recovered source has a missing expression operand";
    if (Depth > 200)
      return "recovered source exceeds the expression-depth limit";
    const auto [It, Fresh] = SeenExpressions.emplace(Expression, Depth);
    if (!Fresh && It->second >= Depth)
      continue;
    It->second = Depth;
    if (Expression->Kind == ExprKind::Undef)
      return "recovered source contains an undefined value";
    if (!Spend(Expression->Operands.size()))
      return "recovered source publication budget exhausted";
    for (const auto &Operand : Expression->Operands)
      Expressions.emplace_back(Operand.get(), Depth + 1);
    if (Expression->IndirectTarget)
      Expressions.emplace_back(Expression->IndirectTarget.get(), Depth + 1);
  }
  return {};
}

std::string llvmRecoverySourceLimitation(const llvm::Module &Module) {
  size_t Budget = 1000000;
  std::vector<const llvm::Value *> Pending;
  std::set<const llvm::Value *> Seen;
  const auto Add = [&](const llvm::Value *Value) {
    if (!Budget)
      return false;
    --Budget;
    Pending.push_back(Value);
    return true;
  };
  for (const auto &Global : Module.globals())
    if (Global.hasInitializer() && !Add(Global.getInitializer()))
      return "recovered LLVM source publication budget exhausted";
  for (const auto &Function : Module)
    for (const auto &Block : Function)
      for (const auto &Instruction : Block) {
        if (!Budget)
          return "recovered LLVM source publication budget exhausted";
        --Budget;
        for (const auto &Operand : Instruction.operands())
          if (!Add(Operand.get()))
            return "recovered LLVM source publication budget exhausted";
      }
  while (!Pending.empty()) {
    const llvm::Value *Value = Pending.back();
    Pending.pop_back();
    if (!Seen.insert(Value).second)
      continue;
    if (llvm::isa<llvm::UndefValue, llvm::PoisonValue>(Value))
      return "recovered LLVM source contains an undefined or poison value";
    // Instructions are visited above. Only constant aggregate/expression
    // operands need recursive inspection; globals have their own root list.
    if (const auto *Constant = llvm::dyn_cast<llvm::Constant>(Value);
        Constant && !llvm::isa<llvm::GlobalValue>(Value))
      for (const auto &Operand : Constant->operands())
        if (!Add(Operand.get()))
          return "recovered LLVM source publication budget exhausted";
  }
  return {};
}
} // namespace

static const char *devirtualizeSource(
    neverd_session_t Session, neverd_va_t Entry,
    const neverd_devirtualize_options_v1 *Options, const char **Report,
    bool MachineState,
    const neverd_devirtualize_options_v2 *ExtendedOptions = nullptr,
    const neverd_devirtualize_options_v3 *BudgetOptions = nullptr,
    const neverd_devirtualize_options_v4 *EntryOptions = nullptr,
    const neverd_devirtualize_options_v5 *AlignmentOptions = nullptr) {
  if (Report)
    *Report = nullptr;
  auto *S = toSession(Session);
  if (!S)
    return nullptr;
  S->clearError();
  llvm::json::Object Evidence{
      {"schemaVersion", 1},
      {"entry", vaHex(Entry)},
      {"status", "invalid-input"},
      {"complete", false},
      {"contract", "fixed image mappings and permissions; no concurrent "
                   "mutation; source recovery only"}};
  auto PublishReport = [&] {
    if (Report)
      *Report = dupStr(
          llvm::formatv("{0:2}", llvm::json::Value(std::move(Evidence))).str());
  };
  auto Fail = [&](const std::string &Error) -> const char * {
    S->setError(Error);
    Evidence["error"] = Error;
    PublishReport();
    return nullptr;
  };
  try {
    PipelineOptions PO;
    PO.OnlyFunctionEntries = {Entry};
    PO.InterpreterSpecialization.emplace();
    auto &Config = *PO.InterpreterSpecialization;
    Config.DiscoverControlState = true;
    Config.ExplicitMachineState = MachineState;
    Config.NormalNonfaultingExecution = MachineState;
    Config.X64CetDisabled = MachineState;
    Evidence["sourceABI"] =
        MachineState ? "x64-machine-state-v1" : "ordinary-source";
    if (!MachineState)
      Evidence["frameContract"] =
          "if a private source frame is reconstructed, every external-origin "
          "LOAD/STORE range, including computed external addresses, must be "
          "disjoint from the native invocation-private frame and its "
          "reconstructed source storage (source relocation precondition)";
    if (MachineState)
      Evidence["executionProfile"] =
          "64-bit CPL3/IOPL0; shadow stacks disabled; normal nonfaulting "
          "execution without asynchronous events; canonical entry flags; "
          "TF/RF/VM/AC/VIF/VIP clear; POPFQ TF/AC clear; state storage "
          "disjoint from "
          "guest memory; fixed original mappings; little-endian 64-bit host";
    if (Options) {
      const size_t RequiredSize = AlignmentOptions  ? sizeof(*AlignmentOptions)
                                  : EntryOptions    ? sizeof(*EntryOptions)
                                  : BudgetOptions   ? sizeof(*BudgetOptions)
                                  : ExtendedOptions ? sizeof(*ExtendedOptions)
                                                    : sizeof(*Options);
      if (Options->struct_size < RequiredSize)
        return Fail(
            AlignmentOptions
                ? "devirtualize options do not cover the complete v5 structure"
            : EntryOptions
                ? "devirtualize options do not cover the complete v4 structure"
            : BudgetOptions
                ? "devirtualize options do not cover the complete v3 structure"
            : ExtendedOptions
                ? "devirtualize options do not cover the complete v2 structure"
                : "devirtualize options do not cover the complete v1 "
                  "structure");
      // Older entry points may receive arbitrary future tails. Inspect each
      // extension only through its matching API, after checking its full size.
      if (AlignmentOptions) {
        if (AlignmentOptions->max_symbolic_nodes)
          Config.MaxSymbolicNodes = AlignmentOptions->max_symbolic_nodes;
        const auto Alignment = AlignmentOptions->entry_frame_alignment;
        const auto Residue = AlignmentOptions->entry_frame_residue;
        if (!Alignment && Residue)
          return Fail("entry residue requires an alignment");
        if (Alignment) {
          if (!MachineState)
            return Fail("entry alignment requires the machine-state API");
          Config.EntryFrameAlignment =
              analysis::InterpreterEntryAlignment{Alignment, Residue};
          if (!Config.EntryFrameAlignment->valid())
            return Fail("invalid entry alignment or residue");
        }
      }
      if (EntryOptions) {
        constexpr uint32_t KnownFlags =
            NEVERD_DEVIRTUALIZE_V4_DISABLE_CONTROL_DISCOVERY |
            NEVERD_DEVIRTUALIZE_V4_HAS_ENTRY_FRAME_BOUNDS;
        if (EntryOptions->flags & ~KnownFlags)
          return Fail("invalid devirtualize v4 flags");
        const bool HasBounds =
            EntryOptions->flags & NEVERD_DEVIRTUALIZE_V4_HAS_ENTRY_FRAME_BOUNDS;
        if (HasBounds && !MachineState)
          return Fail("entry frame bounds require the machine-state API");
        if (!HasBounds &&
            (EntryOptions->entry_frame_begin || EntryOptions->entry_frame_end))
          return Fail("entry frame endpoints require the bounds flag");
        Config.MaxChainedTransfers = EntryOptions->max_chained_transfers;
        Config.DiscoverControlState =
            !(EntryOptions->flags &
              NEVERD_DEVIRTUALIZE_V4_DISABLE_CONTROL_DISCOVERY);
        if (HasBounds)
          Config.EntryFrameBounds = analysis::SpecializationEntryFrameBounds{
              EntryOptions->entry_frame_begin, EntryOptions->entry_frame_end};
      }
      if (BudgetOptions && BudgetOptions->reserved)
        return Fail("invalid devirtualize v3 flags");
      if (ExtendedOptions && ExtendedOptions->reserved)
        return Fail("invalid devirtualize v2 flags");
      if (Options->reserved ||
          (Options->use_llvm != 0 && Options->use_llvm != 1) ||
          (Options->no_opt != 0 && Options->no_opt != 1))
        return Fail("invalid devirtualize flags");
      if (Options->control_register_count > 16 ||
          (Options->control_register_count && !Options->control_registers))
        return Fail("invalid control register list");
      if (S->Img.Arch != Arch::X64)
        return Fail("interpreter recovery currently requires x64");
      const auto &TRI = getTargetRegInfo(Arch::X64);
      for (size_t I = 0; I < Options->control_register_count; ++I) {
        const char *Name = Options->control_registers[I];
        bool Found = false;
        if (Name)
          for (uint64_t Offset : TRI.GeneralRegs)
            if (llvm::StringRef(Name).equals_insensitive(
                    TRI.GetRegName(Offset, 8))) {
              Config.ControlRegisters.push_back({Offset, 8});
              Found = true;
              break;
            }
        if (!Found)
          return Fail("control register must be a full x64 general register");
      }
      if (Options->control_frame_slot_count > 64 ||
          (Options->control_frame_slot_count && !Options->control_frame_slots))
        return Fail("invalid frame control slot list");
      for (size_t I = 0; I < Options->control_frame_slot_count; ++I) {
        const auto &Slot = Options->control_frame_slots[I];
        if (!Slot.bytes || Slot.bytes > 8 || Slot.reserved)
          return Fail("invalid frame control slot range");
        Config.ControlFrameSlots.push_back({Slot.offset, Slot.bytes});
      }
      if (Options->max_nodes)
        Config.MaxNodes = Options->max_nodes;
      if (Options->max_contexts_per_address)
        Config.MaxContextsPerAddress = Options->max_contexts_per_address;
      if (Options->max_operations)
        Config.MaxOperations = Options->max_operations;
      if (ExtendedOptions && ExtendedOptions->max_control_refinements)
        Config.MaxControlRefinements = ExtendedOptions->max_control_refinements;
      if (BudgetOptions && BudgetOptions->max_control_fields)
        Config.MaxControlFields = BudgetOptions->max_control_fields;
      if (BudgetOptions && BudgetOptions->max_solver_queries)
        Config.MaxSolverQueries = BudgetOptions->max_solver_queries;
      PO.LiftMode = Options->use_llvm != 0;
      PO.NoOpt = Options->no_opt != 0;
    }
    llvm::LLVMContext Context;
    if (S->Img.InputFileSHA256)
      Evidence["imageSha256"] = llvm::toHex(*S->Img.InputFileSHA256);
    else if (!S->Img.Raw.empty())
      Evidence["imageSha256"] = llvm::toHex(llvm::SHA256::hash(S->Img.Raw));
    else
      Evidence["imageSha256"] = nullptr;
    llvm::json::Array Controls;
    for (const auto &Range : Config.ControlRegisters)
      Controls.push_back(
          llvm::json::Object{{"offset", static_cast<int64_t>(Range.Offset)},
                             {"bytes", Range.Bytes}});
    Evidence["controlRegisters"] = std::move(Controls);
    llvm::json::Array FrameSlots;
    for (const auto &Slot : Config.ControlFrameSlots)
      FrameSlots.push_back(
          llvm::json::Object{{"offset", Slot.Offset}, {"bytes", Slot.Bytes}});
    Evidence["controlFrameSlots"] = std::move(FrameSlots);
    Evidence["frameRoot"] = "entry-rsp";
    Evidence["returnContract"] =
        "ordinary ABI return; every external-origin store target range, "
        "including computed external addresses, is disjoint from the entry "
        "return-address slot (environment precondition); root-derived writes "
        "checked";
    if (MachineState)
      Evidence["returnContract"] =
          "state captured before final native RET pop; internal near CALL/RET "
          "use physical guest stack; entry return slot preserved; status zero "
          "required; nonzero status does not certify output or roll back "
          "guest memory effects; every guest write range, including computed "
          "external addresses, must be disjoint from the entry return-address "
          "slot (environment precondition); root-derived writes checked";
    Evidence["maxNativeReturnSlots"] = Config.MaxNativeReturnSlots;
    Evidence["maxNodes"] = Config.MaxNodes;
    Evidence["maxContextsPerAddress"] = Config.MaxContextsPerAddress;
    Evidence["maxOperations"] = static_cast<int64_t>(Config.MaxOperations);
    Evidence["maxNodeEvaluations"] = Config.MaxNodeEvaluations;
    Evidence["maxIndirectTargets"] = Config.MaxIndirectTargets;
    Evidence["maxImmutableReadAddresses"] = Config.MaxImmutableReadAddresses;
    Evidence["maxControlTuples"] = Config.MaxControlTuples;
    Evidence["maxControlFields"] = Config.MaxControlFields;
    Evidence["maxChainedTransfers"] = Config.MaxChainedTransfers;
    if (Config.EntryFrameBounds)
      Evidence["entryFrameBounds"] = llvm::json::Object{
          {"begin", Config.EntryFrameBounds->Begin},
          {"end", Config.EntryFrameBounds->End},
          {"contract", "entry RSP plus every signed offset in [begin,end) "
                       "fits in [0,UINT64_MAX] without wrapping; unchecked "
                       "caller precondition; no memory accessibility, "
                       "initialization or nonalias guarantee"}};
    else
      Evidence["entryFrameBounds"] = nullptr;
    if (Config.EntryFrameAlignment)
      Evidence["entryFrameAlignment"] = llvm::json::Object{
          {"alignment", Config.EntryFrameAlignment->Alignment},
          {"residue", Config.EntryFrameAlignment->Residue},
          {"contract", "entry RSP modulo alignment must equal residue; "
                       "checked before guest accesses or state writes; "
                       "rejected entry returns status 2 with state unchanged"}};
    else
      Evidence["entryFrameAlignment"] = nullptr;
    Evidence["discoverControlState"] = Config.DiscoverControlState;
    Evidence["maxControlRefinements"] = Config.MaxControlRefinements;
    Evidence["maxDiscoveryVisits"] =
        static_cast<int64_t>(Config.MaxDiscoveryVisits);
    Evidence["maxSolverQueries"] =
        static_cast<int64_t>(Config.MaxSolverQueries);
    Evidence["maxSolverGates"] = static_cast<int64_t>(Config.MaxSolverGates);
    Evidence["maxSolverConflicts"] =
        static_cast<int64_t>(Config.MaxSolverConflicts);
    Evidence["maxSolverPropagations"] =
        static_cast<int64_t>(Config.MaxSolverPropagations);
    Evidence["maxSolverWatchVisits"] =
        static_cast<int64_t>(Config.MaxSolverWatchVisits);
    Evidence["maxSymbolicNodes"] =
        static_cast<int64_t>(Config.MaxSymbolicNodes);
    Pipeline P;
    auto Result = P.run(S->Img, Context, PO);
    if (Result.InterpreterRecovery) {
      const auto &R = *Result.InterpreterRecovery;
      Evidence["status"] = statusName(R.Status);
      Evidence["controlComplete"] = R.complete();
      Evidence["contexts"] = static_cast<int64_t>(R.Contexts);
      Evidence["nodeEvaluations"] = static_cast<int64_t>(R.NodeEvaluations);
      Evidence["evaluatedOperations"] =
          static_cast<int64_t>(R.EvaluatedOperations);
      Evidence["solverQueries"] = static_cast<int64_t>(R.SolverQueries);
      Evidence["relationalWidenings"] = R.RelationalWidenings;
      Evidence["discoveredControlFields"] = R.DiscoveredControlFields;
      Evidence["discoveredContextFields"] = R.DiscoveredContextFields;
      Evidence["controlRefinements"] = R.ControlRefinements;
      Evidence["discoveryVisits"] = static_cast<int64_t>(R.DiscoveryVisits);
      Evidence["residualBlocks"] =
          static_cast<int64_t>(R.Residual.Blocks.size());
      llvm::json::Array Reads;
      for (const auto &Read : R.Reads)
        Reads.push_back(llvm::json::Object{
            {"instruction", vaHex(Read.InstructionAddress)},
            {"operation", Read.OpSeq},
            {"address", vaHex(Read.Address)},
            {"bytes", llvm::toHex(llvm::ArrayRef<uint8_t>(Read.Bytes))},
            {"evidence", Read.Evidence}});
      Evidence["immutableReads"] = std::move(Reads);
      llvm::json::Array Origins;
      for (const auto &Origin : R.Origins)
        Origins.push_back(llvm::json::Object{
            {"residual", vaHex(Origin.ResidualAddress)},
            {"native", vaHex(Origin.NativeInstruction.Address)},
            {"size", Origin.NativeInstruction.Size}});
      Evidence["origins"] = std::move(Origins);
    }
    if (!Result.Success || Result.MedIRVerifierFailures ||
        Result.LLVMVerifierFailed || Result.BackendUnhandledValueIntrinsics)
      return Fail(Result.Error.empty() ? "recovered IR verification failed"
                                       : Result.Error);
    if (Result.MedFuncs.empty())
      return Fail("recovery produced no source input-domain evidence");
    for (const auto &Function : Result.MedFuncs) {
      if (!PO.LiftMode && Function.SkippedSSA)
        return Fail("recovered mutable source requires LLVM output; HighC "
                    "lowering is incomplete");
      const LowFunc *SourceFrame = nullptr;
      if (!MachineState) {
        const auto Count = std::count_if(
            Result.LowFuncs.begin(), Result.LowFuncs.end(),
            [&](const auto &Low) { return Low.Entry == Function.Entry; });
        if (Count == 1)
          SourceFrame = &*std::find_if(
              Result.LowFuncs.begin(), Result.LowFuncs.end(),
              [&](const auto &Low) { return Low.Entry == Function.Entry; });
      }
      if (const auto Limitation =
              medRecoverySourceLimitation(Function, S->Img.Arch, SourceFrame);
          !Limitation.empty())
        return Fail(Limitation);
    }
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    if (MachineState)
      OS << "/* Explicit x64 machine-state source ABI v1.\n"
            " * Pass 17 aligned uint64_t words: RAX, RCX, RDX, RBX, RSP, RBP,\n"
            " * RSI, RDI, R8..R15, RFLAGS. Guest memory uses original "
            "addresses.\n"
            " * Normal nonfaulting CPL3/IOPL0 execution; CET disabled.\n"
            " * State storage must not alias guest memory. Zero status is "
            "success;\n"
            " * nonzero status invalidates the result and does not undo "
            "stores.\n"
            " * Every guest write range must be disjoint from the entry "
            "return slot.\n"
            " * State is captured before the final native return-address pop.\n"
            " */\n";
    else
      OS << "/* Ordinary recovery source frame contract: every "
            "external-origin\n"
            " * LOAD/STORE range, including computed external addresses, must\n"
            " * be disjoint from the native invocation-private frame and its\n"
            " * reconstructed source storage.\n"
            " */\n";
    if (Config.EntryFrameBounds)
      OS << "/* Numeric entry-RSP precondition: for every signed offset in ["
         << Config.EntryFrameBounds->Begin << ","
         << Config.EntryFrameBounds->End
         << "),\n"
            " * entry RSP + offset must fit in [0,UINT64_MAX] without "
            "wrapping.\n"
            " * This premise is not checked at runtime and grants no memory\n"
            " * accessibility, initialization or nonalias guarantees.\n"
            " */\n";
    if (Config.EntryFrameAlignment)
      OS << "/* Checked entry RSP modulo "
         << Config.EntryFrameAlignment->Alignment
         << " == " << Config.EntryFrameAlignment->Residue
         << ". Other roots return status 2\n"
            " * before guest accesses or state writes. No memory guarantees.\n"
            " */\n";
    CEmitterOptions EmitOptions;
    EmitOptions.TheArch = S->Img.Arch;
    EmitOptions.Format = S->Img.Format;
    // The machine-state ABI preserves guest virtual addresses and accesses
    // their original mappings. Image-backed source objects would silently
    // change both observable register values and guest memory identity.
    EmitOptions.Image = Result.InterpreterMachineSourceABI ? nullptr : &S->Img;
    if (PO.LiftMode) {
      if (!Result.LlvmModule)
        return Fail("recovery produced no LLVM module");
      if (const auto Limitation =
              llvmRecoverySourceLimitation(*Result.LlvmModule);
          !Limitation.empty())
        return Fail(Limitation);
      LLVMCEmitter Emitter;
      if (!Emitter.emit(*Result.LlvmModule, OS, EmitOptions, nullptr,
                        EmitOptions.Image))
        return Fail("recovered LLVM-to-C emission failed");
    } else {
      if (Result.HighFuncs.empty())
        return Fail("recovery produced no source function");
      for (const auto &Function : Result.HighFuncs)
        if (const auto Limitation = highRecoverySourceLimitation(Function);
            !Limitation.empty())
          return Fail(Limitation);
      HighCEmitter Emitter;
      if (!Emitter.emit(Result.HighFuncs, OS, EmitOptions))
        return Fail("recovered HighC emission failed");
    }
    Evidence["complete"] = true;
    PublishReport();
    return dupStr(Source);
  } catch (const std::exception &Error) {
    return Fail(Error.what());
  }
}

extern "C" const char *
neverd_devirtualize_source_v1(neverd_session_t Session, neverd_va_t Entry,
                              const neverd_devirtualize_options_v1 *Options,
                              const char **Report) {
  return devirtualizeSource(Session, Entry, Options, Report, false);
}

extern "C" const char *neverd_devirtualize_machine_source_v1(
    neverd_session_t Session, neverd_va_t Entry,
    const neverd_devirtualize_options_v1 *Options, const char **Report) {
  return devirtualizeSource(Session, Entry, Options, Report, true);
}

extern "C" const char *
neverd_devirtualize_source_v2(neverd_session_t Session, neverd_va_t Entry,
                              const neverd_devirtualize_options_v2 *Options,
                              const char **Report) {
  return devirtualizeSource(Session, Entry, Options ? &Options->base : nullptr,
                            Report, false, Options);
}

extern "C" const char *neverd_devirtualize_machine_source_v2(
    neverd_session_t Session, neverd_va_t Entry,
    const neverd_devirtualize_options_v2 *Options, const char **Report) {
  return devirtualizeSource(Session, Entry, Options ? &Options->base : nullptr,
                            Report, true, Options);
}

extern "C" const char *
neverd_devirtualize_source_v3(neverd_session_t Session, neverd_va_t Entry,
                              const neverd_devirtualize_options_v3 *Options,
                              const char **Report) {
  return devirtualizeSource(Session, Entry,
                            Options ? &Options->base.base : nullptr, Report,
                            false, Options ? &Options->base : nullptr, Options);
}

extern "C" const char *neverd_devirtualize_machine_source_v3(
    neverd_session_t Session, neverd_va_t Entry,
    const neverd_devirtualize_options_v3 *Options, const char **Report) {
  return devirtualizeSource(Session, Entry,
                            Options ? &Options->base.base : nullptr, Report,
                            true, Options ? &Options->base : nullptr, Options);
}

extern "C" const char *
neverd_devirtualize_source_v4(neverd_session_t Session, neverd_va_t Entry,
                              const neverd_devirtualize_options_v4 *Options,
                              const char **Report) {
  return devirtualizeSource(
      Session, Entry, Options ? &Options->base.base.base : nullptr, Report,
      false, Options ? &Options->base.base : nullptr,
      Options ? &Options->base : nullptr, Options);
}

extern "C" const char *neverd_devirtualize_machine_source_v4(
    neverd_session_t Session, neverd_va_t Entry,
    const neverd_devirtualize_options_v4 *Options, const char **Report) {
  return devirtualizeSource(
      Session, Entry, Options ? &Options->base.base.base : nullptr, Report,
      true, Options ? &Options->base.base : nullptr,
      Options ? &Options->base : nullptr, Options);
}

extern "C" const char *
neverd_devirtualize_source_v5(neverd_session_t Session, neverd_va_t Entry,
                              const neverd_devirtualize_options_v5 *Options,
                              const char **Report) {
  return devirtualizeSource(
      Session, Entry, Options ? &Options->base.base.base.base : nullptr, Report,
      false, Options ? &Options->base.base.base : nullptr,
      Options ? &Options->base.base : nullptr,
      Options ? &Options->base : nullptr, Options);
}

extern "C" const char *neverd_devirtualize_machine_source_v5(
    neverd_session_t Session, neverd_va_t Entry,
    const neverd_devirtualize_options_v5 *Options, const char **Report) {
  return devirtualizeSource(
      Session, Entry, Options ? &Options->base.base.base.base : nullptr, Report,
      true, Options ? &Options->base.base.base : nullptr,
      Options ? &Options->base.base : nullptr,
      Options ? &Options->base : nullptr, Options);
}

#include "LocalTypesDebugContext.h"
#include "SessionImpl.h"
#include "SourceEdits.h"

#include "neverd/ir/SourceCallABI.h"
#include "neverd/ir/low/SourceCallOccurrence.h"
#include "neverd/sdk/Limits.h"
#include "neverd/types/TypeParse.h"

#include "llvm/ADT/StringExtras.h"
using namespace neverd;
using namespace neverd::sdk;

std::string neverd::sdk::variableCallProfile(std::string Snapshot,
                                             const SourceViewOptions &Options) {
  for (const auto &Edit : Options.CallTypes) {
    for (const auto *Text : {&Edit.Id, &Edit.Snapshot, &Edit.Declaration})
      Snapshot += ":" + std::to_string(Text->size()) + ":" + *Text;
    for (const auto &Type : Edit.VariadicTypes)
      Snapshot += ":" + std::to_string(Type.size()) + ":" + Type;
  }
  if (!Options.StackFrame.empty()) {
    Snapshot += Options.StackFrame.Snapshot;
    for (const auto &[PC, Offset] : Options.StackFrame.Points)
      Snapshot += ":sp:" + std::to_string(PC) + ":" + std::to_string(Offset);
    for (const auto &V : Options.StackFrame.Variables)
      Snapshot += ":slot:" + std::to_string(V.Offset) + ":" +
                  std::to_string(V.Bytes) + ":" +
                  std::to_string(V.Name.size()) + ":" + V.Name + ":" + V.Type;
  }
  return Options.CallTypes.empty() && Options.StackFrame.empty()
             ? Snapshot
             : sourceVariableSnapshot(Snapshot);
}

std::string neverd::sdk::sourceCallId(const SourceCallOccurrenceKey &Site) {
  return "call:" + llvm::utohexstr(Site.Instruction) + ":" +
         std::to_string(Site.Sequence) + ":" +
         std::to_string(static_cast<unsigned>(Site.Opcode)) + ":" +
         (Site.StaticTarget ? llvm::utohexstr(*Site.StaticTarget) : "indirect");
}
std::string neverd::sdk::sourceCallSnapshot(const LowFunc &Function) {
  SnapshotStream OS;
  OS << "call-site-v1:" << Function.Entry << ':' << Function.OriginalSize;
  size_t Work = 0;
  auto Variable = [&](const NdVar &V) {
    OS << ':' << unsigned(V.Space) << ':' << V.Offset << ':' << V.Size << ':'
       << unsigned(V.Provenance) << ':' << V.AddressOwnerVA;
  };
  for (const auto &Block : Function.Blocks) {
    OS << "B" << Block.Id << ':' << Block.StartAddr << ':' << Block.EndAddr;
    for (auto Successor : Block.Succs)
      OS << ':' << Successor;
    for (const auto &Op : Block.Ops) {
      if (++Work > kMaxCallSnapshotOperations ||
          OS.tell() > kMaxCallSnapshotBytes)
        return {};
      OS << "O" << unsigned(Op.Opcode) << ':' << Op.Addr << ':' << Op.Seq << ':'
         << unsigned(Op.MemoryOrdering) << ':'
         << unsigned(Op.MemoryAddressSpace);
      Variable(Op.Output);
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        Variable(Op.Inputs[I]);
    }
  }
  return OS.snapshot();
}

std::optional<SourceCallTypeHint>
neverd::sdk::parseSourceCallType(Session &S, const SourceCallEdit &Edit,
                                 const SourceCallOccurrenceKey &Site,
                                 std::string &Error) {
  if (Edit.Declaration.empty() ||
      Edit.Declaration.size() > kMaxFunctionDeclarationBytes ||
      Edit.Declaration.find('\0') != std::string::npos ||
      Edit.VariadicTypes.size() > kMaxCallVariadicTypes) {
    Error = "A call-site declaration must be bounded C text with at most 32 "
            "actual arguments";
    return {};
  }
  const auto *Library = S.readLocalTypes();
  if (!Library) {
    Error = S.LastError;
    return {};
  }
  auto Read = types::parseFunctionDeclaration(Edit.Declaration, *Library);
  if (!Read.ok() || !Read.Function) {
    Error = Read.Problems.empty() ? "Enter a complete C function declaration"
                                  : Read.Problems.front().Message;
    return {};
  }
  const auto &Function = *Read.Function;
  if (Function.NoReturn ||
      (Function.Convention != types::CallingConvention::Unspecified &&
       Function.Convention != types::CallingConvention::Cdecl)) {
    Error = "Call-site declarations use the image's ordinary C ABI and cannot "
            "override control-flow effects";
    return {};
  }
  if ((!Function.Variadic && !Edit.VariadicTypes.empty()) ||
      (Function.Variadic && Function.Params.empty())) {
    Error = "Actual variadic types require a declaration with a nonempty fixed "
            "prefix followed by ...";
    return {};
  }
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::Analyst;
  Hint.AnalystSite = Site;
  Hint.TargetAddress = Site.StaticTarget.value_or(0);
  if (Site.StaticTarget) {
    Hint.TargetName =
        (kAutoFuncPrefix + llvm::utohexstr(*Site.StaticTarget)).str();
    for (const auto &Symbol : S.Img.Symbols)
      if (Symbol.Addr == *Site.StaticTarget && Symbol.IsFunc &&
          !Symbol.Name.empty()) {
        Hint.TargetName = Symbol.Name;
        break;
      }
  }
  Hint.AnalystFixedCount =
      Function.Variadic ? static_cast<int>(Function.Params.size()) : -1;
  Hint.Signature.ReturnType = Function.Return
                                  ? sourceCallTypeOf(*Library, *Function.Return)
                                  : NdType::makeVoid();
  for (const auto &P : Function.Params) {
    const auto Type = P.Type ? sourceCallTypeOf(*Library, *P.Type) : nullptr;
    Hint.Signature.Parameters.push_back({P.Name, Type});
  }
  for (const auto &Text : Edit.VariadicTypes) {
    if (Text.empty() || Text.size() > kMaxFunctionDeclarationBytes ||
        Text.find('\0') != std::string::npos) {
      Error = "Invalid actual variadic type";
      return {};
    }
    auto Actual = types::parseFunctionDeclaration(
        "void __argument(" + Text + ")", *Library);
    if (!Actual.ok() || Actual.Function->Variadic ||
        Actual.Function->Params.size() != 1 ||
        !Actual.Function->Params.front().Name.empty() ||
        !Actual.Function->Params.front().Type) {
      Error = "Each actual variadic argument needs one type without a variable "
              "name";
      return {};
    }
    Hint.Signature.Parameters.push_back(
        {"",
         sourceCallTypeOf(*Library, *Actual.Function->Params.front().Type)});
  }
  if (!assignAnalystCallABI(Hint, S.Img.Arch, S.Img.abiFormat(), Error))
    return {};
  return Hint;
}

void neverd::sdk::prepareSourceCallVariant(Session &S, va_t Entry,
                                           const SourceViewOptions &Options) {
  const auto *Function = S.findLowFunc(Entry);
  const auto Snapshot =
      Function ? sourceCallSnapshot(*Function) : std::string();
  if (S.CallVariant && S.CallVariant->Entry == Entry &&
      S.CallVariant->Snapshot == Snapshot &&
      S.CallVariant->Symbolic == Options.Symbolic &&
      S.CallVariant->Edits == Options.CallTypes &&
      S.CallVariant->Frame == Options.StackFrame)
    return;
  auto Variant = std::make_unique<Session::CallIRVariant>();
  Variant->Entry = Entry;
  Variant->Snapshot = Snapshot;
  Variant->Symbolic = Options.Symbolic;
  Variant->Edits = Options.CallTypes;
  Variant->Frame = Options.StackFrame;
  std::map<std::string, SourceCallOccurrenceKey> Sites;
  if (Function)
    for (const auto &Block : Function->Blocks)
      for (const auto &Op : Block.Ops)
        if (const auto Site = sourceCallOccurrenceKey(Op))
          Sites[sourceCallId(*Site)] = *Site;
  PipelineOptions Policy;
  S.applyAnalysisOptions(Policy);
  Policy.OnlyFunctionEntries = {Entry};
  Policy.SymbolicHighC = Options.Symbolic;
  Policy.AnalysisCache = nullptr;
  for (const auto &Edit : Options.CallTypes) {
    SourceCallEditResult Status{Edit.Id};
    if (Snapshot.empty() || Edit.Snapshot != Snapshot ||
        !Sites.count(Edit.Id)) {
      Status.Refusal = "The original call analysis changed; reset or reapply "
                       "the call-site declaration";
    } else {
      auto Hint =
          parseSourceCallType(S, Edit, Sites.at(Edit.Id), Status.Refusal);
      if (Hint) {
        const auto Bias = sourceCallReturnAddressBias(
            &S.Img, S.Img.Arch, Hint->AnalystSite->Instruction);
        bool Stack = false;
        for (const auto &P : Hint->Signature.Parameters)
          Stack |= P.Location.Kind == SourceABICarrierKind::Stack;
        if (Stack && !Bias)
          Status.Refusal = "The original call instruction has no proven stack "
                           "return-address bias";
        else if (sourceCallVariadicStateDefined(
                     *Function, *Hint, S.Img.abiFormat(), Status.Refusal) &&
                 sourceCallStackArgumentsDefined(*Function, *Hint, S.Img.Arch,
                                                 Bias.value_or(0),
                                                 Status.Refusal)) {
          Policy.SourceCallSiteHints[Entry].emplace(*Hint->AnalystSite, *Hint);
          Variant->Hints.emplace(Edit.Id, *Hint);
          Status.Applied = true;
        }
      }
    }
    Variant->Results.push_back(std::move(Status));
  }
  if (!Options.StackFrame.empty()) {
    if (!Function)
      throw std::invalid_argument("Original stack analysis is unavailable");
    validateSourceStackFrame(S, *Function, Options.StackFrame);
    Policy.SourceStackFrames[Entry] = Options.StackFrame;
  }
  if (!Policy.SourceCallSiteHints[Entry].empty() ||
      !Policy.SourceStackFrames.empty()) {
    std::string Error;
    try {
      auto Result =
          std::make_unique<PipelineResult>(Pipeline().rebuildSourceCalls(
              S.Img, Policy, S.PipeResult, S.decompileDebug()));
      if (!Result->Success || Result->HighFuncs.empty())
        Error = Result->Error.empty()
                    ? "The private call-site analysis could not be completed"
                    : Result->Error;
      else {
        for (const auto &[Site, Hint] : Policy.SourceCallSiteHints[Entry]) {
          const auto It =
              std::find_if(Result->MedFuncs.begin(), Result->MedFuncs.end(),
                           [&](const MedFunc &F) { return F.Entry == Entry; });
          if (It == Result->MedFuncs.end()) {
            Error = "The private call-site analysis lost its function";
            break;
          }
          if (!sourceCallArgumentsDefined(*It, Site, Error))
            break;
        }
        if (Error.empty())
          Variant->Result = std::move(Result);
      }
    } catch (const std::exception &E) {
      Error = E.what();
    }
    if (!Error.empty())
      for (auto &Status : Variant->Results)
        if (Status.Applied) {
          Status.Applied = false;
          Status.Refusal = Error;
        }
  }
  if (!Options.StackFrame.empty() && !Variant->Result)
    throw std::invalid_argument(
        "The corrected stack frame could not be projected to source");
  S.CallVariant = std::move(Variant);
}

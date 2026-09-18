#include "neverd/pipeline/NativeSourceHints.h"

#include "NativeSourcePreservation.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighSourceFlow.h"
#include "neverd/ir/med/MedNoReturn.h"
#include "neverd/ir/med/MedSourceParameterUses.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/pipeline/Pipeline.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <tuple>

namespace neverd {
namespace {
bool integerCarrier(const TypeRef &Type) {
  return Type &&
         ((Type->Kind == NdTypeKind::Int &&
           (Type->Size == 1 || Type->Size == 2 || Type->Size == 4 ||
            Type->Size == 8)) ||
          (Type->Kind == NdTypeKind::Ptr && Type->Size == 8 && Type->Pointee));
}

bool scalarCarrier(const TypeRef &Type) {
  return integerCarrier(Type) || (Type && Type->Kind == NdTypeKind::Float &&
                                  (Type->Size == 4 || Type->Size == 8));
}

bool sameScalar(const TypeRef &A, const TypeRef &B) {
  return scalarCarrier(A) && scalarCarrier(B) && A->Kind == B->Kind &&
         A->Size == B->Size && A->IsSigned == B->IsSigned;
}

bool completeNativeAudit(va_t Entry, const PipelineFunctionAudit &Audit) {
  return Entry == Audit.Entry &&
         Audit.Disposition == PipelineFunctionDisposition::Accepted &&
         Audit.HasLowIR && Audit.HasMedIR && Audit.MedIRVerified &&
         Audit.DecodedInstructions &&
         Audit.DecodedInstructions == Audit.LiftedInstructions &&
         Audit.DecodeFailures.empty() &&
         Audit.UnsupportedInstructions.empty() && Audit.TruncatedPaths.empty();
}

bool hasNativeSourceStateContract(const BinaryImage &Image, const LowFunc *Low,
                                  const MedFunc &Med, bool RequireCalls);

// These are internal source parameters, not a guessed external convention.
// Use MedIR's observable entry-byte analysis and independent full-width native
// reads. Caller-saved input registers may subsequently become scratch values.
// Preserved context inputs require either no native writes to preserved
// non-frame registers or an independent proof that every exit restores state.
// Saving and restoring scratch registers does not create hidden outputs.
// The complete source body and its callers still require validation.
std::vector<uint64_t> nativeEntryRegisters(const BinaryImage &Image,
                                           const LowFunc *Low,
                                           const MedFunc &Med,
                                           const SourceFunctionTypeHint &Hint) {
  if (!Low || Low->Entry != Med.Entry || Low->Blocks.empty() ||
      Low->Blocks.size() > 16384)
    return {};
  auto Observed = observedMedSourceEntryRegisters(Med, Hint);
  // An earlier, narrow call result can leave an unobserved high word in a
  // return register. It cannot invalidate independent evidence that an entry
  // context reaches an effect. This fallback only adds observed inputs; result
  // proof and final source validation remain separate requirements.
  if (!Observed)
    Observed = observedMedSourceEntryRegisters(Med, Hint,
                                               SourceEntryDemand::EffectsOnly);
  if (!Observed)
    return {};
  const auto &TRI = getTargetRegInfo(Hint.Architecture);
  auto IntegerRegister = [&](uint64_t Register) {
    return !TRI.isFrameOrLinkReg(Register) &&
           (Hint.Architecture == Arch::AArch64
                ? Register <= a64reg::X28 && Register % 8 == 0
                : TRI.isGeneralReg(Register));
  };
  const auto Preserved = TRI.callPreservedRanges(BinaryFormat::MachO);
  size_t Remaining = 262144;
  std::set<uint64_t> Reads;
  bool PreservedWrite = false;
  for (const auto &Block : Low->Blocks)
    for (const auto &Op : Block.Ops) {
      if (!Remaining-- || Op.NumInputs > 6)
        return {};
      const auto &Output = Op.Output;
      if (Output.isReg() && Output.Size &&
          !(TRI.isFrameOrLinkReg(Output.Offset) && Output.Size <= 8))
        for (const auto &Range : Preserved)
          if (!TRI.isFrameOrLinkReg(Range.Offset) &&
              (Output.Offset <= Range.Offset
                   ? Range.Offset - Output.Offset < Output.Size
                   : Output.Offset - Range.Offset < Range.Bytes))
            PreservedWrite = true;
      for (unsigned I = 0; I < Op.NumInputs; ++I) {
        if (!Remaining--)
          return {};
        const auto &Input = Op.Inputs[I];
        if (Input.isReg() && Input.Size == 8 && IntegerRegister(Input.Offset) &&
            Observed->count(Input.Offset))
          Reads.insert(Input.Offset);
      }
    }
  const bool ReadsPreserved =
      std::any_of(Reads.begin(), Reads.end(), [&](uint64_t Register) {
        return TRI.isCallPreserved(Register, 8);
      });
  if (PreservedWrite && ReadsPreserved &&
      !hasNativeSourceStateContract(Image, Low, Med, false))
    std::erase_if(Reads, [&](uint64_t Register) {
      return TRI.isCallPreserved(Register, 8);
    });
  return {Reads.begin(), Reads.end()};
}

// Typed two-word calls materialize their physical results through SUBBYTES.
// Authenticate the complete lowering prefix before treating those extracts
// as register definitions; ordinary SUBBYTES operations are only views.
bool completeCallResultPrefix(llvm::ArrayRef<MedOp> Ops, size_t Index,
                              Arch Architecture) {
  const auto &Call = Ops[Index];
  if (Ops.size() - Index < 3 || !Call.SourceCallHint ||
      Call.Output.Kind != MedVar::Temp || Call.Output.Id < 0 ||
      Call.Output.Size != 16)
    return false;
  const auto &Signature = Call.SourceCallHint->Signature;
  const auto &TRI = getTargetRegInfo(Architecture);
  if (!Signature.HasExplicitABI || Signature.Architecture != Architecture ||
      !Signature.ReturnType || Signature.ReturnType->Size != 16 ||
      Signature.ReturnComponents.size() != 2 || TRI.IntReturnRegs.size() < 2)
    return false;
  for (unsigned I = 0; I < 2; ++I) {
    const auto &Location = Signature.ReturnComponents[I];
    const auto &Extract = Ops[Index + I + 1];
    if (Location.Kind != SourceABICarrierKind::IntegerRegister ||
        Location.RegisterOffset != TRI.IntReturnRegs[I] ||
        Location.ValueBytes != 8 || Extract.Opcode != NdOp::SUBBYTES ||
        Extract.NumInputs != 2 || Extract.Output.Kind != MedVar::Reg ||
        Extract.Output.RegOff != Location.RegisterOffset ||
        Extract.Output.Size != 8 || Extract.Inputs[0] != Call.Output ||
        Extract.Inputs[0].Size != 16 || !Extract.Inputs[1].isConst() ||
        Extract.Inputs[1].ConstVal != I * 8U)
      return false;
  }
  return true;
}

// A source-bound helper can have no usable scalar result. An internal void
// summary preserves its effects and deliberately supplies no result to
// callers. Callees may return values used inside the helper; those values do
// not establish a result on every exit from the helper itself.
// Framed and ordinary-call shapes require exact state restoration;
// the established frameless tail shape uses the narrower no-write proof plus
// byte-taint rejection of stack-derived arguments and stores.
bool hasNativeSourceStateContract(const BinaryImage &Image, const LowFunc *Low,
                                  const MedFunc &Med, bool RequireCalls) {
  if (!Low || Low->Entry != Med.Entry || Low->Blocks.empty() ||
      Low->Blocks.size() > 16384)
    return false;
  NativeSourceCalls Calls;
  size_t Remaining = 262144;
  for (const auto &Block : Med.Blocks)
    for (const auto &Op : Block.Ops) {
      if (!Remaining--)
        return false;
      if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
        continue;
      if (Op.OriginSeq < 0 || !Op.NumInputs || Op.Inputs[0].Size != 8 ||
          !Op.SourceCallHint)
        return false;
      const auto &Binding = *Op.SourceCallHint;
      using Kind = SourceCallTypeHint::Kind;
      const bool StaticRuntime =
          Op.Inputs[0].isConst() &&
          (Binding.CallKind == Kind::ObjCRuntimeCall ||
           Binding.CallKind == Kind::SwiftRuntimeCall ||
           Binding.CallKind == Kind::DarwinRuntimeCall ||
           Binding.CallKind == Kind::SwiftStringBridge ||
           Binding.CallKind == Kind::SwiftStringFromNSString);
      const bool StaticMessage =
          Op.Inputs[0].isConst() &&
          (Binding.CallKind == Kind::ObjCMessage ||
           Binding.CallKind == Kind::ObjCSuper2) &&
          (Binding.Signature.Origin ==
               SourceFunctionTypeHint::OriginKind::ObjCRuntime ||
           Binding.Signature.Origin ==
               SourceFunctionTypeHint::OriginKind::ObjCSDK) &&
          Binding.Signature.HasExplicitABI;
      const bool StaticNative =
          Op.Inputs[0].isConst() && Binding.CallKind == Kind::Native &&
          Binding.TargetAddress == Op.Inputs[0].ConstVal &&
          Binding.TargetAddress != Med.Entry &&
          Image.isCodeAddress(Binding.TargetAddress) &&
          Binding.Signature.Origin ==
              SourceFunctionTypeHint::OriginKind::NativeAnalysis &&
          Binding.Signature.HasExplicitABI;
      const bool DynamicWitness =
          Op.Opcode == NdOp::INDIR_CALL && !Op.Inputs[0].isConst() &&
          isSwiftValueWitnessSourceCallHint(Binding, Image.Arch);
      if ((!StaticRuntime && !StaticNative && !StaticMessage &&
           !DynamicWitness) ||
          Binding.DoesNotReturn || !Binding.Signature.ReturnType ||
          !Image.isCodeAddress(Op.Addr) ||
          !Calls
               .emplace(NativeSourceCallKey{Op.Addr, Op.OriginSeq, Op.Opcode,
                                            Op.Inputs[0].isConst()
                                                ? std::optional<va_t>(
                                                      Op.Inputs[0].ConstVal)
                                                : std::nullopt},
                        &Binding.Signature)
               .second)
        return false;
    }
  if (RequireCalls && Calls.empty())
    return false;
  std::set<NativeSourceCallKey> NativeCalls;
  for (const auto &Block : Low->Blocks)
    for (const auto &Op : Block.Ops) {
      if (!Remaining-- || Op.NumInputs > 6)
        return false;
      if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
        continue;
      const auto Key = nativeSourceCallKey(Op);
      if (!Key)
        return false;
      if (!Calls.count(*Key) || !NativeCalls.insert(*Key).second)
        return false;
    }
  return NativeCalls.size() == Calls.size() &&
         (restoresNativeSourceState(*Low, Image.Arch, Calls) ||
          preservesNativeSourceLeafState(*Low, Image.Arch, Calls));
}

// This proves a defined machine carrier, not an original return declaration.
// An observed full-width parameter can supply the initial register value.
// PHIs merge physical state, and calls or partial writes invalidate it until
// a complete result is computed again.
bool definedReturnPaths(const MedFunc &Function, Arch Architecture,
                        const SourceABIValueLocation &Location,
                        const std::optional<MedVar> &IncomingParameter) {
  const auto ReturnRegister = Location.RegisterOffset;
  const auto Width = Location.ValueBytes;
  const size_t Count = Function.Blocks.size();
  if (!Count || Count > 16384)
    return false;
  size_t Remaining = 262144;
  std::map<int, size_t> Index;
  std::optional<size_t> Entry;
  bool HasAddresses = false;
  for (size_t I = 0; I < Count; ++I) {
    const auto &Block = Function.Blocks[I];
    if (Block.Id < 0 || !Index.emplace(Block.Id, I).second)
      return false;
    HasAddresses |= Block.StartAddr != 0;
    if (Block.StartAddr == Function.Entry) {
      if (Entry)
        return false;
      Entry = I;
    }
  }
  if (!Entry && !HasAddresses && Index.count(0))
    Entry = Index.at(0);
  if (!Entry)
    return false;
  std::vector<std::set<size_t>> Preds(Count), Succs(Count);
  for (size_t I = 0; I < Count; ++I)
    for (int Successor : Function.Blocks[I].Succs) {
      const auto Found = Index.find(Successor);
      if (!Remaining-- || Found == Index.end() ||
          !Succs[I].insert(Found->second).second)
        return false;
      Preds[Found->second].insert(I);
    }
  for (size_t I = 0; I < Count; ++I) {
    std::set<size_t> Declared;
    for (int Predecessor : Function.Blocks[I].Preds) {
      const auto Found = Index.find(Predecessor);
      if (!Remaining-- || Found == Index.end() ||
          !Declared.insert(Found->second).second)
        return false;
    }
    if (Declared != Preds[I])
      return false;
  }
  std::vector<bool> Reachable(Count);
  std::vector<size_t> Visit{*Entry};
  Reachable[*Entry] = true;
  for (size_t I = 0; I < Visit.size(); ++I)
    for (size_t Successor : Succs[Visit[I]])
      if (!Reachable[Successor]) {
        Reachable[Successor] = true;
        Visit.push_back(Successor);
      }
  const auto &TRI = getTargetRegInfo(Architecture);
  // Nonconstant MedVar equality is kind/id/version. Version zero alone is
  // not an entry identity: an internal definition can receive that version.
  using ValueKey = std::tuple<MedVar::VarKind, int, int>;
  std::set<ValueKey> Definitions, IncomingUses;
  auto Define = [&](const MedVar &Value) {
    if (IncomingParameter && Value.Size &&
        (Value.Kind == MedVar::Reg || Value.Kind == MedVar::Param))
      Definitions.emplace(Value.Kind, Value.Id, Value.SSAVer);
  };
  std::vector<std::optional<bool>> Transfer(Count);
  std::vector<std::pair<size_t, std::optional<bool>>> Returns;
  for (size_t I = 0; I < Count; ++I) {
    auto &Fact = Transfer[I];
    if (IncomingParameter)
      for (const auto &Phi : Function.Blocks[I].Phis) {
        if (!Remaining--)
          return false;
        Define(Phi.Output);
      }
    const auto &Ops = Function.Blocks[I].Ops;
    size_t CallResultEnd = 0;
    for (size_t OpIndex = 0; OpIndex < Ops.size(); ++OpIndex) {
      const auto &Op = Ops[OpIndex];
      if (!Remaining--)
        return false;
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        Fact = false;
        if (Remaining < 2)
          return false;
        Remaining -= 2;
        CallResultEnd = completeCallResultPrefix(Ops, OpIndex, Architecture)
                            ? OpIndex + 3
                            : 0;
      }
      const bool Seed = Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
                        Op.Output == Op.Inputs[0] &&
                        Op.Output.Size == Op.Inputs[0].Size;
      if (!Seed)
        Define(Op.Output);
      // Params alone do not prove that a generic ABI placeholder was read.
      // Only actual reachable uses of the entry version can supply this
      // initial fact; SSA seeds, return markers and PHIs cannot invent it.
      if (IncomingParameter && Reachable[I] && !Seed &&
          Op.Opcode != NdOp::RETURN)
        for (unsigned J = 0; J < Op.NumInputs; ++J) {
          if (!Remaining--)
            return false;
          const auto &Input = Op.Inputs[J];
          if (((Input.Kind == MedVar::Reg && Input.SSAVer == 0) ||
               (Input.Kind == MedVar::Param && Input == *IncomingParameter)) &&
              Input.RegOff == IncomingParameter->RegOff && Input.Size >= Width)
            IncomingUses.emplace(Input.Kind, Input.Id, Input.SSAVer);
        }
      const auto &Out = Op.Output;
      if (!Seed && (Op.Opcode != NdOp::SUBBYTES || OpIndex < CallResultEnd) &&
          Out.Kind == MedVar::Reg && Out.Size &&
          (Out.RegOff <= ReturnRegister
               ? ReturnRegister - Out.RegOff < Out.Size
               : Out.RegOff - ReturnRegister < Width)) {
        const bool StackRestore =
            Architecture == Arch::X64 && Op.Opcode == NdOp::LOAD &&
            Op.NumInputs == 1 && Op.Inputs[0].Kind == MedVar::Reg &&
            Op.Inputs[0].RegOff == TRI.StackPointer;
        Fact =
            Out.RegOff == ReturnRegister && Out.Size >= Width && !StackRestore;
      }
      if (Op.Opcode == NdOp::RETURN)
        Returns.emplace_back(I, Fact);
    }
  }
  const bool ObservedIncoming =
      std::any_of(IncomingUses.begin(), IncomingUses.end(),
                  [&](const ValueKey &Key) { return !Definitions.count(Key); });
  // Greatest fixed point of definite availability. Only inherited facts can
  // fall from true to false; a local complete write establishes its own fact.
  // Entry needs its observed parameter; disconnected components have no
  // initial result. Backedges must agree with the initial entry fact too.
  std::vector<bool> Outgoing(Count);
  std::deque<size_t> Unknown;
  for (size_t I = 0; I < Count; ++I) {
    Outgoing[I] = Transfer[I].value_or(
        Reachable[I] && (I == *Entry ? ObservedIncoming : !Preds[I].empty()));
    if (!Outgoing[I])
      Unknown.push_back(I);
  }
  while (!Unknown.empty()) {
    const size_t I = Unknown.front();
    Unknown.pop_front();
    for (size_t Successor : Succs[I]) {
      if (!Remaining--)
        return false;
      if (!Transfer[Successor] && Outgoing[Successor]) {
        Outgoing[Successor] = false;
        Unknown.push_back(Successor);
      }
    }
  }
  // Compute each meet once even when a malformed block has many returns.
  std::vector<bool> Incoming(Count);
  for (size_t I = 0; I < Count; ++I)
    Incoming[I] =
        Reachable[I] && (I == *Entry ? ObservedIncoming : !Preds[I].empty()) &&
        std::all_of(Preds[I].begin(), Preds[I].end(),
                    [&](size_t Predecessor) { return Outgoing[Predecessor]; });
  for (const auto &[I, Fact] : Returns)
    if (Location.Kind != SourceABICarrierKind::None &&
        !Fact.value_or(Incoming[I]))
      return false;
  return !Returns.empty();
}

std::optional<SourceFunctionTypeHint>
integerPairReturn(const MedFunc &Med, const SourceFunctionTypeHint &Scalar) {
  const auto Architecture = Scalar.Architecture;
  if (Architecture != Arch::AArch64 && Architecture != Arch::X64)
    return std::nullopt;
  const auto &TRI = getTargetRegInfo(Architecture);
  std::string Error;
  if (Scalar.Origin != SourceFunctionTypeHint::OriginKind::NativeAnalysis ||
      !Scalar.HasExplicitABI || !validateSourceABI(Scalar, Error) ||
      !integerCarrier(Scalar.ReturnType) || Scalar.ReturnType->Size != 8 ||
      !Scalar.ReturnComponents.empty() ||
      Scalar.ReturnLocation.Kind != SourceABICarrierKind::IntegerRegister ||
      TRI.IntReturnRegs.size() < 2 || Med.DoesNotReturn || Med.IsVariadic ||
      Scalar.ReturnLocation.RegisterOffset != TRI.IntReturnRegs[0] ||
      Scalar.ReturnLocation.ValueBytes != 8 || !Med.MultiReturn.empty() ||
      Med.FPReturnViaX87)
    return std::nullopt;
  size_t Remaining = 262144;
  for (const auto &Block : Med.Blocks) {
    if (!Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty())
      return std::nullopt;
    for (const auto &Op : Block.Ops) {
      if (!Remaining-- || Op.Opcode == NdOp::INTRINSIC)
        return std::nullopt;
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
        if (!Op.SourceCallHint ||
            !validateSourceABI(Op.SourceCallHint->Signature, Error) ||
            Op.NumInputs !=
                sourceABIParameters(Op.SourceCallHint->Signature).size() + 1)
          return std::nullopt;
    }
  }
  SourceFunctionTypeHint Pair = Scalar;
  Pair.ReturnType =
      NdType::makeStruct({Scalar.ReturnType, NdType::makeInt(8, false)});
  Pair.ReturnLocation = {};
  for (unsigned I = 0; I < 2; ++I) {
    SourceABIValueLocation Location{SourceABICarrierKind::IntegerRegister,
                                    TRI.IntReturnRegs[I], 0, 8};
    std::optional<MedVar> Incoming;
    for (const auto &Parameter : Med.Params)
      if (Parameter.Kind == MedVar::Param && Parameter.Id >= 0 &&
          Parameter.RegOff == Location.RegisterOffset && Parameter.Size == 8)
        Incoming = Parameter;
    if (!definedReturnPaths(Med, Architecture, Location, Incoming))
      return std::nullopt;
    Pair.ReturnComponents.push_back(Location);
  }
  return validateSourceABI(Pair, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Pair))
             : std::nullopt;
}

struct ExactNativeContract {
  bool Recognized = false;
  std::optional<SourceFunctionTypeHint> Signature;
};

// Generic return-type inference may choose int32 after a W0/EAX write even
// though the lifted machine computation also defines the upper register bits.
// Internal source calls must preserve that complete value when callers read it.
// Limit this refinement to leaf bodies: an imported narrow result does not
// authenticate its caller-saved upper bits.
bool completeIntegerLeafReturn(const MedFunc &Med, const HighFunc &High,
                               Arch Architecture,
                               SourceABIValueLocation Location) {
  if (!Med.ReturnType || Med.ReturnType->Kind != NdTypeKind::Int ||
      Med.ReturnType->Size >= 8)
    return false;
  for (const auto &Block : Med.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
          Op.Opcode == NdOp::INTRINSIC)
        return false;
  Location.ValueBytes = 8;
  if (!definedReturnPaths(Med, Architecture, Location, std::nullopt))
    return false;
  const auto Flow = buildHighSourceFlowGraph(High);
  if (!Flow.Diagnostics.Complete || !Flow.Diagnostics.Items.empty())
    return false;
  bool HasReturn = false;
  std::vector<const HighExpr *> Pending;
  for (const auto &Node : Flow.Nodes) {
    if (!Node.Statement)
      continue;
    const auto &S = *Node.Statement;
    if (S.Kind == StmtKind::Return) {
      HasReturn = true;
      if (!S.RetVal || !S.RetVal->Type ||
          S.RetVal->Type->Kind != NdTypeKind::Int || S.RetVal->Type->Size != 8)
        return false;
    }
    forEachExpr(S, [&](const ExprPtr &E) { Pending.push_back(E.get()); });
  }
  std::set<const HighExpr *> Seen;
  size_t Remaining = 262144;
  while (!Pending.empty()) {
    const auto *E = Pending.back();
    Pending.pop_back();
    if (!E || !Remaining-- || E->Kind == ExprKind::Undef)
      return false;
    if (!Seen.insert(E).second)
      continue;
    for (const auto &Operand : E->Operands)
      Pending.push_back(Operand.get());
  }
  return HasReturn;
}

ExactNativeContract
compilerRTPlatformVersionContract(const BinaryImage &Image, const MedFunc &Med,
                                  const SourceFunctionTypeHint &Observed,
                                  std::string &Diagnostic) {
  constexpr llvm::StringLiteral SymbolName = "___isPlatformVersionAtLeast";
  size_t MatchingSymbols = 0;
  bool EntryMatches = false;
  bool SymbolValid = true;
  for (const auto &Symbol : Image.Symbols) {
    if (Symbol.Name != SymbolName)
      continue;
    ++MatchingSymbols;
    if (Symbol.Addr == Med.Entry) {
      EntryMatches = true;
      SymbolValid &= Symbol.IsFunc;
    }
  }
  if (!EntryMatches)
    return {};
  ExactNativeContract Result;
  Result.Recognized = true;
  auto Reject = [&](const char *Reason) {
    Diagnostic = Reason;
    return Result;
  };
  if (MatchingSymbols != 1 || !SymbolValid)
    return Reject("compiler-rt platform helper has ambiguous symbol identity");

  SourceFunctionTypeHint ExpectedAvailability;
  ExpectedAvailability.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
  ExpectedAvailability.ReturnType = NdType::makeInt(1, false);
  ExpectedAvailability.Parameters = {
      {"count", NdType::makeInt(4, false)},
      {"versions", NdType::makePtr(NdType::makeVoid())},
  };
  std::string ABIError;
  if (!assignDarwinFixedSourceABI(ExpectedAvailability, Image.Arch, ABIError))
    return Reject("compiler-rt platform helper has no supported Darwin ABI");

  size_t AvailabilityCalls = 0;
  size_t Remaining = 262144;
  for (const auto &Block : Med.Blocks)
    for (const auto &Operation : Block.Ops) {
      if (!Remaining--)
        return Reject("compiler-rt platform helper body exceeds proof budget");
      if ((Operation.Opcode != NdOp::CALL &&
           Operation.Opcode != NdOp::INDIR_CALL) ||
          !Operation.SourceCallHint)
        continue;
      const auto &Call = *Operation.SourceCallHint;
      if (Call.TargetName != "_availability_version_check")
        continue;
      if (Call.CallKind != SourceCallTypeHint::Kind::DarwinRuntimeCall ||
          !Call.WeakImport || !Call.TargetAddress || Call.DoesNotReturn ||
          !equalSourceABIs(Call.Signature, ExpectedAvailability))
        return Reject(
            "compiler-rt platform helper has an invalid availability probe");
      ++AvailabilityCalls;
    }
  if (AvailabilityCalls != 1)
    return Reject(
        "compiler-rt platform helper requires one exact availability probe");

  SourceFunctionTypeHint Exact;
  Exact.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Exact.ReturnType = NdType::makeInt(4, true);
  Exact.Parameters = {
      {"platform", NdType::makeInt(4, false)},
      {"major", NdType::makeInt(4, false)},
      {"minor", NdType::makeInt(4, false)},
      {"subminor", NdType::makeInt(4, false)},
  };
  if (!assignDarwinFixedSourceABI(Exact, Image.Arch, ABIError))
    return Reject("compiler-rt platform helper contract is unsupported");
  if (Observed.Parameters.size() != Exact.Parameters.size() ||
      !Observed.ReturnType || Observed.ReturnType->Kind != NdTypeKind::Int ||
      (Observed.ReturnType->Size != 4 && Observed.ReturnType->Size != 8) ||
      Observed.ReturnLocation.Kind != Exact.ReturnLocation.Kind ||
      Observed.ReturnLocation.RegisterOffset !=
          Exact.ReturnLocation.RegisterOffset)
    return Reject(
        "compiler-rt platform helper machine carriers disagree with contract");
  for (size_t I = 0; I < Exact.Parameters.size(); ++I) {
    const auto &Actual = Observed.Parameters[I];
    const auto &Expected = Exact.Parameters[I];
    if (!Actual.Type || Actual.Type->Kind != NdTypeKind::Int ||
        Actual.Type->Size != 4 ||
        Actual.Location.Kind != Expected.Location.Kind ||
        Actual.Location.RegisterOffset != Expected.Location.RegisterOffset ||
        Actual.Location.ValueBytes != Expected.Location.ValueBytes)
      return Reject(
          "compiler-rt platform helper parameters disagree with contract");
  }
  Result.Signature = std::move(Exact);
  return Result;
}
} // namespace

std::set<va_t> observedNativeIntegerPairReturns(const LowFunc &Function,
                                                Arch Architecture) {
  if ((Architecture != Arch::AArch64 && Architecture != Arch::X64) ||
      Function.Blocks.size() > 16384)
    return {};
  const auto &TRI = getTargetRegInfo(Architecture);
  if (TRI.IntReturnRegs.size() < 2)
    return {};
  const auto Register = TRI.IntReturnRegs[1];
  std::set<va_t> Targets;
  size_t Remaining = 262144;
  for (const auto &Block : Function.Blocks) {
    std::optional<va_t> Pending;
    for (const auto &Op : Block.Ops) {
      if (!Remaining-- || Op.NumInputs > 6)
        return {};
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
          Op.Opcode == NdOp::INTRINSIC || Op.Opcode == NdOp::INDIR_BR ||
          Op.Opcode == NdOp::RETURN) {
        Pending.reset();
        if (Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
            Op.Inputs[0].isConst())
          Pending = Op.Inputs[0].Offset;
        continue;
      }
      if (!Pending)
        continue;
      const bool SelfZero =
          (Op.Opcode == NdOp::INT_XOR || Op.Opcode == NdOp::INT_SUB) &&
          Op.NumInputs == 2 && Op.Inputs[0] == Op.Inputs[1];
      if (!SelfZero)
        for (unsigned I = 0; I < Op.NumInputs; ++I)
          if (Op.Inputs[I].isReg() && Op.Inputs[I].Offset == Register &&
              Op.Inputs[I].Size == 8)
            Targets.insert(*Pending);
      if (Op.Output.isReg() && Op.Output.Size &&
          (Op.Output.Offset <= Register
               ? Register - Op.Output.Offset < Op.Output.Size
               : Op.Output.Offset - Register < 8))
        Pending.reset();
    }
  }
  return Targets;
}

std::optional<SourceFunctionTypeHint> inferNativeSourceTypeHint(
    const BinaryImage &Image, const MedFunc &Med, const HighFunc &High,
    const PipelineFunctionAudit &Audit, std::string &Diagnostic,
    const LowFunc *Low, bool ObserveIntegerPair) {
  Diagnostic.clear();
  auto Reject =
      [&](const char *Reason) -> std::optional<SourceFunctionTypeHint> {
    Diagnostic = Reason;
    return std::nullopt;
  };
  if (Image.Format != BinaryFormat::MachO || Image.Bits != Bitness::Bits64 ||
      Image.IsRelocatable ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) ||
      !Image.isCodeAddress(Med.Entry) || Med.Entry != High.Entry)
    return Reject(
        "native source inference requires one linked Darwin function");
  if (!completeNativeAudit(Med.Entry, Audit))
    return Reject("native source inference requires complete verified lifting");
  if (Med.SourceTypeHint || High.SourceTypeHint || Med.SourceParametersBound)
    return Reject("native function already has a source declaration");
  const bool NoReturn = Med.DoesNotReturn && High.DoesNotReturn &&
                        hasProvenNoReturnExit(Med, Image.Arch);
  if (Med.IsVariadic || !Med.MultiReturn.empty() || Med.FPReturnViaX87 ||
      ((Med.DoesNotReturn || High.DoesNotReturn) && !NoReturn))
    return Reject("native function has a non-scalar or non-returning ABI");
  if (Med.Blocks.empty() || High.Body.empty() ||
      Med.Params.size() != Med.TypedParams.size() || Med.Params.size() > 64 ||
      Med.Params.size() != High.Params.size() ||
      !sameScalar(Med.ReturnType, High.ReturnType))
    return Reject("native scalar parameter or return types are incomplete");

  const auto &TRI = getTargetRegInfo(Image.Arch);
  const auto IntegerLayout =
      TRI.integerArgumentLayout(Med.CC == CallingConv::Win64);
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Hint.Architecture = Image.Arch;
  Hint.HasExplicitABI = true;
  Hint.ReturnType = NoReturn ? NdType::makeVoid() : Med.ReturnType;
  const bool FloatingReturn = Hint.ReturnType->Kind == NdTypeKind::Float;
  Hint.ReturnLocation.Kind = NoReturn ? SourceABICarrierKind::None
                             : FloatingReturn
                                 ? SourceABICarrierKind::FloatingRegister
                                 : SourceABICarrierKind::IntegerRegister;
  Hint.ReturnLocation.RegisterOffset = NoReturn         ? 0
                                       : FloatingReturn ? TRI.FPReturnReg
                                                        : TRI.IntReturnReg;
  Hint.ReturnLocation.ValueBytes = Hint.ReturnType->Size;
  const auto EntryBytes = observedMedSourceEntryBytes(Med, Hint);
  std::set<uint64_t> ParameterRegisters;
  std::set<uint64_t> AuxiliaryRegisters;
  std::set<int> StackSlots;
  std::optional<MedVar> IncomingReturnParameter;
  for (size_t Index = 0; Index < Med.Params.size(); ++Index) {
    const auto &Parameter = Med.Params[Index];
    // Generic ABI recovery fills unused register gaps with Id=-1. Such a
    // placeholder proves neither a type nor an argument; retain only observed
    // inputs, with their original physical register positions unchanged.
    if (Parameter.Id < 0)
      continue;
    const auto &Type = Med.TypedParams[Index].Type;
    const bool Floating =
        std::find(TRI.FPParamRegs.begin(), TRI.FPParamRegs.end(),
                  Parameter.RegOff) != TRI.FPParamRegs.end();
    if (Parameter.Kind != MedVar::Param || !Type ||
        Type->Size != Parameter.Size ||
        (!scalarCarrier(Type) &&
         !(Floating && Type->Kind == NdTypeKind::Int && Type->Size == 16)))
      return Reject("native parameter lacks a scalar machine carrier");
    SourceParameterTypeHint Source;
    Source.Name = "native_arg" + std::to_string(Hint.Parameters.size());
    Source.Type = Type;
    Source.Location.ValueBytes = Type->Size;
    if (Parameter.RegOff == kNoParamReg) {
      // This is the generic stack recovery contract, not an inferred C
      // parameter index. A narrow SUBBYTES use can represent several packed
      // arm64 values in one slot, so it requires a separate range analysis.
      const int RegisterCount =
          static_cast<int>(IntegerLayout.Registers.size());
      if (Type->Kind == NdTypeKind::Float || Parameter.Size != 8 ||
          Parameter.Id < RegisterCount || Parameter.Id >= RegisterCount + 512)
        return Reject("native stack parameter has no complete slot evidence");
      Source.Location.Kind = SourceABICarrierKind::Stack;
      Source.Location.EntryStackOffset =
          IntegerLayout.EntryStackBase +
          int64_t(Parameter.Id - RegisterCount) * IntegerLayout.SlotBytes;
      if (!StackSlots.insert(Parameter.Id).second)
        return Reject("native stack parameters overlap");
    } else {
      if (!ParameterRegisters.insert(Parameter.RegOff).second)
        return Reject("native parameter register is ambiguous");
      if (Floating) {
        uint16_t Width = Parameter.Size;
        if (Width == 16) {
          if (!EntryBytes || !EntryBytes->count(Parameter.RegOff))
            return Reject("native floating parameter has no entry-byte proof");
          const auto Bytes = EntryBytes->at(Parameter.RegOff);
          if (!Bytes || (Bytes & ~uint64_t(0xFF)))
            return Reject(
                "native floating parameter observes non-scalar lanes");
          Width = Bytes & ~uint64_t(0xF) ? 8 : 4;
        }
        if ((Type->Kind != NdTypeKind::Int &&
             Type->Kind != NdTypeKind::Float) ||
            (Width != 4 && Width != 8))
          return Reject("native floating parameter lacks a scalar lane");
        // Generic IR may describe comparison or bit-copy inputs as integers.
        // The source carrier preserves those bits in the observed FP lane;
        // it does not change the generic operation's numeric interpretation.
        Source.Type = NdType::makeFloat(Width);
        Source.Location.ValueBytes = Width;
        Source.Location.Kind = SourceABICarrierKind::FloatingRegister;
      } else {
        if (Type->Kind == NdTypeKind::Float)
          return Reject(
              "native floating parameter has no FP register evidence");
        if (IntegerLayout.registerIndex(Parameter.RegOff) < 0) {
          if (Parameter.Size != 8)
            return Reject(
                "native auxiliary parameter requires a complete word");
          AuxiliaryRegisters.insert(Parameter.RegOff);
        }
        Source.Location.Kind = SourceABICarrierKind::IntegerRegister;
      }
      Source.Location.RegisterOffset = Parameter.RegOff;
      if (Parameter.RegOff == Hint.ReturnLocation.RegisterOffset &&
          Parameter.Size >= Hint.ReturnType->Size)
        IncomingReturnParameter = Parameter;
    }
    Hint.Parameters.push_back(std::move(Source));
  }
  if (!Med.MutableStackParamHomes.empty()) {
    // Mutable homes are stored by parameter index, whereas ordinary stack
    // Params use ABI slot IDs. Do not combine these coordinate systems.
    return Reject("native mutable stack parameters require range recovery");
  }

  bool HasReturn = false;
  for (const auto &Block : Med.Blocks) {
    if (!Block.ExceptionalSuccs.empty() || !Block.ExceptionalPreds.empty())
      return Reject("native exception-dependent parameters are unsupported");
    for (const auto &Op : Block.Ops) {
      // Generic lifting may attach a placeholder register output to a trap.
      // It is never a result carrier: the shared termination proof cuts the
      // path and HighIR lowers the intrinsic as a terminal statement.
      if (Op.Opcode == NdOp::INTRINSIC &&
          !(NoReturn && isArchitecturalNoReturn(Op, Image.Arch)))
        return Reject("native intrinsic requires explicit scalar ABI evidence");
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        std::string Error;
        if (!Op.SourceCallHint ||
            !validateSourceABI(Op.SourceCallHint->Signature, Error) ||
            (NoReturn &&
             Op.DoesNotReturn != Op.SourceCallHint->DoesNotReturn) ||
            Op.NumInputs !=
                sourceABIParameters(Op.SourceCallHint->Signature).size() + 1)
          return Reject(
              "native function calls a target without a source binding");
      }
      for (uint8_t I = 0; I < Op.NumInputs; ++I) {
        const auto &Input = Op.Inputs[I];
        if (Input.Kind != MedVar::Param)
          continue;
        if (Input.RegOff != kNoParamReg) {
          if (!ParameterRegisters.count(Input.RegOff))
            return Reject("native body uses an unbound input register");
          continue;
        }
        if (!StackSlots.count(Input.Id) || Input.Size != 8 ||
            (Op.Opcode == NdOp::SUBBYTES &&
             (I != 0 || Op.NumInputs != 2 || !Op.Inputs[1].isConst() ||
              Op.Inputs[1].ConstVal != 0 || Op.Output.Size != 8)))
          return Reject("native stack slot is partial or has an unknown range");
      }
      HasReturn |= Op.Opcode == NdOp::RETURN;
    }
    for (const auto &Phi : Block.Phis)
      for (const auto &[Predecessor, Input] : Phi.Args)
        if (Input.Kind == MedVar::Param && Input.RegOff == kNoParamReg)
          return Reject("native stack parameter PHI requires range recovery");
  }
  if (!HasReturn && !NoReturn)
    return Reject("native function has no machine return");
  if (!NoReturn &&
      completeIntegerLeafReturn(Med, High, Image.Arch, Hint.ReturnLocation)) {
    Hint.ReturnType = NdType::makeInt(8, false);
    Hint.ReturnLocation.ValueBytes = 8;
  }
  if (!NoReturn && !definedReturnPaths(Med, Image.Arch, Hint.ReturnLocation,
                                       IncomingReturnParameter)) {
    if (!hasNativeSourceStateContract(Image, Low, Med, true) ||
        !definedReturnPaths(Med, Image.Arch, {}, std::nullopt))
      return Reject("native result has no complete defined carrier on every "
                    "return path");
    Hint.ReturnType = NdType::makeVoid();
    Hint.ReturnLocation = {};
  }
  const auto PointerParameters = inferMedSourcePointerParameters(Med);
  size_t SourceIndex = 0;
  for (size_t Index = 0; Index < Med.Params.size(); ++Index) {
    if (Med.Params[Index].Id < 0)
      continue;
    auto &Parameter = Hint.Parameters[SourceIndex++];
    if (PointerParameters[Index] && Parameter.Type->Kind == NdTypeKind::Int)
      Parameter.Type = NdType::makePtr(NdType::makeVoid());
  }
  if (!validateSourceABI(Hint, Diagnostic))
    return std::nullopt;
  const auto EntryRegisters = nativeEntryRegisters(Image, Low, Med, Hint);
  for (uint64_t Register : AuxiliaryRegisters)
    if (std::find(EntryRegisters.begin(), EntryRegisters.end(), Register) ==
        EntryRegisters.end())
      return Reject(
          "native auxiliary parameter lacks observed native input evidence");
  for (uint64_t Register : EntryRegisters)
    if (!ParameterRegisters.count(Register))
      Hint.Parameters.push_back(
          {"native_arg" + std::to_string(Hint.Parameters.size()),
           NdType::makeInt(8),
           {SourceABICarrierKind::IntegerRegister, Register, 0, 8}});
  if (!validateSourceABI(Hint, Diagnostic))
    return std::nullopt;
  auto Exact = compilerRTPlatformVersionContract(Image, Med, Hint, Diagnostic);
  if (Exact.Recognized)
    return Exact.Signature;
  if (ObserveIntegerPair)
    if (auto Pair = integerPairReturn(Med, Hint))
      return Pair;
  return Hint;
}

std::optional<SourceFunctionTypeHint>
refineNativeIntegerPairReturnHint(const MedFunc &Med, const HighFunc &High,
                                  const PipelineFunctionAudit &Audit) {
  if (Med.Entry != High.Entry || !completeNativeAudit(Med.Entry, Audit) ||
      !Med.SourceParametersBound || !Med.SourceTypeHint ||
      !High.SourceTypeHint || High.DoesNotReturn || High.Body.empty() ||
      !equalSourceABIs(*Med.SourceTypeHint, *High.SourceTypeHint) ||
      !equalSourceTypes(Med.ReturnType, Med.SourceTypeHint->ReturnType) ||
      !equalSourceTypes(High.ReturnType, Med.SourceTypeHint->ReturnType))
    return std::nullopt;
  return integerPairReturn(Med, *Med.SourceTypeHint);
}

std::optional<SourceFunctionTypeHint>
refineNativeSourceTypeHint(const HighFunc &Function,
                           const PipelineFunctionAudit &Audit) {
  if (!completeNativeAudit(Function.Entry, Audit) || !Function.SourceTypeHint ||
      Function.Body.empty() || Function.StructuredExceptionRegions ||
      Function.UnstructuredExceptionRegions)
    return std::nullopt;
  const auto &Original = *Function.SourceTypeHint;
  std::string Error;
  if (Original.Origin != SourceFunctionTypeHint::OriginKind::NativeAnalysis ||
      (Original.Architecture != Arch::AArch64 &&
       Original.Architecture != Arch::X64) ||
      !Original.HasExplicitABI || !Original.ReturnType ||
      (Original.ReturnType->Kind != NdTypeKind::Void &&
       !scalarCarrier(Original.ReturnType)) ||
      !equalSourceTypes(Original.ReturnType, Function.ReturnType) ||
      Original.Parameters.size() != Function.Params.size() ||
      Original.Parameters.size() > 64 || !validateSourceABI(Original, Error))
    return std::nullopt;
  const auto &TRI = getTargetRegInfo(Original.Architecture);
  std::set<size_t> Unused;
  for (size_t I = 0; I < Original.Parameters.size(); ++I) {
    const auto &Parameter = Original.Parameters[I];
    if (!equalSourceTypes(Parameter.Type, Function.Params[I].Type))
      return std::nullopt;
    const auto &Location = Parameter.Location;
    if (Location.Kind == SourceABICarrierKind::IntegerRegister &&
        std::find(TRI.IntParamRegs.begin(), TRI.IntParamRegs.end(),
                  Location.RegisterOffset) == TRI.IntParamRegs.end())
      Unused.insert(I);
  }
  if (Unused.empty())
    return std::nullopt;
  bool Valid = true, HasReturn = false;
  size_t Remaining = 65536;
  std::vector<MedVar> RegisterUses;
  std::map<HighSourceLocalIdentity, std::pair<uint64_t, uint16_t>> RegisterDefs;
  auto Observe = [&](const MedVar &Value) {
    if (Value.Kind == MedVar::Param) {
      if (Value.Id < 0 || size_t(Value.Id) >= Function.Params.size())
        Valid = false;
      else
        Unused.erase(Value.Id);
    } else if (Value.Kind == MedVar::Reg) {
      RegisterUses.push_back(Value);
    }
  };
  const auto Scan = [&](auto &&Self, const ExprPtr &Expression,
                        unsigned Depth) -> void {
    if (!Valid)
      return;
    if (!Expression || !Remaining || Depth > 128 ||
        Expression->Kind == ExprKind::Undef) {
      Valid = false;
      return;
    }
    --Remaining;
    if (Expression->Kind == ExprKind::Var || Expression->Kind == ExprKind::Phi)
      Observe(Expression->Var);
    if (Expression->IntrinsicOutputs.size() > Remaining ||
        Expression->Operands.size() > Remaining) {
      Valid = false;
      return;
    }
    Remaining -= Expression->IntrinsicOutputs.size();
    for (const auto &Output : Expression->IntrinsicOutputs) {
      Observe(Output);
      if (!Valid)
        return;
    }
    for (const auto &Operand : Expression->Operands) {
      Self(Self, Operand, Depth + 1);
      if (!Valid)
        return;
    }
  };
  std::vector<const HighStmt *> Pending;
  if (Function.Body.size() > Remaining)
    return std::nullopt;
  for (const auto &Statement : Function.Body)
    Pending.push_back(&Statement);
  while (Valid && !Pending.empty()) {
    if (!Remaining--)
      return std::nullopt;
    const auto &Statement = *Pending.back();
    Pending.pop_back();
    if (Statement.Kind == StmtKind::Assign && Statement.Dst &&
        Statement.Dst->Kind == ExprKind::Var &&
        Statement.Dst->Var.Kind == MedVar::Reg) {
      const auto &V = Statement.Dst->Var;
      const auto [It, Added] = RegisterDefs.emplace(
          highSourceLocalIdentity(V), std::pair{V.RegOff, V.Size});
      if (!Added)
        It->second.second = It->second.first == V.RegOff
                                ? std::min(It->second.second, V.Size)
                                : 0;
    }
    if (Statement.Kind == StmtKind::Return) {
      HasReturn = true;
      if (Original.ReturnType->Kind != NdTypeKind::Void &&
          (!Statement.RetVal || !Statement.RetVal->Type ||
           Statement.RetVal->Type->Size != Original.ReturnType->Size))
        Valid = false;
    }
    forEachExpr(Statement,
                [&](const ExprPtr &Expression) { Scan(Scan, Expression, 0); });
    auto Add = [&](const std::vector<HighStmt> &Body) {
      if (Body.size() > Remaining || Pending.size() > Remaining - Body.size()) {
        Valid = false;
        return;
      }
      for (const auto &Child : Body)
        Pending.push_back(&Child);
    };
    Add(Statement.Body);
    Add(Statement.ElseBody);
    Add(Statement.DefaultBody);
    if (Statement.Cases.size() > Remaining ||
        Statement.EHClauseBodies.size() > Remaining - Statement.Cases.size()) {
      Valid = false;
      continue;
    }
    Remaining -= Statement.Cases.size() + Statement.EHClauseBodies.size();
    for (const auto &Case : Statement.Cases)
      Add(Case.Body);
    for (const auto &Body : Statement.EHClauseBodies)
      Add(Body);
  }
  if (!Valid || !HasReturn || Unused.empty())
    return std::nullopt;
  std::optional<bool> DefinedLocals;
  for (const auto &Value : RegisterUses) {
    const auto Definition = RegisterDefs.find(highSourceLocalIdentity(Value));
    if (Definition != RegisterDefs.end() &&
        Definition->second.first == Value.RegOff &&
        Definition->second.second >= Value.Size) {
      if (!DefinedLocals) {
        const auto Flow = analyzeHighSourceFlow(
            Function, Original.ReturnType->Kind != NdTypeKind::Void);
        DefinedLocals = Flow.Complete && Flow.Items.empty();
      }
      // Renaming retains the original physical register on a local. A value
      // defined on every reaching path is not an incoming register parameter.
      if (*DefinedLocals)
        continue;
    }
    std::erase_if(Unused, [&](size_t I) {
      const auto &Location = Original.Parameters[I].Location;
      return Value.RegOff <= Location.RegisterOffset
                 ? Location.RegisterOffset - Value.RegOff < Value.Size
                 : Value.RegOff - Location.RegisterOffset < Location.ValueBytes;
    });
  }
  if (Unused.empty())
    return std::nullopt;
  auto Refined = Original;
  for (auto I = Unused.rbegin(); I != Unused.rend(); ++I)
    Refined.Parameters.erase(Refined.Parameters.begin() + *I);
  return validateSourceABI(Refined, Error) ? std::optional(std::move(Refined))
                                           : std::nullopt;
}
} // namespace neverd

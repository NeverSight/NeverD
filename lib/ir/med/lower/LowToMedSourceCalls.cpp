#include "../../../loader/Swift/SwiftBooleanProjection.h"
#include "../../../loader/Swift/SwiftBooleanSourceBinding.h"
#include "../../../loader/Swift/SwiftErrorRuntime.h"
#include "../../../loader/Swift/SwiftErrorSourceProjection.h"
#include "../../../loader/Swift/SwiftMangledValueConstructorABI.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/SourceCallABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/ir/low/SourceCallOccurrence.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/CFunctionParameterCalls.h"
#include "neverd/loader/MachO/ImmutableNativeCalls.h"
#include "neverd/loader/ObjC/ObjCBlockCallHints.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"
#include "neverd/loader/Swift/SwiftConsumedInputEffects.h"
#include "neverd/loader/Swift/SwiftOpaqueValueEffects.h"
#include "neverd/loader/Swift/SwiftValueWitnessCalls.h"
#include "neverd/loader/Swift/SwiftVirtualCalls.h"

#include <algorithm>
#include <optional>
#include <set>

namespace neverd {

void LowToMedConverter::bindSourceCalls(MedFunc &Func, const LowFunc &Low,
                                        BinaryFormat Fmt) {
  // Image-independent IR clients supply explicit source contracts. Their
  // physical carriers do not depend on a container format. Runtime discovery
  // from a loaded image remains restricted to its supported Mach-O path.
  const bool ExplicitSites =
      SourceCallSiteHints && !SourceCallSiteHints->empty();
  const bool RuntimeHints =
      SourceCallRuntimeHintsEnabled && Image && Fmt == BinaryFormat::MachO;
  // Standalone clients historically supplied one shared map. The pipeline
  // keeps entry declarations separate from call-only thunk declarations.
  const auto *EntryHints =
      SourceEntryTypeHints ? SourceEntryTypeHints : SourceCalleeTypeHints;
  const bool ExplicitEntry = EntryHints && EntryHints->count(Low.Entry);
  if (!SourceCallHintsEnabled ||
      (Image && !RuntimeHints && !ExplicitSites && !ExplicitEntry) ||
      (Image && (Image->IsRelocatable || Image->Arch != TargetArch)) ||
      (TargetArch != Arch::AArch64 && TargetArch != Arch::X64))
    return;
  const std::map<unsigned, ObjCReceiverTypeHint> *BlockParameters = nullptr;
  if (ObjCBlockParameterReceivers)
    if (auto It = ObjCBlockParameterReceivers->find(Low.Entry);
        It != ObjCBlockParameterReceivers->end())
      BlockParameters = &It->second;
  const std::map<uint64_t, ObjCReceiverTypeHint> *BlockCaptures = nullptr;
  if (ObjCBlockCaptureReceivers)
    if (auto It = ObjCBlockCaptureReceivers->find(Low.Entry);
        It != ObjCBlockCaptureReceivers->end())
      BlockCaptures = &It->second;
  auto Hints =
      RuntimeHints
          ? buildObjCSourceCallHints(*Image, Low, BlockParameters,
                                     BlockCaptures, SourceCalleeTypeHints)
          : std::map<va_t, SourceCallTypeHint>();
  if (RuntimeHints) {
    auto SwiftHints = buildSwiftValueWitnessCallHints(*Image, Low);
    for (auto &[Address, Hint] : SwiftHints) {
      auto [It, Inserted] = Hints.emplace(Address, std::move(Hint));
      if (!Inserted)
        Hints.erase(It);
    }
    auto VirtualHints = buildSwiftVirtualCallHints(*Image, Low);
    for (auto &[Address, Hint] : VirtualHints) {
      auto [It, Inserted] = Hints.emplace(Address, std::move(Hint));
      if (!Inserted)
        Hints.erase(It);
    }
  }
  if (RuntimeHints) {
    for (auto &[Address, Hint] :
         buildSwiftConsumedInputCallHints(*Image, Low)) {
      const auto Existing = Hints.find(Address);
      if (Existing != Hints.end() &&
          Existing->second.CallKind == Hint.CallKind &&
          Existing->second.TargetAddress == Hint.TargetAddress &&
          Existing->second.TargetName == Hint.TargetName &&
          equalSourceABIs(Existing->second.Signature, Hint.Signature))
        Existing->second.SwiftConsumedInput = Hint.SwiftConsumedInput;
    }
  }
  const SourceFunctionTypeHint *EntrySignature = nullptr;
  if (EntryHints)
    if (auto It = EntryHints->find(Low.Entry); It != EntryHints->end()) {
      std::string Diagnostic;
      if (It->second.Architecture == TargetArch &&
          validateSourceABI(It->second, Diagnostic))
        EntrySignature = &It->second;
    }
  if (RuntimeHints) {
    const ObjCBlockCaptureCallFields *Captures = nullptr;
    if (ObjCBlockCaptureFields)
      if (auto It = ObjCBlockCaptureFields->find(Low.Entry);
          It != ObjCBlockCaptureFields->end())
        Captures = &It->second;
    auto BlockHints =
        buildObjCBlockCallHints(*Image, Low, EntrySignature, &Hints, Captures);
    Hints.insert(BlockHints.begin(), BlockHints.end());
  }
  if (RuntimeHints && EntrySignature) {
    auto FunctionPointers = buildCFunctionParameterCallHints(
        *Image, Low, *EntrySignature, SourceCalleeTypeHints);
    for (auto &[Address, Hint] : FunctionPointers) {
      const auto [It, Inserted] = Hints.emplace(Address, std::move(Hint));
      if (!Inserted)
        Hints.erase(It);
    }
  }
  if (RuntimeHints && SourceCalleeTypeHints) {
    auto NativeHints =
        buildImmutableNativeCallHints(*Image, Low, *SourceCalleeTypeHints);
    for (auto &[Address, Hint] : NativeHints) {
      const auto [It, Inserted] = Hints.emplace(Address, std::move(Hint));
      if (!Inserted)
        Hints.erase(It);
    }
  }
  std::optional<SourceFunctionTypeHint> ProvisionalBooleanEntry;
  if (RuntimeHints && !EntrySignature) {
    bool HasBooleanCandidate = false;
    for (const auto &Block : Low.Blocks) {
      for (const auto &Op : Block.Ops) {
        if (Op.Opcode != NdOp::CALL)
          continue;
        const auto Site = sourceCallOccurrenceKey(Op);
        if (Site && Site->StaticTarget &&
            swiftBooleanRuntimeVeneerCandidate(*Image, *Site->StaticTarget)) {
          HasBooleanCandidate = true;
          break;
        }
      }
      if (HasBooleanCandidate)
        break;
    }
    if (HasBooleanCandidate)
      ProvisionalBooleanEntry =
          provisionalNativeSwiftBooleanEntry(*Image, Low.Entry);
  }
  if (RuntimeHints && (EntrySignature || ProvisionalBooleanEntry)) {
    const auto Booleans = qualifySwiftBooleanProjections(
        *Image, Low,
        EntrySignature ? *EntrySignature : *ProvisionalBooleanEntry,
        SourceCalleeTypeHints);
    for (const auto &Boolean : Booleans) {
      const auto Signature =
          swiftBooleanNormalizedSignature(Boolean.Runtime.ImportName);
      if (!Signature)
        continue;
      SourceCallTypeHint Hint;
      Hint.CallKind = SourceCallTypeHint::Kind::SwiftBooleanProjection;
      Hint.BooleanResult = SourceCallTypeHint::BooleanResultProjection{
          Low.Entry, Boolean.Normalization.Site};
      Hint.Signature = *Signature;
      Hint.TargetAddress = Boolean.Runtime.ImportSlot;
      Hint.TargetName =
          llvm::StringRef(Boolean.Runtime.ImportName).drop_front().str();
      Hints.emplace(Boolean.Normalization.Site.Instruction, std::move(Hint));
    }
  }
  const auto OpaqueHints =
      RuntimeHints && SourceCalleeFunctions && SourceCalleeTypeHints
          ? buildSwiftOpaqueValueCallHints(*Image, Low, *SourceCalleeTypeHints,
                                           *SourceCalleeFunctions)
          : std::map<va_t, SourceCallTypeHint>{};
  const auto ConstructorHints =
      RuntimeHints && SourceCalleeFunctions && SourceCalleeTypeHints
          ? buildSwiftFixedRecordConstructorCallHints(
                *Image, Low, *SourceCalleeTypeHints, *SourceCalleeFunctions)
          : std::map<va_t, SourceCallTypeHint>{};
  const auto &TRI = getTargetRegInfo(TargetArch);
  auto Temporary = [&](uint16_t Size) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.Id = allocVarId();
    V.Size = Size;
    V.TheArch = TargetArch;
    return V;
  };
  std::set<SourceCallOccurrenceKey> UnboundCalls;
  std::optional<std::map<SourceCallOccurrenceKey, va_t>> ConstantCalls;
  for (const auto &Block : Low.Blocks)
    for (const auto &Op : Block.Ops)
      if (const auto Site = sourceCallOccurrenceKey(Op))
        UnboundCalls.insert(*Site);
  for (auto &Block : Func.Blocks) {
    std::vector<MedOp> Ops;
    for (auto Op : Block.Ops) {
      if (Op.Opcode == NdOp::RETURN && EntrySignature &&
          EntrySignature->ReturnType->Kind == NdTypeKind::Void) {
        // RET reads the architecture's conventional result register in the
        // generic LowIR model.  An authenticated source signature returning
        // void makes that machine value unobservable; keeping it would retain
        // an undefined caller-saved carrier and reject an otherwise complete
        // source projection.
        Op.NumInputs = 0;
      } else if (Op.Opcode == NdOp::RETURN && EntrySignature &&
                 EntrySignature->ReturnType->Kind == NdTypeKind::Struct &&
                 EntrySignature->ReturnLocation.Kind !=
                     SourceABICarrierKind::IndirectResultPointer) {
        Op.NumInputs = 0;
        for (const auto &Piece : EntrySignature->ReturnComponents) {
          auto Value =
              ndVarToMedVar(NdVar::reg(Piece.RegisterOffset, Piece.ValueBytes));
          if (Piece.Kind == SourceABICarrierKind::BooleanRegister) {
            MedOp Bit;
            Bit.Opcode = NdOp::INT_AND;
            Bit.Addr = Op.Addr;
            Bit.Output = Temporary(1);
            Bit.addInput(Value);
            Bit.addInput(MedVar::makeConst(1, 1));
            Value = Bit.Output;
            Ops.push_back(std::move(Bit));
          }
          Op.addInput(Value);
        }
      } else if (Op.Opcode == NdOp::RETURN && EntrySignature &&
                 !EntrySignature->ReturnComponents.empty()) {
        // Publish both physical dependencies before SSA. They are separate
        // registers, not one contiguous register-bank slice (notably RAX/RDX).
        MedOp Result;
        Result.Opcode = NdOp::CONCAT;
        Result.Addr = Op.Addr;
        Result.Output = Temporary(EntrySignature->ReturnType->Size);
        for (const auto I : {1U, 0U}) {
          const auto &Piece = EntrySignature->ReturnComponents[I];
          Result.addInput(ndVarToMedVar(
              NdVar::reg(Piece.RegisterOffset, Piece.ValueBytes)));
        }
        Op.NumInputs = 0;
        Op.addInput(Result.Output);
        Ops.push_back(std::move(Result));
      } else if (Op.Opcode == NdOp::RETURN && EntrySignature &&
                 EntrySignature->ReturnLocation.Kind ==
                     SourceABICarrierKind::IntegerRegister) {
        // RET itself does not read X0/RAX. Publish the declared source lane
        // before SSA so PHIs carry its low bytes without requiring unknown
        // upper bits from a narrow call result.
        const auto &Return = EntrySignature->ReturnLocation;
        Op.NumInputs = 0;
        Op.addInput(ndVarToMedVar(
            NdVar::reg(Return.RegisterOffset, Return.ValueBytes)));
      }
      if (Op.Opcode == NdOp::RETURN && EntrySignature)
        if (const auto Error = sourceABIErrorResult(*EntrySignature)) {
          // This normally preserved register is also an observable Swift
          // output. Publish it before SSA, separately from the ordinary result.
          Op.addInput(ndVarToMedVar(NdVar::reg(Error->Location.RegisterOffset,
                                               Error->Location.ValueBytes)));
          Op.HasSourceErrorResult = true;
        }
      // Generic callee summaries can already have published integer/vector
      // register uses (notably SysV x64). A validated source ABI owns their
      // order and lanes. Only replace that complete summary shape; explicit
      // LowIR arguments and exact formatted calls retain their own contract.
      // Prototype argument counts can annotate explicit operands too: the
      // original single-target call occurrence must authorize reconstruction.
      const bool SummaryArguments =
          !Op.ExactArguments && Op.CalleeRegisterArgs >= 0 &&
          Op.NumInputs == 1u + Op.CalleeRegisterArgs +
                              std::max<int>(0, Op.CalleeVectorArgs) &&
          UnboundCalls.count({Op.Addr, Op.OriginSeq, Op.Opcode,
                              Op.Inputs[0].isConst()
                                  ? std::optional<va_t>(Op.Inputs[0].ConstVal)
                                  : std::nullopt});
      const SourceCallOccurrenceKey Site{
          Op.Addr, Op.OriginSeq, Op.Opcode,
          Op.NumInputs && Op.Inputs[0].isConst()
              ? std::optional<va_t>(Op.Inputs[0].ConstVal)
              : std::nullopt};
      const auto SiteHint =
          SourceCallSiteHints ? SourceCallSiteHints->find(Site)
                              : std::map<SourceCallOccurrenceKey,
                                         SourceCallTypeHint>::const_iterator{};
      const bool ExplicitSite =
          ExplicitSites && SiteHint != SourceCallSiteHints->end();
      if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
          (Op.NumInputs != 1 && !SummaryArguments && !ExplicitSite)) {
        Ops.push_back(std::move(Op));
        continue;
      }
      std::optional<SourceCallTypeHint> Hint;
      if (ExplicitSite)
        Hint = SiteHint->second;
      else if (auto It = Hints.find(Op.Addr); It != Hints.end())
        Hint = It->second;
      else if ((RuntimeHints || !Image) && SourceCalleeTypeHints &&
               Op.Opcode == NdOp::CALL && Op.Inputs[0].isConst()) {
        auto It = SourceCalleeTypeHints->find(Op.Inputs[0].ConstVal);
        if (It != SourceCalleeTypeHints->end()) {
          Hint.emplace();
          Hint->Signature = It->second;
          Hint->TargetAddress = It->first;
          // The real native name is resolved by the ordinary call ABI pass.
        }
      }
      // The user's supported prototype already supplied the ordinary call's
      // arguments. An automatic SDK binding must not replace them with the
      // platform declaration. Match both import slots and direct-call stubs.
      if (Hint && !ExplicitSite &&
          Hint->CallKind == SourceCallTypeHint::Kind::DarwinRuntimeCall &&
          UserPrototypeCallees &&
          (UserPrototypeCallees->count(Hint->TargetAddress) ||
           (Op.Inputs[0].isConst() &&
            UserPrototypeCallees->count(Op.Inputs[0].ConstVal))))
        Hint.reset();
      if (Hint) {
        const auto Constructor = ConstructorHints.find(Op.Addr);
        if (Constructor != ConstructorHints.end() &&
            Hint->CallKind == Constructor->second.CallKind &&
            Hint->TargetAddress == Constructor->second.TargetAddress &&
            equalSourceABIs(Hint->Signature, Constructor->second.Signature))
          Hint->SwiftValueConstructor =
              Constructor->second.SwiftValueConstructor;
        const auto Found = OpaqueHints.find(Op.Addr);
        if (Found != OpaqueHints.end() &&
            Hint->CallKind == Found->second.CallKind &&
            Hint->TargetAddress == Found->second.TargetAddress &&
            equalSourceABIs(Hint->Signature, Found->second.Signature))
          Hint->SwiftOpaqueValue = Found->second.SwiftOpaqueValue;
      }
      std::string Diagnostic;
      const bool NativeError =
          Hint && isNativeSwiftErrorSourceCall(*Hint, TargetArch) &&
          Op.Opcode == NdOp::CALL && Op.Inputs[0].isConst() &&
          Op.Inputs[0].ConstVal == Hint->TargetAddress &&
          SourceCalleeFunctions &&
          SourceCalleeFunctions->count(Hint->TargetAddress) &&
          SourceCalleeFunctions->at(Hint->TargetAddress) &&
          SourceCalleeFunctions->at(Hint->TargetAddress)->Entry ==
              Hint->TargetAddress;
      if (!Hint || Hint->Signature.Architecture != TargetArch ||
          !validateSourceABI(Hint->Signature, Diagnostic) ||
          (hasSwiftErrorResult(Hint->Signature) &&
           !isSwiftWillThrowSourceCall(*Hint, TargetArch) && !NativeError) ||
          // The ABI describes an address of a by-value copy. Until a call
          // owns a separate copy-storage proof, leave the original operation
          // unbound; a physical pointer is not its logical record argument.
          (hasIndirectSourceParameters(Hint->Signature) &&
           (!isObjCByValueCopyHint(*Hint, Low.Entry, TargetArch) ||
            Op.Opcode != NdOp::CALL || !Op.Inputs[0].isConst() ||
            Hint->ByValueCopy->Site.Instruction != Op.Addr ||
            Hint->ByValueCopy->Site.Sequence != Op.OriginSeq ||
            Hint->ByValueCopy->Site.StaticTarget != Op.Inputs[0].ConstVal))) {
        Ops.push_back(std::move(Op));
        continue;
      }
      if (Hint->ImmutableNativeCall &&
          (!isImmutableNativeCallHint(*Hint, Low.Entry, TargetArch) ||
           Op.Opcode != NdOp::INDIR_CALL || Op.Inputs[0].isConst() ||
           Hint->ImmutableNativeCall->Site.Instruction != Op.Addr ||
           Hint->ImmutableNativeCall->Site.Sequence != Op.OriginSeq)) {
        Ops.push_back(std::move(Op));
        continue;
      }
      if (Hint->BooleanResult &&
          (!isSwiftBooleanSourceBinding(*Hint) ||
           Hint->BooleanResult->FunctionEntry != Low.Entry ||
           Hint->BooleanResult->Site.Instruction != Op.Addr ||
           Hint->BooleanResult->Site.Sequence != Op.OriginSeq ||
           Hint->BooleanResult->Site.Opcode != Op.Opcode ||
           !Op.Inputs[0].isConst() ||
           Hint->BooleanResult->Site.StaticTarget != Op.Inputs[0].ConstVal)) {
        Ops.push_back(std::move(Op));
        continue;
      }
      const auto &Signature = Hint->Signature;
      const auto Parameters = sourceABIParameters(Signature);
      const bool HasStack =
          std::any_of(Parameters.begin(), Parameters.end(),
                      [](const SourceABIParameter &P) {
                        return P.Location.Kind == SourceABICarrierKind::Stack;
                      });
      std::optional<int64_t> Bias = 0;
      if (HasStack && TargetArch == Arch::X64)
        Bias = sourceCallReturnAddressBias(Image, TargetArch, Op.Addr);
      if (!Bias ||
          Parameters.size() >
              ::neverd::limits::ir::
                  kLowToMedSourceCallsLowToMedConverterBindSourceCallsParametersSizeLimit) {
        Ops.push_back(std::move(Op));
        continue;
      }
      const bool ConstantNativeCall =
          ExplicitSite && Op.Opcode == NdOp::INDIR_CALL &&
          !Op.Inputs[0].isConst() &&
          Hint->CallKind == SourceCallTypeHint::Kind::Native &&
          Signature.Origin ==
              SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      if (ConstantNativeCall) {
        if (!ConstantCalls)
          ConstantCalls = constantRegisterCallTargets(Low);
        const auto Found = ConstantCalls->find(Site);
        if (Found == ConstantCalls->end() ||
            Found->second != Hint->TargetAddress) {
          Ops.push_back(std::move(Op));
          continue;
        }
      }
      // Do this only after every binding check succeeds, so an unavailable
      // or invalid source declaration leaves the generic arguments intact.
      if (ConstantNativeCall) {
        // This source operation now names a proved callee and its complete
        // source declaration. Original LowIR and the physical occurrence key
        // above retain the register call for byte/stack validation. Never
        // turn a constant memory-call slot into its apparent code address.
        Op.Opcode = NdOp::CALL;
        Op.Inputs[0] =
            MedVar::makeConst(Hint->TargetAddress, TRI.PointerSize,
                              ConstantAddressProvenance::CodeAddress);
      }
      Op.NumInputs = 1;
      Op.IntegerCallSignature.reset();
      Op.ScalarCallSignature.reset();
      Op.CalleeRegisterArgs = -1;
      Op.CalleeVariadicFrom = -1;
      Op.CalleeVectorArgs = -1;
      Op.CalleeVectorSlots = 0;
      Op.CalleeVectorArgWidths = 0;
      Op.CalleeFloatReturnBytes = 0;
      Op.CalleeStackArgs = -1;
      for (size_t I = 0; I < Parameters.size(); ++I) {
        const auto &Location = Parameters[I].Location;
        MedVar Argument;
        if (Parameters[I].ParameterIndex == 1 &&
            Hint->SelectorReferenceAddress) {
          MedOp Load;
          Load.Opcode = NdOp::LOAD;
          Load.Addr = Op.Addr;
          Load.Output = Temporary(8);
          Load.addInput(
              MedVar::makeConst(Hint->SelectorReferenceAddress, 8,
                                ConstantAddressProvenance::DataAddress));
          Argument = Load.Output;
          Ops.push_back(std::move(Load));
        } else if (Location.Kind == SourceABICarrierKind::Stack) {
          MedOp Address;
          Address.Opcode = NdOp::INT_ADD;
          Address.Addr = Op.Addr;
          Address.Output = Temporary(8);
          Address.addInput(ndVarToMedVar(NdVar::reg(TRI.StackPointer, 8)));
          Address.addInput(
              MedVar::makeConst(Location.EntryStackOffset - *Bias, 8,
                                ConstantAddressProvenance::Scalar));
          MedOp Load;
          Load.Opcode = NdOp::LOAD;
          Load.Addr = Op.Addr;
          Load.Output = Temporary(Location.ValueBytes);
          Load.addInput(Address.Output);
          Argument = Load.Output;
          Ops.push_back(std::move(Address));
          Ops.push_back(std::move(Load));
        } else if (TargetArch == Arch::AArch64 &&
                   Location.Kind == SourceABICarrierKind::IntegerRegister &&
                   Location.ValueBytes < 4) {
          // AArch64 writes to Wn define the complete 32-bit register, even
          // when the source value carried by a call is only a byte or half.
          // Publish that architectural carrier before SSA, then extract the
          // declared source lane. Reading the narrow physical alias directly
          // would create a separate SSA web that misses converging Wn writes.
          MedOp Extract;
          Extract.Opcode = NdOp::SUBBYTES;
          Extract.Addr = Op.Addr;
          Extract.Output = Temporary(Location.ValueBytes);
          Extract.addInput(
              ndVarToMedVar(NdVar::reg(Location.RegisterOffset, 4)));
          Extract.addInput(
              MedVar::makeConst(0, 4, ConstantAddressProvenance::Scalar));
          Argument = Extract.Output;
          Ops.push_back(std::move(Extract));
        } else {
          Argument = ndVarToMedVar(
              NdVar::reg(Location.RegisterOffset, Location.ValueBytes));
        }
        Op.addInput(Argument);
      }
      const auto &Return = Signature.ReturnLocation;
      const bool IndirectResult =
          Return.Kind == SourceABICarrierKind::IndirectResultPointer;
      Op.Output = Return.Kind == SourceABICarrierKind::None || IndirectResult
                      ? MedVar()
                      : ndVarToMedVar(NdVar::reg(Return.RegisterOffset,
                                                 Return.ValueBytes));
      std::vector<MedOp> ReturnOps;
      const auto FloatingCarrier = [&](const SourceABIValueLocation &Location)
          -> std::optional<std::pair<uint64_t, uint16_t>> {
        if (Location.Kind != SourceABICarrierKind::FloatingRegister ||
            !TRI.isVectorReg(Location.RegisterOffset))
          return std::nullopt;
        auto [Offset, Bytes] =
            TRI.findWideReg(Location.RegisterOffset, Location.ValueBytes);
        if (TRI.FPABIRegWidth)
          Bytes = std::min(Bytes, TRI.FPABIRegWidth);
        if (Bytes <= Location.ValueBytes ||
            TRI.subRegByteOffset(Location.RegisterOffset, Location.ValueBytes,
                                 Offset, Bytes) != 0)
          return std::nullopt;
        return std::make_pair(Offset, Bytes);
      };
      const auto AppendFloatingCarrier =
          [&](MedVar Value, std::pair<uint64_t, uint16_t> Wide) {
            // A source result, scalar or record member, defines only its low
            // lane. Its suffix comes from this call's unknown clobber. Do not
            // publish a narrow architectural register write: SSA would zero
            // its upper lanes before this merge could retain their identity.
            MedOp Upper;
            Upper.Opcode = NdOp::SUBBYTES;
            Upper.Addr = Op.Addr;
            Upper.Output = Temporary(Wide.second - Value.Size);
            Upper.addInput(ndVarToMedVar(NdVar::reg(Wide.first, Wide.second)));
            Upper.addInput(MedVar::makeConst(Value.Size, 4));
            MedOp Merge;
            Merge.Opcode = NdOp::CONCAT;
            Merge.Addr = Op.Addr;
            Merge.Output = ndVarToMedVar(NdVar::reg(Wide.first, Wide.second));
            Merge.addInput(Upper.Output);
            Merge.addInput(Value);
            ReturnOps.push_back(std::move(Upper));
            ReturnOps.push_back(std::move(Merge));
          };
      if (NativeError) {
        // A single source operation owns both observable call results. Define
        // x21/R12 explicitly before SSA can treat its incoming value as an
        // ordinary preserved register and fold a following error test.
        Op.Output = Temporary(16);
        const auto Error = sourceABIErrorResult(Signature);
        for (const auto &[Offset, Register] :
             {std::pair<uint64_t, uint64_t>{0, Return.RegisterOffset},
              {8, Error->Location.RegisterOffset}}) {
          MedOp Extract;
          Extract.Opcode = NdOp::SUBBYTES;
          Extract.Addr = Op.Addr;
          Extract.Output = ndVarToMedVar(NdVar::reg(Register, 8));
          Extract.addInput(Op.Output);
          Extract.addInput(MedVar::makeConst(Offset, 4));
          ReturnOps.push_back(std::move(Extract));
        }
      } else if (IndirectResult) {
        // Preserve the pre-call hidden pointer before SSA introduces the
        // caller-saved x8 clobber. A declared result initializes every record
        // field; the logical call keeps the ordinary source parameter list.
        MedOp Address;
        Address.Opcode = NdOp::COPY;
        Address.Addr = Op.Addr;
        Address.Output = Temporary(8);
        Address.addInput(ndVarToMedVar(NdVar::reg(Return.RegisterOffset, 8)));
        const auto Buffer = Address.Output;
        Ops.push_back(std::move(Address));
        Op.Output = Temporary(Signature.ReturnType->Size);
        for (const auto &Member :
             sourceAggregateMembers(Signature.ReturnType)) {
          MedOp Extract;
          Extract.Opcode = NdOp::SUBBYTES;
          Extract.Addr = Op.Addr;
          Extract.Output = Temporary(Member.Type->Size);
          Extract.addInput(Op.Output);
          Extract.addInput(MedVar::makeConst(Member.ByteOffset, 4));
          MedOp Field;
          Field.Opcode = NdOp::INT_ADD;
          Field.Addr = Op.Addr;
          Field.Output = Temporary(8);
          Field.addInput(Buffer);
          Field.addInput(MedVar::makeConst(Member.ByteOffset, 8));
          MedOp Store;
          Store.Opcode = NdOp::STORE;
          Store.Addr = Op.Addr;
          Store.addInput(Field.Output);
          Store.addInput(Extract.Output);
          ReturnOps.push_back(std::move(Extract));
          ReturnOps.push_back(std::move(Field));
          ReturnOps.push_back(std::move(Store));
        }
      } else if (!Signature.ReturnComponents.empty()) {
        Op.Output = Temporary(Signature.ReturnType->Size);
        const auto Members = sourceAggregateMembers(Signature.ReturnType);
        for (size_t I = 0; I < Signature.ReturnComponents.size(); ++I) {
          const auto &Piece = Signature.ReturnComponents[I];
          const auto Wide = FloatingCarrier(Piece);
          const bool Boolean =
              Piece.Kind == SourceABICarrierKind::BooleanRegister;
          MedOp Extract;
          Extract.Opcode = NdOp::SUBBYTES;
          Extract.Addr = Op.Addr;
          Extract.Output = (Wide || Boolean)
                               ? Temporary(Piece.ValueBytes)
                               : ndVarToMedVar(NdVar::reg(Piece.RegisterOffset,
                                                          Piece.ValueBytes));
          Extract.addInput(Op.Output);
          Extract.addInput(MedVar::makeConst(
              Members.empty() ? I * Piece.ValueBytes : Members[I].ByteOffset,
              4));
          const auto Value = Extract.Output;
          ReturnOps.push_back(std::move(Extract));
          if (Boolean) {
            // The genuine _Bool field normalizes bit zero. Keep every other
            // physical bit dependent on this call's unknown clobber, including
            // bits 7:1 of the low byte. A byte-valued extract alone would
            // silently define them. Ordinary DCE can remove an unused carrier.
            MedOp Upper;
            Upper.Opcode = NdOp::INT_AND;
            Upper.Addr = Op.Addr;
            Upper.Output = Temporary(8);
            Upper.addInput(ndVarToMedVar(NdVar::reg(Piece.RegisterOffset, 8)));
            Upper.addInput(MedVar::makeConst(~uint64_t(1), 8));
            MedOp Extend;
            Extend.Opcode = NdOp::INT_ZEXT;
            Extend.Addr = Op.Addr;
            Extend.Output = Temporary(8);
            Extend.addInput(Value);
            MedOp Merge;
            Merge.Opcode = NdOp::INT_OR;
            Merge.Addr = Op.Addr;
            Merge.Output = ndVarToMedVar(NdVar::reg(Piece.RegisterOffset, 8));
            Merge.addInput(Upper.Output);
            Merge.addInput(Extend.Output);
            ReturnOps.push_back(std::move(Upper));
            ReturnOps.push_back(std::move(Extend));
            ReturnOps.push_back(std::move(Merge));
          } else if (Wide)
            AppendFloatingCarrier(Value, *Wide);
        }
      } else if (Hint->CallKind ==
                 SourceCallTypeHint::Kind::SwiftBooleanProjection) {
        // Only this exact occurrence has a complete caller proof that zeroing
        // undefined bits is unobservable. Ordinary narrow returns retain their
        // unknown upper-byte dependency below.
        Op.Output = Temporary(1);
        MedOp Extend;
        Extend.Opcode = NdOp::INT_ZEXT;
        Extend.Addr = Op.Addr;
        Extend.Output = ndVarToMedVar(NdVar::reg(Return.RegisterOffset, 8));
        Extend.addInput(Op.Output);
        ReturnOps.push_back(std::move(Extend));
      } else if (Return.Kind == SourceABICarrierKind::IntegerRegister &&
                 Return.ValueBytes < 8) {
        // A scalar ABI result does not define the rest of its register. Keep
        // those bits tied to this call's unknown clobber instead of either
        // preserving the pre-call input or inventing an architectural write.
        Op.Output = Temporary(Return.ValueBytes);
        MedVar Value = Op.Output;
        if (Return.ExtendTo32Bits) {
          MedOp Extend;
          Extend.Opcode =
              Signature.ReturnType->IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
          Extend.Addr = Op.Addr;
          Extend.Output = Temporary(4);
          Extend.addInput(Value);
          Value = Extend.Output;
          ReturnOps.push_back(std::move(Extend));
        }
        MedOp Upper;
        Upper.Opcode = NdOp::SUBBYTES;
        Upper.Addr = Op.Addr;
        Upper.Output = Temporary(8 - Value.Size);
        Upper.addInput(ndVarToMedVar(NdVar::reg(Return.RegisterOffset, 8)));
        Upper.addInput(MedVar::makeConst(Value.Size, 4));
        MedOp Merge;
        Merge.Opcode = NdOp::CONCAT;
        Merge.Addr = Op.Addr;
        Merge.Output = ndVarToMedVar(NdVar::reg(Return.RegisterOffset, 8));
        Merge.addInput(Upper.Output);
        Merge.addInput(Value);
        ReturnOps.push_back(std::move(Upper));
        ReturnOps.push_back(std::move(Merge));
      } else if (const auto Wide = FloatingCarrier(Return)) {
        AppendFloatingCarrier(Op.Output, *Wide);
      }
      // These hints were matched to an exact imported runtime declaration by
      // the loader and passed ABI validation above. Carry its termination
      // effect into MedIR as well as source flow. Native candidate signatures
      // are not declarations of this effect and use the separate fixed point.
      if (Hint->DoesNotReturn &&
          Signature.ReturnType->Kind == NdTypeKind::Void &&
          (Hint->CallKind == SourceCallTypeHint::Kind::ObjCRuntimeCall ||
           Hint->CallKind == SourceCallTypeHint::Kind::DarwinRuntimeCall ||
           Hint->CallKind == SourceCallTypeHint::Kind::SwiftRuntimeCall))
        Op.DoesNotReturn = true;
      Op.SourceCallHint =
          std::make_shared<const SourceCallTypeHint>(std::move(*Hint));
      Ops.push_back(std::move(Op));
      for (auto &ReturnOp : ReturnOps)
        Ops.push_back(std::move(ReturnOp));
    }
    Block.Ops = std::move(Ops);
  }

  if (!EntrySignature ||
      EntrySignature->ReturnLocation.Kind !=
          SourceABICarrierKind::IntegerRegister ||
      EntrySignature->ReturnLocation.ValueBytes >= 8)
    return;
  const auto Bytes = EntrySignature->ReturnLocation.ValueBytes;
  std::set<uint64_t> ReturnRegisters{
      EntrySignature->ReturnLocation.RegisterOffset};
  auto WideCopy = [&](const MedOp &Op) {
    return Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
           Op.Output.Kind == MedVar::Reg && Op.Inputs[0].Kind == MedVar::Reg &&
           Op.Output.Size > Bytes &&
           (Op.Output.Size == 2 || Op.Output.Size == 4 ||
            Op.Output.Size == 8) &&
           Op.Inputs[0].Size == Op.Output.Size &&
           !TRI.isFrameOrLinkReg(Op.Output.RegOff) &&
           !TRI.isStackPointer(Op.Output.RegOff) &&
           !TRI.isVectorReg(Op.Output.RegOff) &&
           !TRI.isFrameOrLinkReg(Op.Inputs[0].RegOff) &&
           !TRI.isStackPointer(Op.Inputs[0].RegOff) &&
           !TRI.isVectorReg(Op.Inputs[0].RegOff);
  };
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (const auto &Block : Func.Blocks)
      for (const auto &Op : Block.Ops)
        if (WideCopy(Op) && ReturnRegisters.count(Op.Output.RegOff))
          Changed |= ReturnRegisters.insert(Op.Inputs[0].RegOff).second;
  }
  for (auto &Block : Func.Blocks) {
    std::vector<MedOp> Ops;
    for (auto Op : Block.Ops) {
      if (WideCopy(Op) && ReturnRegisters.count(Op.Output.RegOff)) {
        // A saved full register can carry a narrow result through several
        // merges before the final return. Expose its low-lane dependency now
        // so subregister normalization builds narrow PHIs along that path.
        // CONCAT preserves every upper bit; it is not a narrow register write.
        const auto Input = Op.Inputs[0];
        MedOp Low;
        Low.Opcode = NdOp::SUBBYTES;
        Low.Addr = Op.Addr;
        Low.Output = Temporary(Bytes);
        Low.addInput(ndVarToMedVar(NdVar::reg(Input.RegOff, Bytes)));
        Low.addInput(MedVar::makeConst(0, 4));
        MedVar Value = Low.Output;
        Ops.push_back(std::move(Low));
        // Reassemble with ordinary 1/2/4/8-byte values. A seven-byte "high"
        // would leak a nonexistent uint56_t into C when this register also
        // carries a pointer along another path.
        for (uint16_t Width = Bytes; Width < Input.Size; Width *= 2) {
          MedOp Upper;
          Upper.Opcode = NdOp::SUBBYTES;
          Upper.Addr = Op.Addr;
          Upper.Output = Temporary(Width);
          Upper.addInput(Input);
          Upper.addInput(MedVar::makeConst(Width, 4));
          const bool Last = Width * 2 == Input.Size;
          MedOp Piece;
          Piece.Opcode = NdOp::CONCAT;
          Piece.Addr = Op.Addr;
          Piece.Output = Last ? Op.Output : Temporary(Width * 2);
          Piece.addInput(Upper.Output);
          Piece.addInput(Value);
          Value = Piece.Output;
          Ops.push_back(std::move(Upper));
          if (Last) {
            Op.Opcode = Piece.Opcode;
            Op.NumInputs = 0;
            Op.addInput(Piece.Inputs[0]);
            Op.addInput(Piece.Inputs[1]);
          } else {
            Ops.push_back(std::move(Piece));
          }
        }
      }
      Ops.push_back(std::move(Op));
    }
    Block.Ops = std::move(Ops);
  }
}
} // namespace neverd

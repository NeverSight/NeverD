#include "neverd/loader/MachO/CFunctionParameterCalls.h"

#include "../SourceUnwind.h"
#include "SourceLocalCall.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/DarwinImportVeneer.h"
#include "neverd/loader/MachO/RuntimeFunctionAddress.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/Support/Endian.h"

#include <deque>
#include <set>

namespace neverd {
namespace {
bool scalar(const TypeRef &Type, bool Void = false) {
  return Type && equalSourceTypes(Type, Type) &&
         ((Void && Type->Kind == NdTypeKind::Void) ||
          (Type->Kind == NdTypeKind::Ptr && Type->Size == 8) ||
          (Type->Kind == NdTypeKind::Int &&
           (Type->Size == 1 || Type->Size == 2 || Type->Size == 4 ||
            Type->Size == 8)) ||
          (Type->Kind == NdTypeKind::Float &&
           (Type->Size == 4 || Type->Size == 8)));
}

bool canonical(const SourceFunctionTypeHint &Signature, Arch Architecture) {
  std::string Error;
  if (!Signature.HasExplicitABI || Signature.Architecture != Architecture ||
      !validateSourceABI(Signature, Error))
    return false;
  auto Expected = Signature;
  const bool Assigned =
      Signature.Convention == SourceFunctionTypeHint::ConventionKind::C
          ? assignDarwinScalarSourceABI(Expected, Architecture, Error)
          : assignDarwinSwiftSourceABI(Expected, Architecture, Error);
  return Assigned && equalSourceABIs(Signature, Expected);
}

bool indirectCall(const BinaryImage &Image, const LowBlock &Block,
                  const LowOp &Call, uint64_t Register) {
  if (Register % 8 || Register > a64reg::X28)
    return false;
  const LowInstructionBoundary *Boundary = nullptr;
  for (const auto &Candidate : Block.InstructionBoundaries)
    if (Candidate.Address == Call.Addr) {
      if (Boundary)
        return false;
      Boundary = &Candidate;
    }
  if (!Boundary || Boundary->Size != 4 || Boundary->OpCount != 2 ||
      Boundary->Control != LowInstructionControl::Call ||
      Boundary->ControlFlags != (LowInstructionControlFlag::Call |
                                 LowInstructionControlFlag::Indirect) ||
      Boundary->Mode != InstructionMode::Default || Boundary->Immediate ||
      Boundary->TargetMode != LowInstructionTargetMode::Preserve ||
      &Block.Ops[Boundary->FirstOp + 1] != &Call ||
      Call.Opcode != NdOp::INDIR_CALL || Call.Seq != 1 || Call.NumInputs != 1 ||
      Call.Inputs[0] != NdVar::reg(Register, 8) ||
      Call.Output != NdVar::reg(a64reg::X0, 8))
    return false;
  const auto &Link = Block.Ops[Boundary->FirstOp];
  if (Link.Opcode != NdOp::COPY || Link.Seq != 0 ||
      Link.Output != NdVar::reg(a64reg::X30, 8) || Link.NumInputs != 1 ||
      !Link.Inputs[0].isConst() || Link.Inputs[0].Size != 8 ||
      Link.Inputs[0].Offset != Call.Addr + 4)
    return false;
  const auto Bytes = readImmutableCodeBytes(Image, Call.Addr, 4);
  return Bytes && llvm::support::endian::read32le(Bytes->data()) ==
                      (0xd63f0000u | (uint32_t(Register / 8) << 5));
}

bool registerCopy(const BinaryImage &Image, const LowBlock &Block,
                  const LowOp &Op) {
  if (Op.Opcode != NdOp::COPY || Op.Seq != 0 || Op.NumInputs != 1 ||
      !Op.Output.isReg() || Op.Output.Size != 8 || Op.Output.Offset % 8 ||
      Op.Output.Offset > a64reg::X28 || !Op.Inputs[0].isReg() ||
      Op.Inputs[0].Size != 8 || Op.Inputs[0].Offset % 8 ||
      Op.Inputs[0].Offset > a64reg::X28)
    return false;
  for (const auto &Boundary : Block.InstructionBoundaries)
    if (Boundary.Address == Op.Addr) {
      if (Boundary.Size != 4 || Boundary.OpCount != 1 ||
          &Block.Ops[Boundary.FirstOp] != &Op ||
          Boundary.Control != LowInstructionControl::None ||
          Boundary.ControlFlags != LowInstructionControlFlag::None)
        return false;
      const auto Bytes = readImmutableCodeBytes(Image, Op.Addr, 4);
      return Bytes &&
             llvm::support::endian::read32le(Bytes->data()) ==
                 (0xaa0003e0u | (uint32_t(Op.Inputs[0].Offset / 8) << 16) |
                  uint32_t(Op.Output.Offset / 8));
    }
  return false;
}

std::optional<SourceFunctionTypeHint>
callSignature(const BinaryImage &Image, va_t Target,
              const std::map<va_t, SourceFunctionTypeHint> *NativeCallees) {
  // Runtime catalogs consume the imported slot, while LowIR names the
  // physical veneer. Resolve that identity before asking for its declaration.
  // The shared address owner authenticates the provider, actual export and
  // ordinary C ABI; an inferred native signature cannot override it.
  const auto Slot = isImmutableImageImportSlot(Image, Target)
                        ? std::optional<va_t>(Target)
                        : darwinImportVeneerSlot(Image, Target);
  if (Slot) {
    const auto Address = runtimeCFunctionAddressHint(Image, *Slot);
    return Address && Address->AddressedFunctionABI &&
                   canonical(*Address->AddressedFunctionABI, Image.Arch)
               ? Address->AddressedFunctionABI
               : std::nullopt;
  }
  if (NativeCallees)
    if (const auto It = NativeCallees->find(Target);
        It != NativeCallees->end() && canonical(It->second, Image.Arch))
      return It->second;
  return std::nullopt;
}
} // namespace

std::optional<SourceFunctionTypeHint>
cFunctionParameterSignature(const SourceFunctionTypeHint &Entry,
                            unsigned Parameter, Arch Architecture) {
  std::string Error;
  if (!Entry.HasExplicitABI || Entry.Architecture != Architecture ||
      !validateSourceABI(Entry, Error) || Parameter >= Entry.Parameters.size())
    return std::nullopt;
  const auto &Input = Entry.Parameters[Parameter];
  const auto &Type = Input.Type;
  if (Input.TheRole != SourceParameterTypeHint::Role::Ordinary ||
      !Input.Components.empty() ||
      Input.Location.Kind != SourceABICarrierKind::IntegerRegister ||
      Input.Location.ValueBytes != 8 || Input.Location.ExtendTo32Bits ||
      !Type || Type->Kind != NdTypeKind::Ptr || Type->Size != 8 ||
      !Type->Pointee || Type->Pointee->Kind != NdTypeKind::Func ||
      !scalar(Type->Pointee->RetType, true) ||
      Type->Pointee->ParamTypes.size() > 64)
    return std::nullopt;
  SourceFunctionTypeHint Result;
  Result.Origin = Entry.Origin;
  Result.ReturnType = Type->Pointee->RetType;
  for (const auto &Argument : Type->Pointee->ParamTypes) {
    if (!scalar(Argument))
      return std::nullopt;
    Result.Parameters.push_back({"", Argument});
  }
  return assignDarwinScalarSourceABI(Result, Architecture, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Result))
             : std::nullopt;
}

bool isCFunctionParameterCallHint(const SourceCallTypeHint &Hint,
                                  const SourceFunctionTypeHint &Entry,
                                  va_t FunctionEntry, Arch Architecture) {
  if (Architecture != Arch::AArch64 ||
      Hint.CallKind != SourceCallTypeHint::Kind::CFunctionParameterCall ||
      !Hint.FunctionParameterCall || Hint.ImmutableNativeCall ||
      !FunctionEntry || FunctionEntry % 4 ||
      Hint.FunctionParameterCall->FunctionEntry != FunctionEntry ||
      !Hint.FunctionParameterCall->Site.Instruction ||
      Hint.FunctionParameterCall->Site.Instruction % 4 ||
      Hint.FunctionParameterCall->Site.Sequence != 1 ||
      Hint.FunctionParameterCall->Site.Opcode != NdOp::INDIR_CALL ||
      Hint.FunctionParameterCall->Site.StaticTarget || Hint.TargetAddress ||
      !Hint.TargetName.empty() || Hint.BooleanResult || Hint.ValueWitness ||
      Hint.Virtual || Hint.DoesNotReturn || Hint.WeakImport ||
      Hint.ReturnedArgument || Hint.RuntimeObjCResultType ||
      !Hint.Selector.empty() || !Hint.OwnerClass.empty() ||
      Hint.SelectorReferenceAddress || !Hint.BorrowedByteInputs.empty() ||
      !Hint.SwiftStaticStringInputs.empty() ||
      !Hint.CanonicalBooleanInputs.empty() || !Hint.SwiftStringInputs.empty() ||
      Hint.Format || Hint.NilTerminated || Hint.SwiftTypeMetadata ||
      Hint.Receiver || Hint.SelectorResultUse || Hint.SelectorResultTypeUse ||
      Hint.SelectorArgumentTypeUse || Hint.SelectorForwardingUse ||
      Hint.SelectorArgumentStorageUse || Hint.ObjCIndirectResultStorage ||
      Hint.ByteCount || Hint.ImmutablePointerSlot || Hint.AddressedFunctionABI)
    return false;
  const auto Expected = cFunctionParameterSignature(
      Entry, Hint.FunctionParameterCall->Parameter, Architecture);
  return Expected && Hint.Signature.Origin == Entry.Origin &&
         equalSourceABIs(Hint.Signature, *Expected);
}

bool isCFunctionParameterSourceCall(const HighExpr &Expression,
                                    const HighFunc &Function,
                                    Arch Architecture) {
  if (Expression.Kind != ExprKind::Call || !Expression.SourceCallHint ||
      !Function.SourceTypeHint ||
      !isCFunctionParameterCallHint(*Expression.SourceCallHint,
                                    *Function.SourceTypeHint, Function.Entry,
                                    Architecture) ||
      !Expression.IsIndirectCall || Expression.CallAddr ||
      Expression.IntrinsicId != Intrinsic::None ||
      !Expression.IntrinsicOutputs.empty() ||
      Expression.MemoryOrdering != NdMemoryOrdering::None ||
      Expression.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  const auto &Binding = *Expression.SourceCallHint;
  const auto Parameter = Binding.FunctionParameterCall->Parameter;
  const auto &Target = Expression.IndirectTarget;
  const auto &Return = Binding.Signature.ReturnType;
  const bool ResultType =
      Return->Kind == NdTypeKind::Void
          ? equalSourceTypes(Expression.Type, Return)
          : Expression.Type && Expression.Type->Size == Return->Size &&
                (Expression.Type->Kind == NdTypeKind::Int ||
                 Expression.Type->Kind == NdTypeKind::Ptr ||
                 Expression.Type->Kind == NdTypeKind::Float);
  if (!Target || Target->Kind != ExprKind::Var ||
      Target->Var.Kind != MedVar::Param || Target->Var.Id != int(Parameter) ||
      Target->Var.TheArch != Architecture || !Target->Type ||
      Target->Type->Size != 8 ||
      (Target->Type->Kind != NdTypeKind::Int &&
       Target->Type->Kind != NdTypeKind::Ptr) ||
      (Expression.IndirectParamIdx >= 0 &&
       Expression.IndirectParamIdx != int(Parameter)) ||
      Parameter >= Function.Params.size() ||
      !equalSourceTypes(Function.Params[Parameter].Type,
                        Function.SourceTypeHint->Parameters[Parameter].Type) ||
      !ResultType ||
      Expression.Operands.size() != Binding.Signature.Parameters.size())
    return false;
  for (unsigned I = 0; I < Expression.Operands.size(); ++I) {
    const auto &Argument = Expression.Operands[I];
    if (!Argument || !Argument->Type ||
        Argument->Type->Size != Binding.Signature.Parameters[I].Type->Size ||
        (Argument->Type->Kind != NdTypeKind::Int &&
         Argument->Type->Kind != NdTypeKind::Ptr &&
         Argument->Type->Kind != NdTypeKind::Float))
      return false;
  }
  return true;
}

std::map<va_t, SourceCallTypeHint> buildCFunctionParameterCallHints(
    const BinaryImage &Image, const LowFunc &Function,
    const SourceFunctionTypeHint &Entry,
    const std::map<va_t, SourceFunctionTypeHint> *NativeCallees) {
  using Facts = std::map<uint64_t, unsigned>;
  if (Entry.Parameters.size() > 64)
    return {};
  Facts Seed;
  std::map<unsigned, SourceFunctionTypeHint> Signatures;
  for (unsigned I = 0; I < Entry.Parameters.size(); ++I)
    if (auto Signature = cFunctionParameterSignature(Entry, I, Image.Arch)) {
      const auto Register = Entry.Parameters[I].Location.RegisterOffset;
      if (Register % 8 || Register > a64reg::X28 ||
          !Seed.emplace(Register, I).second)
        return {};
      Signatures.emplace(I, std::move(*Signature));
    }
  if (Seed.empty() || Image.Format != BinaryFormat::MachO ||
      Image.Arch != Arch::AArch64 || Image.Bits != Bitness::Bits64 ||
      Image.IsRelocatable || !Function.Entry || Function.Entry % 4 ||
      !Function.DecodedInstructionCount ||
      !Function.hasCompleteLiftCoverage() || Function.Blocks.empty() ||
      Function.Blocks.size() > 4096 || !Function.JumpTables.empty() ||
      (Function.ExceptionMetadata &&
       !isPlainSourceUnwind(*Function.ExceptionMetadata)) ||
      (!Function.ModuleAnalysisRoots.empty() &&
       Function.ModuleAnalysisRoots != std::set<va_t>{Function.Entry}) ||
      (!Function.OrdinaryModuleAnalysisRoots.empty() &&
       Function.OrdinaryModuleAnalysisRoots != std::set<va_t>{Function.Entry}))
    return {};
  if (auto Error = validateLowInstructionBoundaries(
          Function, LowInstructionBoundaryRequirement::Required)) {
    llvm::consumeError(std::move(Error));
    return {};
  }
  std::map<int, const LowBlock *> Blocks;
  std::map<int, size_t> Pending;
  const LowBlock *Root = nullptr;
  size_t Budget = 262144;
  std::set<va_t> Instructions;
  for (const auto &Block : Function.Blocks) {
    if (Block.Id < 0 || !Blocks.emplace(Block.Id, &Block).second ||
        !Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty() ||
        Block.Ops.size() > Budget || Block.Preds.size() > Budget ||
        Block.Succs.size() > Budget ||
        Block.InstructionBoundaries.size() > Budget ||
        std::set<int>(Block.Preds.begin(), Block.Preds.end()).size() !=
            Block.Preds.size() ||
        std::set<int>(Block.Succs.begin(), Block.Succs.end()).size() !=
            Block.Succs.size())
      return {};
    Budget -= Block.Ops.size();
    for (const auto &Boundary : Block.InstructionBoundaries)
      if (Boundary.Size != 4 || Boundary.Address % 4 ||
          !Instructions.insert(Boundary.Address).second)
        return {};
    if (Block.StartAddr == Function.Entry) {
      if (Root || !Block.Preds.empty())
        return {};
      Root = &Block;
    }
    Pending.emplace(Block.Id, Block.Preds.size());
  }
  if (!Root || Instructions.size() != Function.DecodedInstructionCount)
    return {};
  for (const auto &[Id, Block] : Blocks) {
    if (Block != Root && Block->Preds.empty())
      return {};
    for (int Succ : Block->Succs) {
      const auto It = Blocks.find(Succ);
      if (It == Blocks.end() || std::count(It->second->Preds.begin(),
                                           It->second->Preds.end(), Id) != 1)
        return {};
    }
    for (int Pred : Block->Preds) {
      const auto It = Blocks.find(Pred);
      if (It == Blocks.end() || !It->second->hasSucc(Id))
        return {};
    }
  }
  const auto DirectCalls = sourceLocalCalls(Image, Function);
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  std::map<va_t, SourceCallTypeHint> Result;
  std::map<int, Facts> Outputs;
  std::deque<int> Queue{Root->Id};
  auto Kill = [](Facts &Values, const NdVar &Output) {
    if (!Output.isReg() || !Output.Size)
      return;
    for (auto It = Values.begin(); It != Values.end();)
      if (It->first < Output.Offset + Output.Size &&
          Output.Offset < It->first + 8)
        It = Values.erase(It);
      else
        ++It;
  };
  auto CrossCall = [&](Facts &Values,
                       const std::optional<SourceFunctionTypeHint> &Signature) {
    if (!Signature) {
      Values.clear();
      return;
    }
    for (auto It = Values.begin(); It != Values.end();)
      if (!TRI.isCallPreserved(It->first, 8, BinaryFormat::MachO))
        It = Values.erase(It);
      else
        ++It;
    const auto EraseLocation = [&](const SourceABIValueLocation &Location) {
      if (Location.Kind == SourceABICarrierKind::IntegerRegister ||
          Location.Kind == SourceABICarrierKind::FloatingRegister)
        Kill(Values, NdVar::reg(Location.RegisterOffset, Location.ValueBytes));
    };
    EraseLocation(Signature->ReturnLocation);
    for (const auto &Piece : Signature->ReturnComponents)
      EraseLocation(Piece);
  };
  while (!Queue.empty()) {
    const int Id = Queue.front();
    Queue.pop_front();
    const auto &Block = *Blocks.at(Id);
    Facts Values = Block.Preds.empty() ? Seed : Outputs.at(Block.Preds.front());
    for (int Pred : Block.Preds)
      for (auto It = Values.begin(); It != Values.end();) {
        const auto Found = Outputs.at(Pred).find(It->first);
        if (Found == Outputs.at(Pred).end() || Found->second != It->second)
          It = Values.erase(It);
        else
          ++It;
      }
    for (const auto &Op : Block.Ops) {
      if (Op.NumInputs > 6 || Op.Output.Offset > UINT64_MAX - Op.Output.Size ||
          Op.MemoryOrdering != NdMemoryOrdering::None ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return {};
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        const auto Site = sourceCallOccurrenceKey(Op);
        std::optional<SourceFunctionTypeHint> Signature;
        if (Site && Site->Opcode == NdOp::CALL && Site->StaticTarget &&
            DirectCalls.count(*Site))
          Signature = callSignature(Image, *Site->StaticTarget, NativeCallees);
        else if (Site && Site->Opcode == NdOp::INDIR_CALL &&
                 !Site->StaticTarget && Op.Inputs[0].isReg()) {
          const auto Origin = Values.find(Op.Inputs[0].Offset);
          if (Origin != Values.end() &&
              indirectCall(Image, Block, Op, Origin->first)) {
            Signature = Signatures.at(Origin->second);
            SourceCallTypeHint Hint;
            Hint.CallKind = SourceCallTypeHint::Kind::CFunctionParameterCall;
            Hint.FunctionParameterCall =
                SourceCallTypeHint::FunctionParameterCallEvidence{
                    Function.Entry, Origin->second, *Site};
            Hint.Signature = *Signature;
            if (!Result.emplace(Op.Addr, std::move(Hint)).second)
              return {};
          }
        }
        CrossCall(Values, Signature);
      } else if (Op.Opcode == NdOp::INTRINSIC) {
        Values.clear();
      } else {
        std::optional<unsigned> Origin;
        if (registerCopy(Image, Block, Op))
          if (const auto It = Values.find(Op.Inputs[0].Offset);
              It != Values.end())
            Origin = It->second;
        Kill(Values, Op.Output);
        if (Origin)
          Values.emplace(Op.Output.Offset, *Origin);
      }
    }
    Outputs.emplace(Id, std::move(Values));
    for (int Succ : Block.Succs)
      if (!--Pending.at(Succ))
        Queue.push_back(Succ);
  }
  // A cycle or disconnected component has no established all-path entry
  // identity. Do not publish the subset processed before discovering it.
  return Outputs.size() == Blocks.size() ? Result
                                         : std::map<va_t, SourceCallTypeHint>{};
}
} // namespace neverd

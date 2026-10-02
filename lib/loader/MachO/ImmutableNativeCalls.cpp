#include "neverd/loader/MachO/ImmutableNativeCalls.h"

#include "../SourceUnwind.h"
#include "SourceLocalCall.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/DarwinImportVeneer.h"
#include "neverd/loader/MachO/RuntimeFunctionAddress.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <set>

namespace neverd {
namespace {
bool overlaps(const NdVar &A, const NdVar &B) {
  return A.Space == B.Space && A.Size && B.Size &&
         (A.Offset <= B.Offset ? B.Offset - A.Offset < A.Size
                               : A.Offset - B.Offset < B.Size);
}

bool ordinary(const LowOp &Op) {
  return Op.NumInputs <= 6 && Op.MemoryOrdering == NdMemoryOrdering::None &&
         Op.MemoryAddressSpace == NdMemoryAddressSpace::Default;
}

bool generalRegister(const NdVar &Value) {
  return Value.isReg() && Value.Size == 8 && Value.Offset % 8 == 0 &&
         Value.Offset <= a64reg::X28 && Value.Offset != a64reg::X18;
}

class Trace {
  const BinaryImage &Image;
  const LowBlock &Block;
  const SourceLocalCalls &DirectCalls;
  const std::map<va_t, SourceFunctionTypeHint> *NativeCallees;
  size_t &Budget;
  std::map<va_t, const LowInstructionBoundary *> Boundaries;

  const LowInstructionBoundary *boundary(const LowOp &Op) const {
    const auto Found = Boundaries.find(Op.Addr);
    return Found == Boundaries.end() ? nullptr : Found->second;
  }

  std::optional<uint32_t>
  word(const LowOp &Op, unsigned Count,
       LowInstructionControl Control = LowInstructionControl::None) const {
    const auto *B = boundary(Op);
    if (!B || B->Size != 4 || B->OpCount != Count || B->Control != Control ||
        B->Mode != InstructionMode::Default ||
        B->TargetMode != LowInstructionTargetMode::Preserve ||
        (Control == LowInstructionControl::None &&
         (B->ControlFlags != LowInstructionControlFlag::None || B->Immediate)))
      return std::nullopt;
    const auto Bytes = readImmutableCodeBytes(Image, Op.Addr, 4);
    return Bytes ? std::optional(llvm::support::endian::read32le(Bytes->data()))
                 : std::nullopt;
  }

  bool preserves(const LowOp &Call, const NdVar &Value) const {
    const auto Site = sourceCallOccurrenceKey(Call);
    if (!ordinary(Call) || !generalRegister(Value) || !Site ||
        !Site->StaticTarget || !DirectCalls.count(*Site) ||
        !getTargetRegInfo(Image.Arch)
             .isCallPreserved(Value.Offset, 8, BinaryFormat::MachO))
      return false;
    std::optional<SourceFunctionTypeHint> Signature;
    const auto Slot = darwinImportVeneerSlot(Image, *Site->StaticTarget);
    if (Slot) {
      const auto Runtime = runtimeCFunctionAddressHint(Image, *Slot);
      if (Runtime)
        Signature = Runtime->AddressedFunctionABI;
      else if (isImmutableImageImportSlot(Image, *Slot)) {
        // Register-specific ARC imports have a catalogued physical input
        // carrier, but cannot be addressed as their canonical C function.
        // A preservation query uses that exact call declaration instead.
        const auto ARC = objcRuntimeSourceCallHint(Image, *Slot);
        const auto Bind = Image.DyldBindSlots.find(*Slot);
        if (ARC && ARC->CallKind == SourceCallTypeHint::Kind::ObjCRuntimeCall &&
            ARC->TargetAddress == *Slot && !ARC->DoesNotReturn &&
            !ARC->WeakImport && Bind != Image.DyldBindSlots.end() &&
            Bind->second.Module == "/usr/lib/libobjc.A.dylib" &&
            ARC->Signature.Origin ==
                SourceFunctionTypeHint::OriginKind::ObjCRuntime)
          Signature = ARC->Signature;
      }
    } else if (NativeCallees) {
      const auto Found = NativeCallees->find(*Site->StaticTarget);
      if (Found != NativeCallees->end() &&
          Found->second.Origin ==
              SourceFunctionTypeHint::OriginKind::NativeAnalysis)
        Signature = Found->second;
    }
    std::string Error;
    if (!Signature || !Signature->HasExplicitABI ||
        Signature->Architecture != Image.Arch ||
        !validateSourceABI(*Signature, Error))
      return false;
    const auto Clobbers = [&](const SourceABIValueLocation &Location) {
      return (Location.Kind == SourceABICarrierKind::IntegerRegister ||
              Location.Kind == SourceABICarrierKind::FloatingRegister) &&
             overlaps(Value,
                      NdVar::reg(Location.RegisterOffset, Location.ValueBytes));
    };
    return !Clobbers(Signature->ReturnLocation) &&
           std::none_of(Signature->ReturnComponents.begin(),
                        Signature->ReturnComponents.end(), Clobbers);
  }

  std::optional<size_t> definition(const NdVar &Value, size_t Before) {
    if (!generalRegister(Value) || Before > Block.Ops.size())
      return std::nullopt;
    for (size_t I = Before; I-- > 0;) {
      if (!Budget)
        return std::nullopt;
      --Budget;
      const auto &Op = Block.Ops[I];
      if (!ordinary(Op) || Op.Opcode == NdOp::INTRINSIC)
        return std::nullopt;
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        if (!preserves(Op, Value))
          return std::nullopt;
      }
      if (overlaps(Op.Output, Value))
        return Op.Output == Value ? std::optional(I) : std::nullopt;
    }
    return std::nullopt;
  }

  bool copy(const LowOp &Op) const {
    const auto Word = word(Op, 1);
    return Word && Op.Seq == 0 && Op.Opcode == NdOp::COPY &&
           Op.NumInputs == 1 && generalRegister(Op.Output) &&
           generalRegister(Op.Inputs[0]) &&
           *Word == (0xaa0003e0u | (uint32_t(Op.Inputs[0].Offset / 8) << 16) |
                     uint32_t(Op.Output.Offset / 8));
  }

  struct Address {
    va_t Value;
    bool Page;
  };

  std::optional<Address> address(const NdVar &Value, size_t Before,
                                 unsigned Depth) {
    if (Depth > 48)
      return std::nullopt;
    const auto Index = definition(Value, Before);
    if (!Index)
      return std::nullopt;
    const auto &Op = Block.Ops[*Index];
    if (copy(Op))
      return address(Op.Inputs[0], *Index, Depth + 1);
    const auto *B = boundary(Op);
    const auto Word = B ? word(Op, B->OpCount) : std::nullopt;
    if (!Word)
      return std::nullopt;
    const unsigned Dst = *Word & 31;
    if (Op.Output != NdVar::reg(Dst * 8, 8))
      return std::nullopt;
    if ((*Word & 0x9f000000) == 0x90000000) {
      const uint64_t Page = Op.Addr & ~uint64_t(0xfff);
      const uint32_t Imm = ((*Word >> 29) & 3) | ((*Word >> 3) & 0x1ffffc);
      const int64_t Delta =
          (int64_t(Imm) - ((Imm & 0x100000) ? 0x200000 : 0)) * 4096;
      if ((Delta < 0 && Page < uint64_t(-Delta)) ||
          (Delta >= 0 && Page > UINT64_MAX - uint64_t(Delta)) ||
          B->OpCount != 1 || Op.Seq != 0 || Op.Opcode != NdOp::COPY ||
          Op.NumInputs != 1 ||
          Op.Inputs[0] != NdVar::addressFragment(Page + Delta, 8))
        return std::nullopt;
      return Address{Page + Delta, true};
    }
    if ((*Word & 0xffc00000) != 0x91000000 || B->OpCount != 3 || Op.Seq != 2 ||
        Op.Opcode != NdOp::INT_ADD || Op.NumInputs != 2 ||
        Op.Inputs[0] != NdVar::reg(((*Word >> 5) & 31) * 8, 8) ||
        Op.Inputs[1] != NdVar::scalar((*Word >> 10) & 0xfff, 4))
      return std::nullopt;
    const auto &Left = Block.Ops[B->FirstOp];
    const auto &Right = Block.Ops[B->FirstOp + 1];
    if (!ordinary(Left) || !ordinary(Right) || Left.Opcode != NdOp::COPY ||
        Left.Seq != 0 || Left.NumInputs != 1 || !Left.Output.isTemp() ||
        Left.Output.Size != 8 || Left.Inputs[0] != Op.Inputs[0] ||
        Right.Opcode != NdOp::COPY || Right.Seq != 1 || Right.NumInputs != 1 ||
        !Right.Output.isTemp() || Right.Output.Size != 4 ||
        overlaps(Left.Output, Right.Output) || Right.Inputs[0] != Op.Inputs[1])
      return std::nullopt;
    const auto Base = address(Op.Inputs[0], B->FirstOp, Depth + 1);
    const auto Offset = (*Word >> 10) & 0xfff;
    if (!Base || !Base->Page || Base->Value > UINT64_MAX - Offset)
      return std::nullopt;
    return Address{Base->Value + Offset, false};
  }

public:
  Trace(const BinaryImage &Image, const LowBlock &Block,
        const SourceLocalCalls &DirectCalls,
        const std::map<va_t, SourceFunctionTypeHint> *NativeCallees,
        size_t &Budget)
      : Image(Image), Block(Block), DirectCalls(DirectCalls),
        NativeCallees(NativeCallees), Budget(Budget) {
    for (const auto &B : Block.InstructionBoundaries)
      Boundaries.emplace(B.Address, &B);
  }

  bool indirectCall(const LowOp &Call) const {
    if (Call.Opcode != NdOp::INDIR_CALL || Call.Seq != 1 ||
        Call.NumInputs != 1 || !ordinary(Call) ||
        !generalRegister(Call.Inputs[0]) ||
        Call.Output != NdVar::reg(a64reg::X0, 8))
      return false;
    const auto Word = word(Call, 2, LowInstructionControl::Call);
    const auto *B = boundary(Call);
    if (!Word || B->Immediate ||
        B->ControlFlags != (LowInstructionControlFlag::Call |
                            LowInstructionControlFlag::Indirect) ||
        &Block.Ops[B->FirstOp + 1] != &Call ||
        *Word != (0xd63f0000u | (uint32_t(Call.Inputs[0].Offset / 8) << 5)))
      return false;
    const auto &Link = Block.Ops[B->FirstOp];
    return ordinary(Link) && Link.Opcode == NdOp::COPY && Link.Seq == 0 &&
           Link.NumInputs == 1 && Link.Output == NdVar::reg(a64reg::X30, 8) &&
           Link.Inputs[0].isConst() && Link.Inputs[0].Size == 8 &&
           Link.Inputs[0].Offset == Call.Addr + 4;
  }

  std::optional<va_t> slot(const NdVar &Value, size_t Before,
                           unsigned Depth = 0) {
    if (Depth > 48)
      return std::nullopt;
    const auto Index = definition(Value, Before);
    if (!Index)
      return std::nullopt;
    const auto &Op = Block.Ops[*Index];
    if (copy(Op))
      return slot(Op.Inputs[0], *Index, Depth + 1);
    const auto *B = boundary(Op);
    if (!B || Op.Opcode != NdOp::COPY || Op.NumInputs != 1)
      return std::nullopt;
    const auto Word = word(Op, B->OpCount);
    if (!Word || (*Word & 0xffc00000) != 0xf9400000 ||
        Op.Output != NdVar::reg((*Word & 31) * 8, 8))
      return std::nullopt;
    const auto BaseRegister = NdVar::reg(((*Word >> 5) & 31) * 8, 8);
    const uint64_t Offset = ((*Word >> 10) & 0xfff) * 8;
    const unsigned Count = Offset ? 4 : 3;
    if (B->OpCount != Count || Op.Seq != int(Count - 1) ||
        &Block.Ops[B->FirstOp + Count - 1] != &Op)
      return std::nullopt;
    const auto &BaseCopy = Block.Ops[B->FirstOp];
    const auto &Load = Block.Ops[B->FirstOp + Count - 2];
    if (!ordinary(BaseCopy) || BaseCopy.Opcode != NdOp::COPY ||
        BaseCopy.Seq != 0 || BaseCopy.NumInputs != 1 ||
        !BaseCopy.Output.isTemp() || BaseCopy.Output.Size != 8 ||
        BaseCopy.Inputs[0] != BaseRegister || !ordinary(Load) ||
        Load.Opcode != NdOp::LOAD || Load.Seq != int(Count - 2) ||
        Load.NumInputs != 1 || Load.Inputs[0] != BaseCopy.Output ||
        !Load.Output.isTemp() || Load.Output.Size != 8 ||
        overlaps(Load.Output, BaseCopy.Output) || Op.Inputs[0] != Load.Output)
      return std::nullopt;
    if (Offset) {
      const auto &Add = Block.Ops[B->FirstOp + 1];
      if (!ordinary(Add) || Add.Opcode != NdOp::INT_ADD || Add.Seq != 1 ||
          Add.NumInputs != 2 || Add.Output != BaseCopy.Output ||
          Add.Inputs[0] != BaseCopy.Output ||
          Add.Inputs[1] != NdVar::scalar(Offset, 8))
        return std::nullopt;
    }
    const auto Base = address(BaseRegister, B->FirstOp, Depth + 1);
    return Base && Base->Value <= UINT64_MAX - Offset
               ? std::optional(Base->Value + Offset)
               : std::nullopt;
  }
};
} // namespace

std::map<va_t, ImmutableNativeCallTarget> immutableNativeCallTargets(
    const BinaryImage &Image, const LowFunc &Function,
    const std::map<va_t, SourceFunctionTypeHint> *NativeCallees) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Arch != Arch::AArch64 || Image.Bits != Bitness::Bits64 ||
      !Function.Entry || Function.Entry % 4 ||
      !Function.DecodedInstructionCount ||
      !Function.hasCompleteLiftCoverage() || Function.Blocks.empty() ||
      Function.Blocks.size() > 4096 || !Function.JumpTables.empty() ||
      (!Function.ModuleAnalysisRoots.empty() &&
       Function.ModuleAnalysisRoots != std::set<va_t>{Function.Entry}) ||
      (!Function.OrdinaryModuleAnalysisRoots.empty() &&
       Function.OrdinaryModuleAnalysisRoots !=
           std::set<va_t>{Function.Entry}) ||
      (Function.ExceptionMetadata &&
       !isPlainSourceUnwind(*Function.ExceptionMetadata)))
    return {};
  if (auto Error = validateLowInstructionBoundaries(
          Function, LowInstructionBoundaryRequirement::Required)) {
    llvm::consumeError(std::move(Error));
    return {};
  }
  size_t Remaining = 262144, InstructionCount = 0;
  std::set<va_t> Instructions;
  bool HasIndirect = false;
  for (const auto &Block : Function.Blocks) {
    if (Block.Ops.size() > Remaining || !Block.ExceptionalPreds.empty() ||
        !Block.ExceptionalSuccs.empty())
      return {};
    Remaining -= Block.Ops.size();
    InstructionCount += Block.InstructionBoundaries.size();
    for (const auto &B : Block.InstructionBoundaries)
      if (B.Size != 4 || B.Address % 4 ||
          !Instructions.insert(B.Address).second)
        return {};
    for (const auto &Op : Block.Ops)
      HasIndirect |= Op.Opcode == NdOp::INDIR_CALL;
  }
  if (!HasIndirect || InstructionCount != Function.DecodedInstructionCount)
    return {};
  const auto DirectCalls = sourceLocalCalls(Image, Function);
  std::map<va_t, ImmutableNativeCallTarget> Result;
  for (const auto &Block : Function.Blocks) {
    Trace Proof(Image, Block, DirectCalls, NativeCallees, Remaining);
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const auto &Call = Block.Ops[I];
      if (!Proof.indirectCall(Call))
        continue;
      const auto Slot = Proof.slot(Call.Inputs[0], I);
      const auto Target =
          Slot ? readImmutableImageCodePointer(Image, *Slot) : std::nullopt;
      const auto Site = sourceCallOccurrenceKey(Call);
      if (Target && Site && *Target != Function.Entry &&
          !Result
               .emplace(Call.Addr,
                        ImmutableNativeCallTarget{Function.Entry, *Site, *Slot,
                                                  *Target})
               .second)
        return {};
    }
  }
  return Remaining ? Result : std::map<va_t, ImmutableNativeCallTarget>{};
}
namespace {
bool scalarCarrier(const TypeRef &Type, bool AllowVoid = false) {
  return Type &&
         ((AllowVoid && Type->Kind == NdTypeKind::Void) ||
          (Type->Kind == NdTypeKind::Int &&
           (Type->Size == 1 || Type->Size == 2 || Type->Size == 4 ||
            Type->Size == 8)) ||
          (Type->Kind == NdTypeKind::Ptr && Type->Size == 8 && Type->Pointee) ||
          (Type->Kind == NdTypeKind::Float &&
           (Type->Size == 4 || Type->Size == 8)));
}
} // namespace

bool isImmutableNativeCallHint(const SourceCallTypeHint &Hint,
                               va_t FunctionEntry, Arch Architecture) {
  if (Architecture != Arch::AArch64 || !FunctionEntry || FunctionEntry % 4 ||
      Hint.CallKind != SourceCallTypeHint::Kind::Native ||
      !Hint.ImmutableNativeCall || Hint.FunctionParameterCall ||
      !Hint.TargetAddress || Hint.TargetAddress == FunctionEntry ||
      Hint.TargetAddress % 4 || !Hint.TargetName.empty() ||
      Hint.BooleanResult || Hint.ValueWitness || Hint.Virtual ||
      Hint.DoesNotReturn || Hint.WeakImport || Hint.ReturnedArgument ||
      Hint.RuntimeObjCResultType || !Hint.Selector.empty() ||
      !Hint.OwnerClass.empty() || Hint.SelectorReferenceAddress ||
      !Hint.BorrowedByteInputs.empty() ||
      !Hint.SwiftStaticStringInputs.empty() ||
      !Hint.CanonicalBooleanInputs.empty() || !Hint.SwiftStringInputs.empty() ||
      Hint.Format || Hint.NilTerminated || Hint.SwiftTypeMetadata ||
      Hint.Receiver || Hint.SelectorResultUse || Hint.SelectorResultTypeUse ||
      Hint.SelectorArgumentTypeUse || Hint.SelectorForwardingUse ||
      Hint.SelectorArgumentStorageUse || Hint.ObjCIndirectResultStorage ||
      Hint.ByteCount || Hint.ImmutablePointerSlot || Hint.AddressedFunctionABI)
    return false;
  const auto &Proof = *Hint.ImmutableNativeCall;
  const auto &ABI = Hint.Signature;
  std::string Error;
  return Proof.FunctionEntry == FunctionEntry && Proof.Site.Instruction &&
         Proof.Site.Instruction % 4 == 0 && Proof.Site.Sequence == 1 &&
         Proof.Site.Opcode == NdOp::INDIR_CALL && !Proof.Site.StaticTarget &&
         Proof.Slot && Proof.Slot % 8 == 0 &&
         Proof.Target == Hint.TargetAddress &&
         ABI.Origin == SourceFunctionTypeHint::OriginKind::NativeAnalysis &&
         ABI.HasExplicitABI && ABI.Architecture == Architecture &&
         validateSourceABI(ABI, Error) && scalarCarrier(ABI.ReturnType, true) &&
         ABI.ReturnComponents.empty() && ABI.Parameters.size() <= 64 &&
         std::all_of(ABI.Parameters.begin(), ABI.Parameters.end(),
                     [](const SourceParameterTypeHint &P) {
                       return scalarCarrier(P.Type) && P.Components.empty();
                     });
}

std::map<va_t, SourceCallTypeHint> buildImmutableNativeCallHints(
    const BinaryImage &Image, const LowFunc &Function,
    const std::map<va_t, SourceFunctionTypeHint> &NativeCallees) {
  std::map<va_t, SourceCallTypeHint> Result;
  if (NativeCallees.empty() || Function.Blocks.size() > 4096)
    return Result;
  // Most source-bound functions have no indirect call. Avoid rebuilding
  // instruction-boundary indexes for those functions on every inference round.
  size_t Remaining = 262144;
  bool HasIndirect = false;
  for (const auto &Block : Function.Blocks) {
    if (Block.Ops.size() > Remaining)
      return Result;
    Remaining -= Block.Ops.size();
    HasIndirect =
        std::any_of(Block.Ops.begin(), Block.Ops.end(), [](const LowOp &Op) {
          return Op.Opcode == NdOp::INDIR_CALL;
        });
    if (HasIndirect)
      break;
  }
  if (!HasIndirect)
    return Result;
  for (const auto &[Address, Target] :
       immutableNativeCallTargets(Image, Function, &NativeCallees)) {
    const auto Callee = NativeCallees.find(Target.Target);
    if (Callee == NativeCallees.end())
      continue;
    SourceCallTypeHint Hint;
    Hint.ImmutableNativeCall = Target;
    Hint.TargetAddress = Target.Target;
    Hint.Signature = Callee->second;
    if (isImmutableNativeCallHint(Hint, Function.Entry, Image.Arch))
      Result.emplace(Address, std::move(Hint));
  }
  return Result;
}

bool isImmutableNativeSourceCall(const HighExpr &Expression, va_t FunctionEntry,
                                 Arch Architecture) {
  if (Expression.Kind != ExprKind::Call || !Expression.SourceCallHint ||
      !isImmutableNativeCallHint(*Expression.SourceCallHint, FunctionEntry,
                                 Architecture) ||
      Expression.IsIndirectCall || Expression.IndirectTarget ||
      Expression.IndirectParamIdx != -1 || !Expression.CallTarget.empty() ||
      Expression.CallAddr != Expression.SourceCallHint->TargetAddress ||
      Expression.IntrinsicId != Intrinsic::None ||
      !Expression.IntrinsicOutputs.empty() ||
      Expression.MemoryOrdering != NdMemoryOrdering::None ||
      Expression.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  const auto &ABI = Expression.SourceCallHint->Signature;
  if (!scalarCarrier(Expression.Type, true) ||
      Expression.Type->Size != ABI.ReturnType->Size ||
      (Expression.Type->Kind == NdTypeKind::Void) !=
          (ABI.ReturnType->Kind == NdTypeKind::Void) ||
      Expression.Operands.size() != ABI.Parameters.size())
    return false;
  for (size_t I = 0; I < Expression.Operands.size(); ++I)
    if (!Expression.Operands[I] ||
        !scalarCarrier(Expression.Operands[I]->Type) ||
        Expression.Operands[I]->Type->Size != ABI.Parameters[I].Type->Size)
      return false;
  return true;
}
} // namespace neverd

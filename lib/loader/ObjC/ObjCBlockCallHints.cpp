#include "neverd/loader/ObjC/ObjCBlockCallHints.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Limits.h"
#include "neverd/loader/MachO/DarwinImportVeneer.h"
#include "neverd/loader/ObjC/ObjCBlocks.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <optional>
#include <tuple>

namespace neverd {
namespace {
using Key = std::tuple<VnodeSpace, uint64_t, uint16_t>;
Key key(const NdVar &V) { return {V.Space, V.Offset, V.Size}; }
bool width(unsigned Size) {
  return Size == 1 || Size == 2 || Size == 4 || Size == 8;
}
bool scalar(const TypeRef &T) {
  return T && width(T->Size) &&
         (T->Kind == NdTypeKind::Int ||
          (T->Kind == NdTypeKind::Ptr && T->Size == 8));
}
bool writablePointerSlot(const BinaryImage &Image, va_t Address) {
  // A zero-filled writable symbol supplies storage identity, not a constant
  // pointer value. The invoke proof below must still use this exact load.
  if (!Address || Address % 8 || Address > InvalidVA - 8 ||
      Image.MachOChainedFixupsAmbiguous || Image.DyldBindSlots.count(Address) ||
      Image.MachOResolvedChainedPointerSlots.count(Address))
    return false;
  const auto *Section = Image.getSectionFor(Address);
  const auto *Segment = Image.getSegmentFor(Address);
  const auto *Bytes = Image.readVA(Address, 8);
  if (!Section || !Segment || !Bytes ||
      Image.getSectionFor(Address + 7) != Section ||
      Image.getSegmentFor(Address + 7) != Segment || !Section->isReadable() ||
      !Section->isWritable() || Section->isExecutable() ||
      !Segment->isReadable() || !Segment->isWritable() ||
      Segment->isExecutable() ||
      !std::all_of(Bytes, Bytes + 8, [](uint8_t Byte) { return Byte == 0; }))
    return false;
  const Symbol *Slot = nullptr;
  for (const auto &Symbol : Image.Symbols) {
    if (Symbol.Addr > Address && Symbol.Addr < Address + 8)
      return false;
    if (Symbol.Addr != Address)
      continue;
    if (Slot || Symbol.IsFunc || Symbol.Name.empty() ||
        (Symbol.Size && Symbol.Size < 8))
      return false;
    Slot = &Symbol;
  }
  return Slot != nullptr;
}
struct Value {
  enum class Kind { Scalar, CallInteger, Frame, Number, ImageBits, Invoke };
  Kind K = Kind::Scalar;
  // A base identifies one original incoming value; copies preserve identity.
  uint64_t Base = 0;
  int64_t Offset = 0;
  TypeRef Type;
  bool NumericBase = false;
  std::optional<SourceFunctionTypeHint> BlockSignature;
};
bool sameBase(const Value &A, const Value &B) {
  return A.Base == B.Base && A.Offset == B.Offset &&
         A.NumericBase == B.NumericBase &&
         (A.K == B.K || A.K == Value::Kind::Invoke ||
          B.K == Value::Kind::Invoke);
}
bool overlaps(const NdVar &V, uint64_t Offset, unsigned Size) {
  return V.isReg() && V.Offset < Offset + Size && Offset < V.Offset + V.Size;
}
bool branchTerminator(NdOp Opcode) {
  return Opcode == NdOp::BRANCH || Opcode == NdOp::COND_BR;
}
bool sameScalarType(const TypeRef &A, const TypeRef &B) {
  return A && B && A->Kind == B->Kind && A->Size == B->Size &&
         A->IsSigned == B->IsSigned &&
         (A->Kind == NdTypeKind::Void || A->Kind == NdTypeKind::Int ||
          A->Kind == NdTypeKind::Ptr);
}
bool sameLocation(const SourceABIValueLocation &A,
                  const SourceABIValueLocation &B) {
  return A.Kind == B.Kind && A.RegisterOffset == B.RegisterOffset &&
         A.EntryStackOffset == B.EntryStackOffset &&
         A.ValueBytes == B.ValueBytes && A.ExtendTo32Bits == B.ExtendTo32Bits;
}
bool mergeInferredBlockABI(SourceCallTypeHint &A, const SourceCallTypeHint &B,
                           std::set<size_t> &AZeroArguments,
                           const std::set<size_t> &BZeroArguments) {
  if (A.CallKind != SourceCallTypeHint::Kind::BlockInvoke ||
      B.CallKind != A.CallKind ||
      (A.Signature.Origin !=
           SourceFunctionTypeHint::OriginKind::NativeAnalysis &&
       A.Signature.Origin !=
           SourceFunctionTypeHint::OriginKind::BlockRuntime) ||
      B.Signature.Origin != A.Signature.Origin ||
      A.Signature.Convention != B.Signature.Convention ||
      A.Signature.Architecture != B.Signature.Architecture ||
      !A.Signature.HasExplicitABI || !B.Signature.HasExplicitABI ||
      !sameScalarType(A.Signature.ReturnType, B.Signature.ReturnType) ||
      !sameLocation(A.Signature.ReturnLocation, B.Signature.ReturnLocation) ||
      !A.Signature.ReturnComponents.empty() ||
      !B.Signature.ReturnComponents.empty() ||
      A.Signature.Parameters.size() != B.Signature.Parameters.size())
    return false;
  for (size_t I = 0; I < A.Signature.Parameters.size(); ++I) {
    auto &Left = A.Signature.Parameters[I];
    const auto &Right = B.Signature.Parameters[I];
    if (Left.Name != Right.Name || Left.TheRole != Right.TheRole ||
        Left.IndirectByValue || Right.IndirectByValue ||
        !sameLocation(Left.Location, Right.Location) ||
        !Left.Components.empty() || !Right.Components.empty())
      return false;
    if (sameScalarType(Left.Type, Right.Type))
      continue;
    // An exact zero is representable in either signedness at the same integer
    // width and ABI location. Retain the other path's observed type; arbitrary
    // signed/unsigned values and different widths still cannot establish one
    // callback projection. This never asserts an original block declaration.
    if (I && Left.Type && Right.Type && Left.Type->Kind == NdTypeKind::Int &&
        Right.Type->Kind == NdTypeKind::Int &&
        Left.Type->Size == Right.Type->Size &&
        (AZeroArguments.count(I) || BZeroArguments.count(I))) {
      if (AZeroArguments.count(I) && BZeroArguments.count(I))
        Left.Type = NdType::makeInt(Left.Type->Size, false);
      else if (AZeroArguments.count(I))
        Left.Type = Right.Type;
      continue;
    }
    // A literal zero is a null pointer at the same Darwin integer carrier.
    // This only joins an already proven pointer path with an exact zero path.
    if (I == 0 || !Left.Type || !Right.Type || Left.Type->Size != 8 ||
        Right.Type->Size != 8 ||
        Left.Location.Kind != SourceABICarrierKind::IntegerRegister)
      return false;
    if (Left.Type->Kind == NdTypeKind::Ptr &&
        Right.Type->Kind == NdTypeKind::Int && !Right.Type->IsSigned &&
        BZeroArguments.count(I))
      continue;
    if (Left.Type->Kind == NdTypeKind::Int && !Left.Type->IsSigned &&
        AZeroArguments.count(I) && Right.Type->Kind == NdTypeKind::Ptr) {
      Left.Type = Right.Type;
      continue;
    }
    return false;
  }
  std::string Error;
  if (!validateSourceABI(A.Signature, Error))
    return false;
  // A common zero must be zero on every path already merged. Retaining only
  // the first path's zero would authorize an unrelated later type conflict.
  for (auto It = AZeroArguments.begin(); It != AZeroArguments.end();)
    if (!BZeroArguments.count(*It))
      It = AZeroArguments.erase(It);
    else
      ++It;
  return true;
}
bool noIndirectResultPointer(const LowOp &Op, Arch Architecture) {
  // Darwin arm64 passes an indirect aggregate result in X8. The observed
  // invoke target in X8 rules that convention out for this call site.
  return Architecture == Arch::AArch64 && Op.NumInputs == 1 &&
         Op.Inputs[0].isReg() && Op.Inputs[0].Offset == a64reg::X8 &&
         Op.Inputs[0].Size == 8;
}
bool provenIndirectTailCall(const LowFunc &Function, const LowOp &Call) {
  if (Call.Opcode != NdOp::INDIR_CALL || Call.NumInputs != 1 ||
      !Call.Output.isReg())
    return false;
  // Path replay concatenates operations without their physical instruction
  // boundaries. The original block remains the authoritative tail-call proof;
  // neither the replayed path nor a same-address duplicate can supply it.
  const LowBlock *PhysicalBlock = nullptr;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::INDIR_CALL && Op.Addr == Call.Addr) {
        if (PhysicalBlock)
          return false;
        PhysicalBlock = &Block;
      }
  if (!PhysicalBlock)
    return false;
  const auto &Block = *PhysicalBlock;
  bool Found = false;
  for (const auto &Boundary : Block.InstructionBoundaries) {
    if (Boundary.Address != Call.Addr)
      continue;
    if (Found || Boundary.Control != LowInstructionControl::TailCall ||
        !hasLowInstructionControlFlag(Boundary.ControlFlags,
                                      LowInstructionControlFlag::Indirect) ||
        !Block.Succs.empty() || !Block.ExceptionalSuccs.empty() ||
        Boundary.FirstOp > Block.Ops.size() ||
        Boundary.OpCount > Block.Ops.size() - Boundary.FirstOp)
      return false;
    bool SameCall = false, Return = false;
    for (size_t I = Boundary.FirstOp; I < Boundary.FirstOp + Boundary.OpCount;
         ++I) {
      const auto &Op = Block.Ops[I];
      if (Op.Opcode == NdOp::INDIR_CALL)
        SameCall |= Op.Addr == Call.Addr && Op.Output == Call.Output &&
                    Op.NumInputs == 1 && Op.Inputs[0] == Call.Inputs[0];
      if (Op.Opcode == NdOp::RETURN)
        Return |= Op.Addr == Call.Addr && Op.NumInputs == 1 &&
                  Op.Inputs[0] == Call.Output;
    }
    if (!SameCall || !Return)
      return false;
    Found = true;
  }
  return Found;
}
std::optional<uint64_t>
boundReleaseRegister(const std::map<va_t, SourceCallTypeHint> *BoundCalls,
                     va_t Address, Arch Architecture) {
  if (!BoundCalls)
    return std::nullopt;
  const auto It = BoundCalls->find(Address);
  if (It == BoundCalls->end())
    return std::nullopt;
  const auto &Hint = It->second;
  std::string Error;
  if (Hint.CallKind != SourceCallTypeHint::Kind::ObjCRuntimeCall ||
      Hint.TargetName != "objc_release" ||
      Hint.Signature.Architecture != Architecture ||
      !Hint.Signature.HasExplicitABI ||
      !validateSourceABI(Hint.Signature, Error) || !Hint.Signature.ReturnType ||
      Hint.Signature.ReturnType->Kind != NdTypeKind::Void ||
      Hint.Signature.Parameters.size() != 1 ||
      !Hint.Signature.Parameters[0].Type ||
      Hint.Signature.Parameters[0].Type->Kind != NdTypeKind::Ptr ||
      Hint.Signature.Parameters[0].Location.Kind !=
          SourceABICarrierKind::IntegerRegister ||
      Hint.Signature.Parameters[0].Location.ValueBytes != 8)
    return std::nullopt;
  return Hint.Signature.Parameters[0].Location.RegisterOffset;
}
bool boundReleaseCall(const std::map<va_t, SourceCallTypeHint> *BoundCalls,
                      va_t Address, Arch Architecture,
                      const TargetRegInfo &TRI) {
  return boundReleaseRegister(BoundCalls, Address, Architecture) ==
         TRI.IntParamRegs[0];
}
bool boundReleaseAwayFromResult(
    const std::map<va_t, SourceCallTypeHint> *BoundCalls, va_t Address,
    Arch Architecture, const TargetRegInfo &TRI) {
  const auto Register = boundReleaseRegister(BoundCalls, Address, Architecture);
  return Architecture == Arch::AArch64 && Register &&
         TRI.IntReturnRegs.size() >= 2 && *Register != TRI.IntReturnRegs[0] &&
         *Register != TRI.IntReturnRegs[1];
}
bool boundVoidObjCMessage(const std::map<va_t, SourceCallTypeHint> *BoundCalls,
                          va_t Address, Arch Architecture) {
  if (Architecture != Arch::AArch64 || !BoundCalls)
    return false;
  const auto It = BoundCalls->find(Address);
  if (It == BoundCalls->end())
    return false;
  const auto &Hint = It->second;
  std::string Error;
  if (Hint.CallKind != SourceCallTypeHint::Kind::ObjCMessage ||
      Hint.Signature.Architecture != Architecture ||
      Hint.Signature.Convention != SourceFunctionTypeHint::ConventionKind::C ||
      !Hint.Signature.HasExplicitABI ||
      !validateSourceABI(Hint.Signature, Error) || !Hint.Signature.ReturnType ||
      Hint.Signature.ReturnType->Kind != NdTypeKind::Void ||
      !Hint.Signature.ReturnComponents.empty())
    return false;
  for (const auto &Parameter : Hint.Signature.Parameters)
    if (!Parameter.Type || !scalar(Parameter.Type) ||
        Parameter.Location.Kind != SourceABICarrierKind::IntegerRegister ||
        !Parameter.Components.empty())
      return false;
  return true;
}

bool boundIntegerResultObjCMessage(
    const std::map<va_t, SourceCallTypeHint> *BoundCalls, va_t Address,
    Arch Architecture, const TargetRegInfo &TRI) {
  if (Architecture != Arch::AArch64 || !BoundCalls)
    return false;
  const auto It = BoundCalls->find(Address);
  if (It == BoundCalls->end())
    return false;
  const auto &Hint = It->second;
  std::string Error;
  if (Hint.CallKind != SourceCallTypeHint::Kind::ObjCMessage ||
      Hint.Signature.Architecture != Architecture ||
      Hint.Signature.Convention != SourceFunctionTypeHint::ConventionKind::C ||
      !Hint.Signature.HasExplicitABI ||
      !validateSourceABI(Hint.Signature, Error) ||
      !scalar(Hint.Signature.ReturnType) ||
      !Hint.Signature.ReturnComponents.empty() ||
      Hint.Signature.ReturnLocation.Kind !=
          SourceABICarrierKind::IntegerRegister ||
      Hint.Signature.ReturnLocation.RegisterOffset != TRI.IntReturnReg)
    return false;
  for (const auto &Parameter : Hint.Signature.Parameters)
    if (!Parameter.Type || !scalar(Parameter.Type) ||
        Parameter.Location.Kind != SourceABICarrierKind::IntegerRegister ||
        !Parameter.Components.empty())
      return false;
  return true;
}

const SourceCallTypeHint *
validatedDarwinCall(const std::map<va_t, SourceCallTypeHint> *BoundCalls,
                    va_t Address, Arch Architecture) {
  if (!BoundCalls)
    return nullptr;
  const auto It = BoundCalls->find(Address);
  if (It == BoundCalls->end())
    return nullptr;
  const auto Kind = It->second.CallKind;
  if (Kind != SourceCallTypeHint::Kind::ObjCMessage &&
      Kind != SourceCallTypeHint::Kind::ObjCSuper2 &&
      Kind != SourceCallTypeHint::Kind::ObjCRuntimeCall &&
      Kind != SourceCallTypeHint::Kind::DarwinRuntimeCall)
    return nullptr;
  std::string Error;
  return It->second.Signature.Architecture == Architecture &&
                 validateSourceABI(It->second.Signature, Error)
             ? &It->second
             : nullptr;
}

bool boundRetainBlock(const BinaryImage &Image,
                      const SourceCallTypeHint &Bound) {
  if (Bound.CallKind != SourceCallTypeHint::Kind::ObjCRuntimeCall ||
      Bound.TargetName != "objc_retainBlock")
    return false;
  const auto Bind = Image.DyldBindSlots.find(Bound.TargetAddress);
  if (Bind == Image.DyldBindSlots.end() ||
      Bind->second.Module != "/usr/lib/libobjc.A.dylib")
    return false;
  const auto Runtime = objcRuntimeSourceCallHint(Image, Bound.TargetAddress);
  return Runtime && Runtime->CallKind == Bound.CallKind &&
         Runtime->TargetName == Bound.TargetName;
}

bool pointerAutoreleaseReturnTailcall(
    const BinaryImage &Image, const LowOp &Op,
    const SourceFunctionTypeHint *EntrySignature, const TargetRegInfo &TRI) {
  if (Image.Arch != Arch::AArch64 || Op.Opcode != NdOp::CALL ||
      Op.NumInputs != 1 || !Op.Inputs[0].isConst() || !Op.Output.isReg() ||
      Op.Output.Offset != TRI.IntReturnReg || Op.Output.Size != 8 ||
      !EntrySignature || !EntrySignature->HasExplicitABI ||
      !EntrySignature->ReturnType ||
      EntrySignature->ReturnType->Kind != NdTypeKind::Ptr ||
      EntrySignature->ReturnLocation.Kind !=
          SourceABICarrierKind::IntegerRegister ||
      EntrySignature->ReturnLocation.RegisterOffset != TRI.IntReturnReg ||
      EntrySignature->ReturnLocation.ValueBytes != 8)
    return false;
  const auto Slot = darwinImportVeneerSlot(Image, Op.Inputs[0].Offset);
  if (!Slot)
    return false;
  const auto Bind = Image.DyldBindSlots.find(*Slot);
  if (Bind == Image.DyldBindSlots.end() ||
      Bind->second.Module != "/usr/lib/libobjc.A.dylib" ||
      std::find(Image.DynInfo.NeededLibs.begin(),
                Image.DynInfo.NeededLibs.end(),
                Bind->second.Module) == Image.DynInfo.NeededLibs.end())
    return false;
  const auto Runtime = objcRuntimeSourceCallHint(Image, *Slot);
  std::string Error;
  if (!Runtime ||
      Runtime->CallKind != SourceCallTypeHint::Kind::ObjCRuntimeCall ||
      Runtime->TargetName != "objc_autoreleaseReturnValue" ||
      !validateSourceABI(Runtime->Signature, Error) ||
      !Runtime->Signature.ReturnType ||
      Runtime->Signature.ReturnType->Kind != NdTypeKind::Ptr ||
      Runtime->Signature.ReturnLocation.Kind !=
          SourceABICarrierKind::IntegerRegister ||
      Runtime->Signature.ReturnLocation.RegisterOffset != TRI.IntReturnReg ||
      Runtime->Signature.Parameters.size() != 1 ||
      !Runtime->Signature.Parameters[0].Type ||
      Runtime->Signature.Parameters[0].Type->Kind != NdTypeKind::Ptr ||
      Runtime->Signature.Parameters[0].Location.Kind !=
          SourceABICarrierKind::IntegerRegister ||
      Runtime->Signature.Parameters[0].Location.RegisterOffset !=
          TRI.IntParamRegs[0])
    return false;
  return true;
}

std::optional<SourceFunctionTypeHint>
stackBlockSignature(const BinaryImage &Image,
                    const std::map<std::pair<int64_t, unsigned>, Value> &Slots,
                    const Value &Argument) {
  // The descriptor provides only a call ABI here. Source projection proves
  // the stack isa, capture ownership, and consumer lifetime independently.
  if (Argument.K != Value::Kind::Frame ||
      Argument.Offset < -::neverd::limits::ir::kMaxSourceFrameDisplacement ||
      Argument.Offset > ::neverd::limits::ir::kMaxSourceFrameDisplacement)
    return std::nullopt;
  const auto Field = [&](int64_t Offset) -> const Value * {
    const auto It = Slots.find({Argument.Offset + Offset, 8});
    return It == Slots.end() ? nullptr : &It->second;
  };
  const auto *Flags = Field(8), *Invoke = Field(16), *Descriptor = Field(24);
  if (!Flags || !Invoke || !Descriptor || Invoke->K != Value::Kind::Number ||
      !Image.isCodeAddress(uint64_t(Invoke->Base + Invoke->Offset)) ||
      Descriptor->K != Value::Kind::Number)
    return std::nullopt;
  std::optional<uint64_t> FlagBits;
  if (Flags->K == Value::Kind::Number)
    FlagBits = uint64_t(Flags->Base + Flags->Offset);
  else if (Flags->K == Value::Kind::ImageBits && !Flags->Offset) {
    if (auto Bytes = readImmutableImageBytes(Image, Flags->Base, 8))
      FlagBits = llvm::support::endian::read64le(Bytes->data());
  }
  if (!FlagBits || *FlagBits >> 32)
    return std::nullopt;
  std::string Error;
  auto Block = readObjCBlockDescriptor(
      Image, uint64_t(Descriptor->Base + Descriptor->Offset),
      static_cast<uint32_t>(*FlagBits), Error);
  if (!Block || !Block->InvokeTypeHint || !Block->Limitations.empty())
    return std::nullopt;
  return Block->InvokeTypeHint;
}

std::optional<unsigned>
resultWidth(const LowBlock &Block, size_t CallIndex, const TargetRegInfo &TRI,
            Arch Architecture,
            const std::map<va_t, SourceCallTypeHint> *BoundCalls) {
  std::optional<unsigned> Width;
  bool IntegerLive = true;
  bool SecondIntegerLive =
      Architecture == Arch::AArch64 && TRI.IntReturnRegs.size() >= 2;
  bool FloatingLive = true;
  for (size_t I = CallIndex + 1; I < Block.Ops.size(); ++I) {
    const auto &Op = Block.Ops[I];
    if (branchTerminator(Op.Opcode) || Op.Opcode == NdOp::INDIR_BR ||
        Op.Opcode == NdOp::INTRINSIC)
      return Width;
    if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
      // objc_release has one pointer argument and no result. Once the block
      // return GPR has already been overwritten, this call also clobbers the
      // volatile FP return bank. The preceding invoke result is unobserved,
      // so its source projection can use a void block prototype.
      if (!Width && !IntegerLive && Op.Opcode == NdOp::CALL &&
          boundReleaseCall(BoundCalls, Op.Addr, Architecture, TRI))
        return 0U;
      // This exact release reads another register. Keep both return banks
      // live until a later overwrite proves the block result unobserved.
      if (Op.Opcode == NdOp::CALL &&
          boundReleaseAwayFromResult(BoundCalls, Op.Addr, Architecture, TRI))
        continue;
      if (IntegerLive && BoundCalls) {
        const auto It = BoundCalls->find(Op.Addr);
        if (It != BoundCalls->end()) {
          const auto &Hint = It->second;
          std::string Error;
          if (Hint.CallKind == SourceCallTypeHint::Kind::ObjCRuntimeCall &&
              Hint.TargetName == "objc_retainAutoreleasedReturnValue" &&
              Hint.Signature.Architecture == Architecture &&
              Hint.Signature.HasExplicitABI &&
              validateSourceABI(Hint.Signature, Error) &&
              Hint.Signature.Parameters.size() == 1 &&
              Hint.Signature.Parameters[0].Type &&
              Hint.Signature.Parameters[0].Type->Kind == NdTypeKind::Ptr &&
              Hint.Signature.Parameters[0].Location.Kind ==
                  SourceABICarrierKind::IntegerRegister &&
              Hint.Signature.Parameters[0].Location.RegisterOffset ==
                  TRI.IntReturnReg &&
              Hint.Signature.Parameters[0].Location.ValueBytes == 8)
            return std::max(Width.value_or(0), 8U);
        }
      }
      if (!Width && !IntegerLive && Op.Opcode == NdOp::CALL &&
          (boundVoidObjCMessage(BoundCalls, Op.Addr, Architecture) ||
           boundIntegerResultObjCMessage(BoundCalls, Op.Addr, Architecture,
                                         TRI)))
        return 0U;
      return Width;
    }
    for (unsigned J = 0; J < Op.NumInputs; ++J) {
      const auto &V = Op.Inputs[J];
      if (FloatingLive && overlaps(V, TRI.FPReturnReg, 16))
        return std::nullopt;
      if (SecondIntegerLive && overlaps(V, TRI.IntReturnRegs[1], 8))
        return std::nullopt;
      if (IntegerLive && V.isReg() && V.Offset == TRI.IntReturnReg &&
          width(V.Size))
        Width = std::max(Width.value_or(0), unsigned(V.Size));
    }
    if (overlaps(Op.Output, TRI.IntReturnReg, 8))
      IntegerLive = false;
    if (SecondIntegerLive && overlaps(Op.Output, TRI.IntReturnRegs[1], 8))
      SecondIntegerLive = false;
    if (overlaps(Op.Output, TRI.FPReturnReg, 16))
      FloatingLive = false;
    if (Op.Opcode == NdOp::RETURN) {
      // AArch64 RET names LR, not the returned scalar. A call immediately
      // forwarded through an otherwise untouched return bank retains the full
      // X0 machine carrier. This does not assert an original 64-bit prototype.
      return Width ? Width
             : Architecture == Arch::AArch64 && IntegerLive
                 ? std::optional<unsigned>(8)
                 : std::nullopt;
    }
  }
  return Width;
}

bool discardedResultAcrossCFG(
    const BinaryImage &Image, const LowFunc &Function, va_t CallAddress,
    const TargetRegInfo &TRI,
    const std::map<va_t, SourceCallTypeHint> *BoundCalls,
    const SourceFunctionTypeHint *EntrySignature) {
  if (Image.Arch != Arch::AArch64 || !BoundCalls ||
      TRI.IntReturnRegs.size() < 2 || TRI.FPReturnRegs.empty() ||
      Function.Blocks.size() >
          ::neverd::limits::loader::
              kObjCBlockCallHintsDiscardedResultAcrossCFGFunctionBlocksSizeLimit)
    return false;
  std::string EntryError;
  const bool VoidBlockEntry =
      EntrySignature &&
      EntrySignature->Origin ==
          SourceFunctionTypeHint::OriginKind::BlockRuntime &&
      EntrySignature->HasExplicitABI && EntrySignature->ReturnType &&
      EntrySignature->ReturnType->Kind == NdTypeKind::Void &&
      validateSourceABI(*EntrySignature, EntryError);
  std::map<int, const LowBlock *> ById;
  const LowBlock *CallBlock = nullptr;
  size_t CallIndex = 0;
  for (const auto &Block : Function.Blocks) {
    if (Block.Id < 0 || !ById.emplace(Block.Id, &Block).second)
      return false;
    for (size_t I = 0; I < Block.Ops.size(); ++I)
      if (Block.Ops[I].Opcode == NdOp::INDIR_CALL &&
          Block.Ops[I].Addr == CallAddress) {
        if (CallBlock)
          return false;
        CallBlock = &Block;
        CallIndex = I;
      }
  }
  if (!CallBlock)
    return false;

  struct Cursor {
    const LowBlock *Block;
    size_t Index;
    bool Integer0Live;
    bool Integer1Live;
    std::set<int> Visited;
  };
  std::vector<Cursor> Work{
      {CallBlock, CallIndex + 1, true, true, {CallBlock->Id}}};
  size_t Budget = ::neverd::limits::loader::
      kObjCBlockCallHintsDiscardedResultAcrossCFGBudget;
  while (!Work.empty()) {
    auto State = std::move(Work.back());
    Work.pop_back();
    bool Released = false;
    for (size_t I = State.Index; I < State.Block->Ops.size(); ++I) {
      if (!Budget--)
        return false;
      const auto &Op = State.Block->Ops[I];
      if (Op.Opcode == NdOp::INTRINSIC || Op.Opcode == NdOp::INDIR_BR)
        return false;
      if (Op.Opcode == NdOp::RETURN) {
        // An AArch64 return can carry the stale X0 machine value even when a
        // descriptor proves this enclosing block has no source result. The
        // preceding path still must not have read any invoke-result carrier.
        if (!VoidBlockEntry || Op.NumInputs > 1 ||
            (Op.NumInputs == 1 && (!Op.Inputs[0].isReg() ||
                                   !((Op.Inputs[0].Offset == TRI.IntReturnReg &&
                                      width(Op.Inputs[0].Size)) ||
                                     (Op.Inputs[0].Offset == TRI.LinkRegister &&
                                      Op.Inputs[0].Size == 8)))))
          return false;
        Released = true;
        break;
      }
      for (unsigned J = 0; J < Op.NumInputs; ++J) {
        const auto &Input = Op.Inputs[J];
        if ((State.Integer0Live && overlaps(Input, TRI.IntReturnReg, 8)) ||
            (State.Integer1Live && overlaps(Input, TRI.IntReturnRegs[1], 8)))
          return false;
        for (const auto FPRegister : TRI.FPReturnRegs)
          if (overlaps(Input, FPRegister, 16))
            return false;
      }
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        if (Op.Opcode == NdOp::CALL && State.Block->Succs.empty() &&
            State.Block->ExceptionalSuccs.empty() &&
            I + 1 == State.Block->Ops.size() && Op.NumInputs == 1 &&
            Op.Inputs[0].isConst() &&
            libc::isNoReturnTarget(Image, Op.Inputs[0].Offset)) {
          const auto *Bound =
              validatedDarwinCall(BoundCalls, Op.Addr, Image.Arch);
          if (Bound && Bound->DoesNotReturn &&
              libc::isNoReturnFunction(Bound->TargetName) &&
              Bound->Signature.HasExplicitABI && Bound->Signature.ReturnType &&
              Bound->Signature.ReturnType->Kind == NdTypeKind::Void &&
              Bound->Signature.Parameters.empty()) {
            // A no-argument terminating call cannot observe the block result.
            Released = true;
            break;
          }
        }
        if (!State.Integer0Live && State.Block->Succs.empty() &&
            I + 2 == State.Block->Ops.size() &&
            State.Block->Ops[I + 1].Opcode == NdOp::RETURN &&
            pointerAutoreleaseReturnTailcall(Image, Op, EntrySignature, TRI)) {
          // The block result is neither this pointer-only ARC call's argument
          // nor the containing method's returned pointer.
          Released = true;
          break;
        }
        if (Op.Opcode == NdOp::CALL &&
            boundReleaseAwayFromResult(BoundCalls, Op.Addr, Image.Arch, TRI))
          continue;
        if (VoidBlockEntry && Op.Opcode == NdOp::CALL && !State.Integer1Live &&
            boundReleaseRegister(BoundCalls, Op.Addr, Image.Arch) ==
                TRI.IntReturnRegs[1])
          // X1 was overwritten after the invoke. An authenticated release_x1
          // therefore consumes the new object, not a second result carrier.
          continue;
        if (Op.Opcode == NdOp::CALL && !State.Integer0Live &&
            (boundVoidObjCMessage(BoundCalls, Op.Addr, Image.Arch) ||
             boundIntegerResultObjCMessage(BoundCalls, Op.Addr, Image.Arch,
                                           TRI))) {
          Released = true;
          break;
        }
        if (Op.Opcode != NdOp::CALL || State.Integer0Live ||
            !boundReleaseCall(BoundCalls, Op.Addr, Image.Arch, TRI))
          return false;
        Released = true;
        break;
      }
      auto Kills = [&](uint64_t Register) {
        return Op.Output.isReg() && Op.Output.Offset == Register &&
               (Op.Output.Size >= 8 ||
                (Op.Output.Size == 4 &&
                 TRI.writeZeroExtends(Op.Output.Offset, Op.Output.Size)));
      };
      State.Integer0Live &= !Kills(TRI.IntReturnReg);
      State.Integer1Live &= !Kills(TRI.IntReturnRegs[1]);
      if (branchTerminator(Op.Opcode)) {
        if (I + 1 != State.Block->Ops.size())
          return false;
        break;
      }
    }
    if (Released)
      continue;
    if (State.Block->Succs.empty())
      return false;
    for (const auto SuccessorId : State.Block->Succs) {
      const auto It = ById.find(SuccessorId);
      if (It == ById.end() || State.Visited.count(SuccessorId) ||
          !It->second->ExceptionalPreds.empty())
        return false;
      bool HasPredecessor = false;
      for (const auto PredecessorId : It->second->Preds)
        HasPredecessor |= PredecessorId == State.Block->Id;
      if (!HasPredecessor)
        return false;
      auto Next = State;
      Next.Block = It->second;
      Next.Index = 0;
      Next.Visited.insert(SuccessorId);
      Work.push_back(std::move(Next));
      if (Work.size() >
          ::neverd::limits::loader::
              kObjCBlockCallHintsDiscardedResultAcrossCFGWorkSizeLimit)
        return false;
    }
  }
  return true;
}

std::vector<uint64_t> preservedEntryContextRegisters(
    const LowFunc &Function, const LowBlock &Entry, const LowBlock &Candidate,
    const TargetRegInfo &TRI,
    const std::map<va_t, SourceCallTypeHint> *BoundCalls, Arch Architecture) {
  if (!BoundCalls ||
      Function.Blocks.size() >
          ::neverd::limits::loader::
              kObjCBlockCallHintsPreservedEntryContextRegistersFunctionBlocksSizeLimit ||
      !Entry.Preds.empty() || !Entry.ExceptionalPreds.empty() ||
      !Candidate.ExceptionalPreds.empty() || &Entry == &Candidate)
    return {};
  std::map<int, const LowBlock *> ById;
  for (const auto &Block : Function.Blocks)
    if (Block.Id < 0 || !ById.emplace(Block.Id, &Block).second)
      return {};
  std::set<int> Ancestors{Candidate.Id};
  std::vector<int> Pending{Candidate.Id};
  while (!Pending.empty()) {
    const auto Id = Pending.back();
    Pending.pop_back();
    const auto *Block = ById.at(Id);
    if (Block != &Entry && Block->Preds.empty())
      return {};
    if (!Block->ExceptionalPreds.empty())
      return {};
    for (const auto ParentId : Block->Preds) {
      const auto Parent = ById.find(ParentId);
      if (Parent == ById.end() ||
          std::find(Parent->second->Succs.begin(), Parent->second->Succs.end(),
                    Id) == Parent->second->Succs.end())
        return {};
      if (Ancestors.insert(ParentId).second)
        Pending.push_back(ParentId);
    }
  }
  if (!Ancestors.count(Entry.Id))
    return {};
  std::set<int> Reached{Entry.Id};
  Pending = {Entry.Id};
  while (!Pending.empty()) {
    const auto Id = Pending.back();
    Pending.pop_back();
    for (const auto NextId : ById.at(Id)->Succs)
      if (Ancestors.count(NextId) && Reached.insert(NextId).second)
        Pending.push_back(NextId);
  }
  if (Reached != Ancestors)
    return {};
  // The candidate must not run a second time with its context register
  // already overwritten by its first invocation.
  std::set<int> AfterCandidate;
  Pending = Candidate.Succs;
  while (!Pending.empty()) {
    const auto Id = Pending.back();
    Pending.pop_back();
    if (Id == Candidate.Id)
      return {};
    const auto Found = ById.find(Id);
    if (Found == ById.end())
      return {};
    if (AfterCandidate.insert(Id).second)
      Pending.insert(Pending.end(), Found->second->Succs.begin(),
                     Found->second->Succs.end());
  }

  std::vector<uint64_t> Result;
  for (size_t SeedIndex = 0; SeedIndex < Entry.Ops.size(); ++SeedIndex) {
    const auto &Seed = Entry.Ops[SeedIndex];
    if (Seed.Opcode != NdOp::COPY || !Seed.Output.isReg() ||
        Seed.Output.Size != 8 || Seed.NumInputs != 1 ||
        !Seed.Inputs[0].isReg() ||
        Seed.Inputs[0].Offset != TRI.IntParamRegs[0] ||
        Seed.Inputs[0].Size != 8 || Seed.Output.Offset < a64reg::X19 ||
        Seed.Output.Offset > a64reg::X28 ||
        (Seed.Output.Offset - a64reg::X19) % 8 != 0 ||
        !TRI.isCallPreserved(Seed.Output.Offset, 8))
      continue;
    bool Valid = true;
    for (size_t I = 0; I < SeedIndex; ++I) {
      const auto &Op = Entry.Ops[I];
      if (overlaps(Op.Output, TRI.IntParamRegs[0], 8) ||
          Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
          Op.Opcode == NdOp::INTRINSIC || Op.Opcode == NdOp::INDIR_BR ||
          Op.Opcode == NdOp::RETURN || branchTerminator(Op.Opcode))
        Valid = false;
    }
    if (!Valid)
      continue;
    for (const auto Id : Ancestors) {
      const auto *Block = ById.at(Id);
      if (Block == &Candidate)
        continue;
      const size_t Begin = Block == &Entry ? SeedIndex + 1 : 0;
      for (size_t I = Begin; I < Block->Ops.size(); ++I) {
        const auto &Op = Block->Ops[I];
        if (overlaps(Op.Output, Seed.Output.Offset, 8) ||
            Op.Opcode == NdOp::INTRINSIC || Op.Opcode == NdOp::INDIR_CALL ||
            Op.Opcode == NdOp::INDIR_BR || Op.Opcode == NdOp::RETURN ||
            (branchTerminator(Op.Opcode) && I + 1 != Block->Ops.size())) {
          Valid = false;
          break;
        }
        if (Op.Opcode == NdOp::CALL) {
          const auto *Bound =
              validatedDarwinCall(BoundCalls, Op.Addr, Architecture);
          if (!Bound || Bound->DoesNotReturn) {
            Valid = false;
            break;
          }
        }
      }
      if (!Valid)
        break;
    }
    if (Valid)
      Result.push_back(Seed.Output.Offset);
  }
  std::sort(Result.begin(), Result.end());
  Result.erase(std::unique(Result.begin(), Result.end()), Result.end());
  return Result;
}

bool clearedFloatingArgumentsBeforeCall(
    const LowBlock &Block, size_t CallIndex, const TargetRegInfo &TRI,
    const std::map<va_t, SourceCallTypeHint> *BoundCalls, Arch Architecture) {
  bool Cleared = false;
  for (size_t I = 0; I < CallIndex; ++I) {
    const auto &Op = Block.Ops[I];
    if (Op.Opcode == NdOp::CALL) {
      const auto *Bound =
          validatedDarwinCall(BoundCalls, Op.Addr, Architecture);
      Cleared = Bound && Bound->Signature.ReturnType &&
                (Bound->Signature.ReturnType->Kind == NdTypeKind::Void ||
                 Bound->Signature.ReturnType->Kind == NdTypeKind::Int ||
                 Bound->Signature.ReturnType->Kind == NdTypeKind::Ptr) &&
                Bound->Signature.ReturnComponents.empty() &&
                Bound->Signature.ReturnLocation.Kind !=
                    SourceABICarrierKind::FloatingRegister;
    } else if (Op.Opcode == NdOp::INDIR_CALL || Op.Opcode == NdOp::INTRINSIC)
      Cleared = false;
    for (const auto FPRegister : TRI.FPParamRegs)
      if (overlaps(Op.Output, FPRegister, 16))
        Cleared = false;
  }
  return Cleared;
}

std::map<va_t, SourceCallTypeHint>
analyzeBlock(const BinaryImage &Image, const LowBlock &Block,
             const SourceFunctionTypeHint *EntrySignature,
             const std::map<va_t, SourceCallTypeHint> *BoundCalls,
             const ObjCBlockCaptureCallFields *Captures,
             const LowFunc *WholeFunction, bool FollowValidatedBranches,
             std::map<va_t, std::set<size_t>> *ZeroArguments = nullptr,
             std::optional<uint64_t> CarriedEntryContext = std::nullopt) {
  std::map<va_t, SourceCallTypeHint> Result;
  if (Block.Ops.size() >
      ::neverd::limits::loader::kObjCBlockCallHintsAnalyzeBlockOpsSizeLimit)
    return Result;
  const auto &TRI = getTargetRegInfo(Image.Arch);
  std::string EntryError;
  if (EntrySignature && (EntrySignature->Architecture != Image.Arch ||
                         !validateSourceABI(*EntrySignature, EntryError)))
    return Result;
  const bool ProvenCaptureContext =
      Captures && EntrySignature &&
      EntrySignature->Origin ==
          SourceFunctionTypeHint::OriginKind::BlockRuntime &&
      !EntrySignature->Parameters.empty() &&
      EntrySignature->Parameters[0].Type &&
      EntrySignature->Parameters[0].Type->Kind == NdTypeKind::Ptr &&
      EntrySignature->Parameters[0].Location.Kind ==
          SourceABICarrierKind::IntegerRegister &&
      EntrySignature->Parameters[0].Location.RegisterOffset ==
          TRI.IntParamRegs[0] &&
      EntrySignature->Parameters[0].Location.ValueBytes == 8;
  std::map<Key, Value> Values;
  std::map<std::pair<int64_t, unsigned>, Value> FrameSlots;
  std::set<uint64_t> WrittenArguments;
  bool FloatingArgumentWrite = false;
  bool FrameEscaped = false;
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  Values.emplace(key(NdVar::reg(TRI.StackPointer, 8)),
                 Value{Value::Kind::Frame, 0, 0, Pointer});
  for (auto Register : TRI.IntParamRegs) {
    if (CarriedEntryContext)
      continue;
    TypeRef Type = NdType::makeInt(8, false);
    if (EntrySignature)
      for (const auto &P : EntrySignature->Parameters)
        if (P.Location.Kind == SourceABICarrierKind::IntegerRegister &&
            P.Location.RegisterOffset == Register && scalar(P.Type))
          Type = P.Type;
    Values.emplace(key(NdVar::reg(Register, 8)),
                   Value{Value::Kind::Scalar, Register + 1, 0, Type});
  }
  if (CarriedEntryContext && ProvenCaptureContext &&
      TRI.isCallPreserved(*CarriedEntryContext, 8))
    Values.emplace(key(NdVar::reg(*CarriedEntryContext, 8)),
                   Value{Value::Kind::Scalar, TRI.IntParamRegs[0] + 1, 0,
                         EntrySignature->Parameters[0].Type});
  auto Read = [&](const NdVar &V) -> std::optional<Value> {
    if (!width(V.Size))
      return std::nullopt;
    if (V.isConst())
      return Value{Value::Kind::Number, V.Offset, 0,
                   NdType::makeInt(V.Size, false), true};
    for (const auto &[K, Stored] : Values) {
      if (std::get<0>(K) != V.Space || std::get<1>(K) != V.Offset ||
          std::get<2>(K) < V.Size)
        continue;
      auto Value = Stored;
      if (V.Size < 8 && Value.K != Value::Kind::Scalar &&
          Value.K != Value::Kind::CallInteger && Value.K != Value::Kind::Number)
        return std::nullopt;
      if (Value.Type && V.Size < Value.Type->Size)
        Value.Type = NdType::makeInt(V.Size, false);
      return Value;
    }
    return std::nullopt;
  };
  auto AuthenticatedEntryArgument = [&](uint64_t Register,
                                        const Value &Argument) {
    // Only a single-block invoke proves an unchanged entry carrier along the
    // whole path to this call. Multi-block path replay does not yet preserve
    // that extra argument provenance across its control-flow alternatives.
    if (!WholeFunction || WholeFunction->Blocks.size() != 1 ||
        !EntrySignature ||
        EntrySignature->Origin !=
            SourceFunctionTypeHint::OriginKind::BlockRuntime ||
        Argument.K != Value::Kind::Scalar || Argument.Base != Register + 1 ||
        Argument.Offset != 0)
      return false;
    for (const auto &Parameter : EntrySignature->Parameters)
      if (Parameter.Location.Kind == SourceABICarrierKind::IntegerRegister &&
          Parameter.Location.RegisterOffset == Register &&
          sameScalarType(Parameter.Type, Argument.Type))
        return true;
    return false;
  };
  va_t PreviousAddress = InvalidVA;
  uint64_t NextCallResultBase = 1ULL << 32;
  for (size_t Index = 0; Index < Block.Ops.size(); ++Index) {
    const auto &Op = Block.Ops[Index];
    if (Op.Opcode == NdOp::INTRINSIC || Op.Opcode == NdOp::INDIR_BR)
      return Result;
    if (branchTerminator(Op.Opcode)) {
      if (!FollowValidatedBranches)
        return Result;
      for (auto It = Values.begin(); It != Values.end();)
        if (std::get<0>(It->first) == VnodeSpace::TEMP)
          It = Values.erase(It);
        else
          ++It;
      PreviousAddress = InvalidVA;
      continue;
    }
    if (Op.Addr != PreviousAddress) {
      for (auto It = Values.begin(); It != Values.end();)
        if (std::get<0>(It->first) == VnodeSpace::TEMP)
          It = Values.erase(It);
        else
          ++It;
      PreviousAddress = Op.Addr;
    }
    if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
      bool ProvenBlockCall = false;
      auto Target = Op.NumInputs == 1 ? Read(Op.Inputs[0]) : std::nullopt;
      auto Receiver = Read(NdVar::reg(TRI.IntParamRegs[0], 8));
      if (Op.Opcode == NdOp::INDIR_CALL && Target &&
          Target->K == Value::Kind::Invoke && Receiver &&
          Receiver->K == Value::Kind::Number) {
        std::string Error;
        const auto Address = Receiver->Base + uint64_t(Receiver->Offset);
        if (auto Literal = readObjCBlockLiteral(Image, Address, Error)) {
          Receiver->Base = Literal->Address;
          Receiver->Offset = 0;
          Receiver->BlockSignature = Literal->Descriptor.InvokeTypeHint;
        }
      }
      if (Op.Opcode == NdOp::INDIR_CALL && Target && Receiver &&
          Target->K == Value::Kind::Invoke && Receiver->Offset == 0 &&
          sameBase(*Target, *Receiver) && !FloatingArgumentWrite &&
          WrittenArguments.count(TRI.IntParamRegs[0])) {
        std::optional<SourceFunctionTypeHint> Signature;
        std::set<size_t> ZeroIndices;
        if (Receiver->BlockSignature)
          Signature = *Receiver->BlockSignature;
        if (Receiver->K == Value::Kind::Number)
          Signature = Receiver->BlockSignature;
        if (!Signature) {
          // CFGBuilder represents a register-indirect tail branch as a call
          // and a return at the same instruction address. The synthetic
          // return reads X0 even when this descriptor-backed invoke is void.
          // Only the authenticated entry ABI can discard that carrier.
          const bool VoidTail =
              Image.Arch == Arch::AArch64 && EntrySignature &&
              EntrySignature->Origin ==
                  SourceFunctionTypeHint::OriginKind::BlockRuntime &&
              EntrySignature->ReturnType &&
              EntrySignature->ReturnType->Kind == NdTypeKind::Void &&
              WholeFunction && provenIndirectTailCall(*WholeFunction, Op) &&
              Index + 2 == Block.Ops.size() &&
              Block.Ops[Index + 1].Opcode == NdOp::RETURN &&
              Block.Ops[Index + 1].Addr == Op.Addr && Op.Output.isReg() &&
              Op.Output.Offset == TRI.IntReturnReg && Op.Output.Size == 8 &&
              Block.Ops[Index + 1].NumInputs == 1 &&
              Block.Ops[Index + 1].Inputs[0].isReg() &&
              Block.Ops[Index + 1].Inputs[0].Offset == TRI.IntReturnReg;
          auto ReturnBytes =
              VoidTail ? std::optional<unsigned>(0)
                       : resultWidth(Block, Index, TRI, Image.Arch, BoundCalls);
          if (!ReturnBytes && WholeFunction &&
              discardedResultAcrossCFG(Image, *WholeFunction, Op.Addr, TRI,
                                       BoundCalls, EntrySignature))
            ReturnBytes = 0U;
          if (ReturnBytes && *ReturnBytes == 0 && !VoidTail &&
              !noIndirectResultPointer(Op, Image.Arch))
            ReturnBytes.reset();
          if (ReturnBytes) {
            SourceFunctionTypeHint Inferred;
            Inferred.Origin =
                SourceFunctionTypeHint::OriginKind::NativeAnalysis;
            Inferred.ReturnType = *ReturnBytes
                                      ? NdType::makeInt(*ReturnBytes, false)
                                      : NdType::makeVoid();
            bool Gap = false, Valid = true;
            for (size_t I = 0; I < TRI.IntParamRegs.size(); ++I) {
              const auto Reg = TRI.IntParamRegs[I];
              auto Argument = Read(NdVar::reg(Reg, 8));
              if (!WrittenArguments.count(Reg) &&
                  (!Argument || !AuthenticatedEntryArgument(Reg, *Argument))) {
                Gap = true;
                continue;
              }
              // The register holding the indirect branch target is not also
              // evidence for an explicit callback argument. A real later
              // argument would leave a carrier gap and remain unbound.
              if (I && Op.Inputs[0].isReg() && Op.Inputs[0].Offset == Reg &&
                  Argument && Argument->K == Value::Kind::Invoke) {
                Gap = true;
                continue;
              }
              if (Gap || !Argument || !scalar(Argument->Type) ||
                  Argument->K == Value::Kind::Invoke ||
                  (Argument->K == Value::Kind::Frame &&
                   (FrameEscaped ||
                    Argument->Offset <
                        -::neverd::limits::ir::kMaxSourceFrameDisplacement ||
                    Argument->Offset >
                        ::neverd::limits::ir::kMaxSourceFrameDisplacement))) {
                Valid = false;
                break;
              }
              Inferred.Parameters.push_back(
                  {I ? "arg" + std::to_string(I - 1) : "block",
                   I ? Argument->Type : Pointer});
              if (Argument->K == Value::Kind::Number && Argument->Base == 0 &&
                  Argument->Offset == 0)
                ZeroIndices.insert(I);
            }
            std::string Error;
            if (Valid &&
                assignDarwinScalarSourceABI(Inferred, Image.Arch, Error))
              Signature = std::move(Inferred);
          }
        }
        if (Signature && !Signature->Parameters.empty()) {
          bool Valid = true;
          for (const auto &P : Signature->Parameters) {
            if (P.Location.Kind != SourceABICarrierKind::IntegerRegister) {
              Valid = false;
              break;
            }
            auto Argument = Read(NdVar::reg(P.Location.RegisterOffset, 8));
            if (!WrittenArguments.count(P.Location.RegisterOffset) &&
                (!Argument || !AuthenticatedEntryArgument(
                                  P.Location.RegisterOffset, *Argument))) {
              Valid = false;
              break;
            }
            if (!Argument || !scalar(P.Type) || !Argument->Type ||
                Argument->Type->Size < P.Type->Size) {
              Valid = false;
              break;
            }
          }
          if (Valid) {
            SourceCallTypeHint Hint;
            Hint.CallKind = SourceCallTypeHint::Kind::BlockInvoke;
            Hint.Signature = std::move(*Signature);
            auto [It, Inserted] = Result.emplace(Op.Addr, std::move(Hint));
            if (Inserted) {
              std::string Error;
              ProvenBlockCall =
                  It->second.Signature.Architecture == Image.Arch &&
                  validateSourceABI(It->second.Signature, Error);
            }
            if (ZeroArguments)
              ZeroArguments->emplace(Op.Addr, std::move(ZeroIndices));
          }
        }
      }
      // A validated Darwin source ABI preserves the stack and nonvolatile
      // registers on the normal return edge. Every other identity and every
      // frame spill is invalidated. The ARC result is a fresh opaque pointer:
      // the later invoke-slot proof must still show the same block receiver.
      std::map<Key, Value> Preserved;
      const auto *Bound = validatedDarwinCall(BoundCalls, Op.Addr, Image.Arch);
      // A locally proven block invoke also uses the Darwin C calling
      // convention, so its callee-saved registers retain their identities.
      if (Bound || ProvenBlockCall)
        for (const auto &[K, V] : Values)
          if (std::get<0>(K) == VnodeSpace::REG && V.K != Value::Kind::Invoke &&
              (TRI.isCallPreserved(std::get<1>(K), std::get<2>(K)) ||
               (std::get<1>(K) == TRI.StackPointer &&
                V.K == Value::Kind::Frame)))
            Preserved.emplace(K, V);
      if (!Bound && Op.Opcode == NdOp::CALL && Target &&
          Target->K == Value::Kind::Number &&
          objcSelectorStubPreservesNonvolatileRegisters(
              Image, uint64_t(Target->Base + Target->Offset)))
        // Keep machine callee-save values without inventing a source ABI.
        for (const auto &[K, V] : Values)
          if (std::get<0>(K) == VnodeSpace::REG && V.K != Value::Kind::Invoke &&
              (TRI.isCallPreserved(std::get<1>(K), std::get<2>(K)) ||
               (std::get<1>(K) == TRI.StackPointer &&
                V.K == Value::Kind::Frame)))
            Preserved.emplace(K, V);
      std::optional<SourceFunctionTypeHint> CopiedBlockSignature;
      if (Op.Opcode == NdOp::CALL && Bound && boundRetainBlock(Image, *Bound) &&
          Receiver && !FrameEscaped)
        CopiedBlockSignature =
            stackBlockSignature(Image, FrameSlots, *Receiver);
      for (auto Register : TRI.IntParamRegs) {
        const auto Argument = Read(NdVar::reg(Register, 8));
        if (Argument && Argument->K == Value::Kind::Frame)
          FrameEscaped = true;
      }
      bool PrivateFrameSurvivesCall =
          Bound && !FrameEscaped &&
          Bound->Signature.Convention ==
              SourceFunctionTypeHint::ConventionKind::C &&
          Bound->Signature.ReturnLocation.Kind !=
              SourceABICarrierKind::IndirectResultPointer;
      if (PrivateFrameSurvivesCall)
        for (const auto &Parameter : Bound->Signature.Parameters)
          if (Parameter.Location.Kind == SourceABICarrierKind::Stack ||
              Parameter.Location.Kind ==
                  SourceABICarrierKind::IndirectResultPointer ||
              !Parameter.Components.empty())
            PrivateFrameSurvivesCall = false;
      Values = std::move(Preserved);
      if (!PrivateFrameSurvivesCall)
        FrameSlots.clear();
      WrittenArguments.clear();
      FloatingArgumentWrite = false;
      if (Op.Opcode == NdOp::CALL && Bound &&
          Bound->CallKind == SourceCallTypeHint::Kind::ObjCRuntimeCall &&
          (Bound->TargetName == "objc_retainAutoreleasedReturnValue" ||
           boundRetainBlock(Image, *Bound)) &&
          Bound->Signature.HasExplicitABI && Bound->Signature.ReturnType &&
          Bound->Signature.ReturnType->Kind == NdTypeKind::Ptr &&
          Bound->Signature.ReturnLocation.Kind ==
              SourceABICarrierKind::IntegerRegister &&
          Bound->Signature.ReturnLocation.RegisterOffset == TRI.IntReturnReg &&
          Bound->Signature.ReturnLocation.ValueBytes == 8 &&
          Bound->Signature.Parameters.size() == 1 &&
          Bound->Signature.Parameters[0].Type &&
          Bound->Signature.Parameters[0].Type->Kind == NdTypeKind::Ptr &&
          Bound->Signature.Parameters[0].Location.Kind ==
              SourceABICarrierKind::IntegerRegister &&
          Bound->Signature.Parameters[0].Location.RegisterOffset ==
              TRI.IntParamRegs[0] &&
          Bound->Signature.Parameters[0].Location.ValueBytes == 8 &&
          Op.Output.isReg() && Op.Output.Offset == TRI.IntReturnReg &&
          Op.Output.Size == 8) {
        Value Returned{Value::Kind::Scalar, NextCallResultBase++, 0, Pointer};
        Returned.BlockSignature = std::move(CopiedBlockSignature);
        Values.emplace(key(Op.Output), std::move(Returned));
        WrittenArguments.insert(TRI.IntParamRegs[0]);
      } else if (Op.Opcode == NdOp::CALL && Bound &&
                 Bound->CallKind == SourceCallTypeHint::Kind::ObjCMessage &&
                 Bound->Signature.HasExplicitABI &&
                 scalar(Bound->Signature.ReturnType) &&
                 Bound->Signature.ReturnType->Kind == NdTypeKind::Int &&
                 Bound->Signature.ReturnLocation.Kind ==
                     SourceABICarrierKind::IntegerRegister &&
                 Bound->Signature.ReturnLocation.RegisterOffset ==
                     TRI.IntReturnReg &&
                 Op.Output.isReg() && Op.Output.Offset == TRI.IntReturnReg &&
                 Op.Output.Size >= Bound->Signature.ReturnType->Size) {
        // An authenticated scalar message result can be a later block
        // argument. Keep its value identity, but never treat this integer as
        // a new block receiver merely because code reads at offset 16.
        Values.emplace(key(Op.Output),
                       Value{Value::Kind::CallInteger, NextCallResultBase++, 0,
                             Bound->Signature.ReturnType});
      } else if (Op.Opcode == NdOp::CALL && Bound &&
                 Image.Arch == Arch::AArch64 &&
                 Bound->CallKind == SourceCallTypeHint::Kind::ObjCMessage &&
                 Bound->Signature.HasExplicitABI &&
                 Bound->Signature.Convention ==
                     SourceFunctionTypeHint::ConventionKind::C &&
                 Bound->Signature.ReturnType &&
                 Bound->Signature.ReturnType->Kind == NdTypeKind::Struct &&
                 Bound->Signature.ReturnType->Size == 16 &&
                 Bound->Signature.ReturnComponents.size() == 2 &&
                 TRI.IntReturnRegs.size() >= 2 && Op.Output.isReg() &&
                 Op.Output.Offset == TRI.IntReturnReg && Op.Output.Size == 8) {
        const auto Members =
            sourceAggregateMembers(Bound->Signature.ReturnType);
        bool IntegerPair = Members.size() == 2;
        for (size_t I = 0; IntegerPair && I < 2; ++I)
          IntegerPair =
              Members[I].Type && Members[I].Type->Kind == NdTypeKind::Int &&
              Members[I].Type->Size == 8 && Members[I].ByteOffset == I * 8 &&
              Bound->Signature.ReturnComponents[I].Kind ==
                  SourceABICarrierKind::IntegerRegister &&
              Bound->Signature.ReturnComponents[I].RegisterOffset ==
                  TRI.IntReturnRegs[I] &&
              Bound->Signature.ReturnComponents[I].ValueBytes == 8;
        if (IntegerPair)
          for (size_t I = 0; I < 2; ++I)
            // Each declared member is a distinct integer value, never a
            // recovered block receiver or an invented aggregate pointer.
            Values.emplace(key(NdVar::reg(TRI.IntReturnRegs[I], 8)),
                           Value{Value::Kind::CallInteger, NextCallResultBase++,
                                 0, Members[I].Type});
      }
      continue;
    }
    const bool PlainMemory =
        Op.MemoryOrdering == NdMemoryOrdering::None &&
        Op.MemoryAddressSpace == NdMemoryAddressSpace::Default;
    if (Op.Opcode == NdOp::STORE) {
      auto Address =
          Op.NumInputs == 2 && PlainMemory ? Read(Op.Inputs[0]) : std::nullopt;
      auto Stored = Op.NumInputs == 2 ? Read(Op.Inputs[1]) : std::nullopt;
      if (Stored && Stored->K == Value::Kind::Frame)
        FrameEscaped = true;
      // Even a private store can change a previously loaded target through a
      // malformed alias; never move the invoke read across any intervening
      // store.
      for (auto It = Values.begin(); It != Values.end();)
        if (It->second.K == Value::Kind::Invoke)
          It = Values.erase(It);
        else
          ++It;
      if (FrameEscaped || !Address || Address->K != Value::Kind::Frame ||
          !Stored || Stored->K == Value::Kind::Frame ||
          Address->Offset <
              -::neverd::limits::ir::kMaxSourceFrameDisplacement ||
          Address->Offset > ::neverd::limits::ir::kMaxSourceFrameDisplacement) {
        FrameSlots.clear();
      } else {
        const auto Size = Op.Inputs[1].Size;
        for (auto It = FrameSlots.begin(); It != FrameSlots.end();)
          if (It->first.first < Address->Offset + Size &&
              Address->Offset < It->first.first + It->first.second)
            It = FrameSlots.erase(It);
          else
            ++It;
        FrameSlots[{Address->Offset, Size}] = *Stored;
      }
      continue;
    }
    std::optional<Value> Out;
    if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
         Op.Opcode == NdOp::INT_SEXT) &&
        Op.NumInputs == 1) {
      Out = Read(Op.Inputs[0]);
    } else if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
               Op.NumInputs == 2 && Op.Inputs[1].isConst()) {
      Out = Read(Op.Inputs[0]);
      if (Out && Out->K != Value::Kind::Invoke) {
        const uint64_t Delta = Op.Inputs[1].Offset;
        Out->Offset =
            int64_t(uint64_t(Out->Offset) +
                    (Op.Opcode == NdOp::INT_ADD ? Delta : uint64_t(0) - Delta));
      } else {
        Out.reset();
      }
    } else if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1 && PlainMemory) {
      auto Address = Read(Op.Inputs[0]);
      if (Address && Address->K == Value::Kind::Number && Op.Output.Size == 8 &&
          readImmutableImageBytes(Image,
                                  uint64_t(Address->Base + Address->Offset), 8))
        // Immutable scalar bytes carry no pointer identity.
        Out = Value{Value::Kind::ImageBits,
                    uint64_t(Address->Base + Address->Offset), 0,
                    NdType::makeInt(8, false)};
      if (Address && Address->K == Value::Kind::Number && Op.Output.Size == 8 &&
          writablePointerSlot(Image, uint64_t(Address->Base + Address->Offset)))
        // Each load is a distinct value: a second read of mutable storage
        // cannot be equated with the receiver used to load its invoke slot.
        Out = Value{Value::Kind::Scalar, NextCallResultBase++, 0,
                    NdType::makeInt(8, false)};
      if (Address && Address->K == Value::Kind::Number && Op.Output.Size == 8) {
        const auto Effective = Address->Base + uint64_t(Address->Offset);
        std::string Error;
        if (Effective >= 16 &&
            readObjCBlockLiteral(Image, Effective - 16, Error)) {
          Address->Base = Effective - 16;
          Address->Offset = 16;
        }
      }
      if (Address && Address->K == Value::Kind::Frame) {
        auto It = FrameSlots.find({Address->Offset, Op.Output.Size});
        if (It != FrameSlots.end())
          Out = It->second;
      } else if (ProvenCaptureContext && Address &&
                 Address->K == Value::Kind::Scalar &&
                 Address->Base == TRI.IntParamRegs[0] + 1 &&
                 Address->Offset >= 32 && Op.Output.Size == 8 &&
                 Captures->ScalarWords.count(uint64_t(Address->Offset))) {
        const bool BlockPointer =
            Captures->BlockWords.count(uint64_t(Address->Offset));
        Out =
            Value{BlockPointer ? Value::Kind::Scalar : Value::Kind::CallInteger,
                  NextCallResultBase++, 0,
                  BlockPointer ? Pointer : NdType::makeInt(8, false)};
      } else if (Address &&
                 (Address->K == Value::Kind::Scalar ||
                  Address->K == Value::Kind::Number) &&
                 Address->Offset == 16 && Op.Output.Size == 8) {
        Out = *Address;
        Out->K = Value::Kind::Invoke;
        Out->Offset = 0;
        Out->Type = Pointer;
      }
    }
    if (!Op.Output.Size)
      continue;
    for (auto It = Values.begin(); It != Values.end();)
      if (std::get<0>(It->first) == Op.Output.Space &&
          std::get<1>(It->first) < Op.Output.Offset + Op.Output.Size &&
          Op.Output.Offset < std::get<1>(It->first) + std::get<2>(It->first))
        It = Values.erase(It);
      else
        ++It;
    if (Out && (width(Op.Output.Size) ||
                (Op.Opcode == NdOp::INT_ZEXT && Op.Output.Size == 16 &&
                 Out->K == Value::Kind::ImageBits)))
      Values.emplace(key(Op.Output), std::move(*Out));
    if (Op.Output.isReg()) {
      for (auto Reg : TRI.IntParamRegs)
        if (overlaps(Op.Output, Reg, 8))
          WrittenArguments.insert(Reg);
      for (auto Reg : TRI.FPParamRegs)
        FloatingArgumentWrite |= overlaps(Op.Output, Reg, 16);
    }
  }
  return Result;
}
} // namespace

std::map<va_t, SourceCallTypeHint>
buildObjCBlockCallHints(const BinaryImage &Image, const LowFunc &Function,
                        const SourceFunctionTypeHint *EntrySignature,
                        const std::map<va_t, SourceCallTypeHint> *BoundCalls,
                        const ObjCBlockCaptureCallFields *Captures) {
  std::map<va_t, SourceCallTypeHint> Result;
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Function.Blocks.empty() ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64))
    return Result;
  bool HasIndirectCall = false;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops)
      HasIndirectCall |= Op.Opcode == NdOp::INDIR_CALL;
  if (!HasIndirectCall)
    return Result;
  const LowBlock *Entry = nullptr;
  if (Function.Blocks.size() == 1) {
    Entry = &Function.Blocks.front();
    if (!Entry->Succs.empty())
      return Result;
  } else {
    for (const auto &Candidate : Function.Blocks)
      if (Candidate.StartAddr == Function.Entry) {
        if (Entry)
          return Result;
        Entry = &Candidate;
      }
  }
  if (!Entry || !Entry->Preds.empty() || !Entry->ExceptionalPreds.empty())
    return Result;
  Result = analyzeBlock(Image, *Entry, EntrySignature, BoundCalls, Captures,
                        &Function, false);
  if (Function.Blocks.size() == 1 ||
      Function.Blocks.size() >
          ::neverd::limits::loader::
              kObjCBlockCallHintsBuildObjCBlockCallHintsFunctionBlocksSizeLimit)
    return Result;

  // A successor with one ordinary predecessor has one proven incoming state.
  // Reconstruct that path, retaining branches as result-use barriers, and
  // publish only calls physically present in its final block. Joins, cycles,
  // exceptional entries, and paths exceeding the local proof budget stay
  // unbound.
  std::map<int, const LowBlock *> ById;
  for (const auto &Block : Function.Blocks)
    if (Block.Id < 0 || !ById.emplace(Block.Id, &Block).second)
      return Result;
  std::vector<std::vector<const LowBlock *>> Paths{{Entry}};
  for (size_t PathIndex = 0; PathIndex < Paths.size(); ++PathIndex) {
    const auto Path = Paths[PathIndex];
    const auto *Parent = Path.back();
    if (Path.size() >=
        ::neverd::limits::loader::
            kObjCBlockCallHintsBuildObjCBlockCallHintsPathSizeLimit)
      continue;
    bool Transferable = true;
    for (size_t I = 0; I < Parent->Ops.size(); ++I) {
      const auto Opcode = Parent->Ops[I].Opcode;
      if (Opcode == NdOp::INTRINSIC || Opcode == NdOp::RETURN ||
          Opcode == NdOp::INDIR_BR ||
          (branchTerminator(Opcode) && I + 1 != Parent->Ops.size())) {
        Transferable = false;
        break;
      }
    }
    if (!Transferable)
      continue;
    for (const auto SuccessorId : Parent->Succs) {
      const auto It = ById.find(SuccessorId);
      if (It == ById.end())
        continue;
      const auto *Successor = It->second;
      if (Successor->Preds.size() != 1 ||
          Successor->Preds.front() != Parent->Id ||
          !Successor->ExceptionalPreds.empty())
        continue;
      bool Cycle = false;
      for (const auto *Ancestor : Path)
        Cycle |= Ancestor->Id == SuccessorId;
      if (Cycle)
        continue;
      auto Extended = Path;
      Extended.push_back(Successor);
      bool HasInvoke = false;
      for (const auto &Op : Successor->Ops)
        HasInvoke |= Op.Opcode == NdOp::INDIR_CALL;
      if (HasInvoke) {
        LowBlock Linear;
        bool Valid = true;
        for (size_t I = 0; I < Extended.size(); ++I) {
          const auto *Part = Extended[I];
          if (Linear.Ops.size() + Part->Ops.size() + 1 >
              ::neverd::limits::loader::
                  kObjCBlockCallHintsBuildObjCBlockCallHintsLinearOpsSizePartOpsSize1Limit) {
            Valid = false;
            break;
          }
          Linear.Ops.insert(Linear.Ops.end(), Part->Ops.begin(),
                            Part->Ops.end());
          if (I + 1 != Extended.size() &&
              (Part->Ops.empty() ||
               !branchTerminator(Part->Ops.back().Opcode))) {
            LowOp Boundary;
            Boundary.Opcode = NdOp::BRANCH;
            Linear.Ops.push_back(std::move(Boundary));
          }
        }
        if (!Valid)
          continue;
        const auto Hints = analyzeBlock(Image, Linear, EntrySignature,
                                        BoundCalls, Captures, &Function, true);
        for (const auto &Op : Successor->Ops) {
          const auto Found = Hints.find(Op.Addr);
          if (Found != Hints.end())
            Result.emplace(*Found);
        }
      }
      if (Paths.size() <
          ::neverd::limits::loader::
              kObjCBlockCallHintsBuildObjCBlockCallHintsPathsSizeLimit)
        Paths.push_back(std::move(Extended));
    }
  }

  // A join can still have a single call ABI when every ordinary incoming
  // path independently proves it. Enumerate all bounded acyclic paths back
  // to entry; an unknown predecessor invalidates the whole proof.
  for (const auto &Candidate : Function.Blocks) {
    bool HasUnboundInvoke = false;
    for (const auto &Op : Candidate.Ops)
      HasUnboundInvoke |=
          Op.Opcode == NdOp::INDIR_CALL && !Result.count(Op.Addr);
    if (!HasUnboundInvoke || &Candidate == Entry)
      continue;
    std::vector<std::vector<const LowBlock *>> Pending{{&Candidate}};
    std::vector<std::vector<const LowBlock *>> Incoming;
    bool Complete = true;
    while (!Pending.empty() && Complete) {
      auto Reverse = std::move(Pending.back());
      Pending.pop_back();
      const auto *Current = Reverse.back();
      if (Current == Entry) {
        std::reverse(Reverse.begin(), Reverse.end());
        Incoming.push_back(std::move(Reverse));
        continue;
      }
      if (Current->Preds.empty() || !Current->ExceptionalPreds.empty() ||
          Reverse.size() >=
              ::neverd::limits::loader::
                  kObjCBlockCallHintsBuildObjCBlockCallHintsReverseSizeLimit) {
        Complete = false;
        break;
      }
      std::set<int> SeenPredecessors;
      for (const auto PredecessorId : Current->Preds) {
        const auto It = ById.find(PredecessorId);
        if (!SeenPredecessors.insert(PredecessorId).second ||
            It == ById.end() ||
            std::find(It->second->Succs.begin(), It->second->Succs.end(),
                      Current->Id) == It->second->Succs.end()) {
          Complete = false;
          break;
        }
        const auto *Predecessor = It->second;
        for (const auto *Ancestor : Reverse)
          if (Ancestor == Predecessor)
            Complete = false;
        for (size_t I = 0; I < Predecessor->Ops.size(); ++I) {
          const auto Opcode = Predecessor->Ops[I].Opcode;
          if (Opcode == NdOp::INTRINSIC || Opcode == NdOp::RETURN ||
              Opcode == NdOp::INDIR_BR ||
              (branchTerminator(Opcode) && I + 1 != Predecessor->Ops.size()))
            Complete = false;
        }
        if (!Complete)
          break;
        auto Extended = Reverse;
        Extended.push_back(Predecessor);
        Pending.push_back(std::move(Extended));
        if (Pending.size() + Incoming.size() > 16) {
          Complete = false;
          break;
        }
      }
    }
    if (!Complete || Incoming.size() < 2)
      continue;
    std::map<va_t, SourceCallTypeHint> Common;
    std::map<va_t, std::set<size_t>> CommonZeroArguments;
    bool FirstPath = true;
    for (const auto &Path : Incoming) {
      LowBlock Linear;
      for (size_t I = 0; I < Path.size(); ++I) {
        const auto *Part = Path[I];
        if (Linear.Ops.size() + Part->Ops.size() + 1 >
            ::neverd::limits::loader::
                kObjCBlockCallHintsBuildObjCBlockCallHintsLinearOpsSizePartOpsSize1Limit) {
          Complete = false;
          break;
        }
        Linear.Ops.insert(Linear.Ops.end(), Part->Ops.begin(), Part->Ops.end());
        if (I + 1 != Path.size() &&
            (Part->Ops.empty() || !branchTerminator(Part->Ops.back().Opcode))) {
          LowOp Boundary;
          Boundary.Opcode = NdOp::BRANCH;
          Linear.Ops.push_back(std::move(Boundary));
        }
      }
      if (!Complete)
        break;
      std::map<va_t, std::set<size_t>> ZeroArguments;
      const auto Hints =
          analyzeBlock(Image, Linear, EntrySignature, BoundCalls, Captures,
                       &Function, true, &ZeroArguments);
      if (FirstPath) {
        for (const auto &Op : Candidate.Ops) {
          const auto Found = Hints.find(Op.Addr);
          if (Op.Opcode == NdOp::INDIR_CALL && Found != Hints.end() &&
              !Result.count(Op.Addr)) {
            Common.emplace(*Found);
            CommonZeroArguments.emplace(Op.Addr, ZeroArguments[Op.Addr]);
          }
        }
        FirstPath = false;
      } else {
        for (auto It = Common.begin(); It != Common.end();) {
          const auto Found = Hints.find(It->first);
          if (Found == Hints.end() ||
              !mergeInferredBlockABI(It->second, Found->second,
                                     CommonZeroArguments[It->first],
                                     ZeroArguments[It->first]))
            It = Common.erase(It);
          else
            ++It;
        }
      }
      if (Common.empty())
        break;
    }
    if (Complete)
      Result.insert(Common.begin(), Common.end());
  }
  if (Image.Arch == Arch::AArch64 && Captures &&
      !Captures->BlockWords.empty() && EntrySignature &&
      EntrySignature->Origin ==
          SourceFunctionTypeHint::OriginKind::BlockRuntime) {
    const auto &TRI = getTargetRegInfo(Image.Arch);
    // A loop can defeat bounded path enumeration even when a callee-save
    // register holds the same descriptor-authenticated block context on every
    // incoming edge. Replay only the final block with that one proven value;
    // all volatile argument carriers start unknown.
    for (const auto &Candidate : Function.Blocks) {
      const LowOp *Invoke = nullptr;
      size_t InvokeIndex = 0;
      for (size_t I = 0; I < Candidate.Ops.size(); ++I)
        if (Candidate.Ops[I].Opcode == NdOp::INDIR_CALL) {
          if (Invoke) {
            Invoke = nullptr;
            break;
          }
          Invoke = &Candidate.Ops[I];
          InvokeIndex = I;
        }
      if (!Invoke || Result.count(Invoke->Addr) ||
          !clearedFloatingArgumentsBeforeCall(Candidate, InvokeIndex, TRI,
                                              BoundCalls, Image.Arch))
        continue;
      const auto Registers = preservedEntryContextRegisters(
          Function, *Entry, Candidate, TRI, BoundCalls, Image.Arch);
      std::optional<SourceCallTypeHint> Unique;
      bool Ambiguous = false;
      for (const auto Register : Registers) {
        const auto Hints =
            analyzeBlock(Image, Candidate, EntrySignature, BoundCalls, Captures,
                         &Function, false, nullptr, Register);
        const auto Found = Hints.find(Invoke->Addr);
        if (Found == Hints.end())
          continue;
        if (Unique) {
          Ambiguous = true;
          break;
        }
        Unique = Found->second;
      }
      if (Unique && !Ambiguous)
        Result.emplace(Invoke->Addr, std::move(*Unique));
    }
  }
  return Result;
}
} // namespace neverd

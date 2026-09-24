#include "neverd/loader/ObjC/ObjCBlockCallHints.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCBlocks.h"

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
struct Value {
  enum class Kind { Scalar, Frame, Number, Invoke };
  Kind K = Kind::Scalar;
  // A base identifies one original incoming value; copies preserve identity.
  uint64_t Base = 0;
  int64_t Offset = 0;
  TypeRef Type;
  bool NumericBase = false;
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

std::optional<unsigned>
resultWidth(const LowBlock &Block, size_t CallIndex, const TargetRegInfo &TRI,
            Arch Architecture,
            const std::map<va_t, SourceCallTypeHint> *BoundCalls) {
  std::optional<unsigned> Width;
  bool IntegerLive = true;
  bool FloatingLive = true;
  for (size_t I = CallIndex + 1; I < Block.Ops.size(); ++I) {
    const auto &Op = Block.Ops[I];
    if (branchTerminator(Op.Opcode) || Op.Opcode == NdOp::INDIR_BR ||
        Op.Opcode == NdOp::INTRINSIC)
      return Width;
    if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
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
      return Width;
    }
    for (unsigned J = 0; J < Op.NumInputs; ++J) {
      const auto &V = Op.Inputs[J];
      if (FloatingLive && overlaps(V, TRI.FPReturnReg, 16))
        return std::nullopt;
      if (IntegerLive && V.isReg() && V.Offset == TRI.IntReturnReg &&
          width(V.Size))
        Width = std::max(Width.value_or(0), unsigned(V.Size));
    }
    if (overlaps(Op.Output, TRI.IntReturnReg, 8))
      IntegerLive = false;
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
std::map<va_t, SourceCallTypeHint>
analyzeBlock(const BinaryImage &Image, const LowBlock &Block,
             const SourceFunctionTypeHint *EntrySignature,
             const std::map<va_t, SourceCallTypeHint> *BoundCalls,
             bool FollowValidatedBranches) {
  std::map<va_t, SourceCallTypeHint> Result;
  if (Block.Ops.size() > 65536)
    return Result;
  const auto &TRI = getTargetRegInfo(Image.Arch);
  std::string EntryError;
  if (EntrySignature && (EntrySignature->Architecture != Image.Arch ||
                         !validateSourceABI(*EntrySignature, EntryError)))
    return Result;
  std::map<Key, Value> Values;
  std::map<std::pair<int64_t, unsigned>, Value> FrameSlots;
  std::set<uint64_t> WrittenArguments;
  bool FloatingArgumentWrite = false;
  bool FrameEscaped = false;
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  Values.emplace(key(NdVar::reg(TRI.StackPointer, 8)),
                 Value{Value::Kind::Frame, 0, 0, Pointer});
  for (auto Register : TRI.IntParamRegs) {
    TypeRef Type = NdType::makeInt(8, false);
    if (EntrySignature)
      for (const auto &P : EntrySignature->Parameters)
        if (P.Location.Kind == SourceABICarrierKind::IntegerRegister &&
            P.Location.RegisterOffset == Register && scalar(P.Type))
          Type = P.Type;
    Values.emplace(key(NdVar::reg(Register, 8)),
                   Value{Value::Kind::Scalar, Register + 1, 0, Type});
  }
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
          Value.K != Value::Kind::Number)
        return std::nullopt;
      if (Value.Type && V.Size < Value.Type->Size)
        Value.Type = NdType::makeInt(V.Size, false);
      return Value;
    }
    return std::nullopt;
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
      auto Target = Op.NumInputs == 1 ? Read(Op.Inputs[0]) : std::nullopt;
      auto Receiver = Read(NdVar::reg(TRI.IntParamRegs[0], 8));
      if (Op.Opcode == NdOp::INDIR_CALL && Target && Receiver &&
          Target->K == Value::Kind::Invoke && Receiver->Offset == 0 &&
          sameBase(*Target, *Receiver) && !FloatingArgumentWrite &&
          WrittenArguments.count(TRI.IntParamRegs[0])) {
        std::optional<SourceFunctionTypeHint> Signature;
        if (Receiver->K == Value::Kind::Number) {
          std::string Error;
          if (auto Literal = readObjCBlockLiteral(Image, Receiver->Base, Error))
            Signature = Literal->Descriptor.InvokeTypeHint;
        }
        if (!Signature) {
          auto ReturnBytes =
              resultWidth(Block, Index, TRI, Image.Arch, BoundCalls);
          if (ReturnBytes) {
            SourceFunctionTypeHint Inferred;
            Inferred.Origin =
                SourceFunctionTypeHint::OriginKind::NativeAnalysis;
            Inferred.ReturnType = NdType::makeInt(*ReturnBytes, false);
            bool Gap = false, Valid = true;
            for (size_t I = 0; I < TRI.IntParamRegs.size(); ++I) {
              const auto Reg = TRI.IntParamRegs[I];
              if (!WrittenArguments.count(Reg)) {
                Gap = true;
                continue;
              }
              auto Argument = Read(NdVar::reg(Reg, 8));
              if (Gap || !Argument || !scalar(Argument->Type) ||
                  Argument->K == Value::Kind::Invoke ||
                  (Argument->K == Value::Kind::Frame &&
                   (FrameEscaped || Argument->Offset < -1048576 ||
                    Argument->Offset > 1048576))) {
                Valid = false;
                break;
              }
              Inferred.Parameters.push_back(
                  {I ? "arg" + std::to_string(I - 1) : "block",
                   I ? Argument->Type : Pointer});
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
            if (P.Location.Kind != SourceABICarrierKind::IntegerRegister ||
                !WrittenArguments.count(P.Location.RegisterOffset)) {
              Valid = false;
              break;
            }
            auto Argument = Read(NdVar::reg(P.Location.RegisterOffset, 8));
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
            Result.emplace(Op.Addr, std::move(Hint));
          }
        }
      }
      // A validated Darwin source ABI preserves the stack and nonvolatile
      // registers on the normal return edge. Every other identity and every
      // frame spill is invalidated. The ARC result is a fresh opaque pointer:
      // the later invoke-slot proof must still show the same block receiver.
      std::map<Key, Value> Preserved;
      const SourceCallTypeHint *Bound = nullptr;
      if (BoundCalls) {
        const auto It = BoundCalls->find(Op.Addr);
        if (It != BoundCalls->end()) {
          std::string Error;
          const auto Kind = It->second.CallKind;
          const bool DarwinCall =
              Kind == SourceCallTypeHint::Kind::ObjCMessage ||
              Kind == SourceCallTypeHint::Kind::ObjCSuper2 ||
              Kind == SourceCallTypeHint::Kind::ObjCRuntimeCall ||
              Kind == SourceCallTypeHint::Kind::DarwinRuntimeCall;
          if (DarwinCall && It->second.Signature.Architecture == Image.Arch &&
              validateSourceABI(It->second.Signature, Error))
            Bound = &It->second;
        }
      }
      if (Bound)
        for (const auto &[K, V] : Values)
          if (std::get<0>(K) == VnodeSpace::REG && V.K != Value::Kind::Invoke &&
              (TRI.isCallPreserved(std::get<1>(K), std::get<2>(K)) ||
               (std::get<1>(K) == TRI.StackPointer &&
                V.K == Value::Kind::Frame)))
            Preserved.emplace(K, V);
      for (auto Register : TRI.IntParamRegs) {
        const auto Argument = Read(NdVar::reg(Register, 8));
        if (Argument && Argument->K == Value::Kind::Frame)
          FrameEscaped = true;
      }
      Values = std::move(Preserved);
      FrameSlots.clear();
      WrittenArguments.clear();
      FloatingArgumentWrite = false;
      if (Op.Opcode == NdOp::CALL && Bound &&
          Bound->CallKind == SourceCallTypeHint::Kind::ObjCRuntimeCall &&
          Bound->TargetName == "objc_retainAutoreleasedReturnValue" &&
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
        Values.emplace(key(Op.Output), Value{Value::Kind::Scalar,
                                             NextCallResultBase++, 0, Pointer});
        WrittenArguments.insert(TRI.IntParamRegs[0]);
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
          Address->Offset < -1048576 || Address->Offset > 1048576) {
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
      if (Address && Address->K == Value::Kind::Number &&
          Address->Offset == 0 && Address->Base >= 16 && Op.Output.Size == 8) {
        std::string Error;
        if (readObjCBlockLiteral(Image, Address->Base - 16, Error)) {
          Address->Base -= 16;
          Address->Offset = 16;
        }
      }
      if (Address && Address->K == Value::Kind::Frame) {
        auto It = FrameSlots.find({Address->Offset, Op.Output.Size});
        if (It != FrameSlots.end())
          Out = It->second;
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
    if (Out && width(Op.Output.Size))
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
                        const std::map<va_t, SourceCallTypeHint> *BoundCalls) {
  std::map<va_t, SourceCallTypeHint> Result;
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Function.Blocks.empty() ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64))
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
  Result = analyzeBlock(Image, *Entry, EntrySignature, BoundCalls, false);
  if (Function.Blocks.size() == 1 || Function.Blocks.size() > 64)
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
    if (Path.size() >= 64)
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
      LowBlock Linear;
      bool Valid = true;
      for (size_t I = 0; I < Extended.size(); ++I) {
        const auto *Part = Extended[I];
        if (Linear.Ops.size() + Part->Ops.size() + 1 > 65536) {
          Valid = false;
          break;
        }
        Linear.Ops.insert(Linear.Ops.end(), Part->Ops.begin(), Part->Ops.end());
        if (I + 1 != Extended.size() &&
            (Part->Ops.empty() || !branchTerminator(Part->Ops.back().Opcode))) {
          LowOp Boundary;
          Boundary.Opcode = NdOp::BRANCH;
          Linear.Ops.push_back(std::move(Boundary));
        }
      }
      if (!Valid)
        continue;
      const auto Hints =
          analyzeBlock(Image, Linear, EntrySignature, BoundCalls, true);
      for (const auto &Op : Successor->Ops) {
        const auto Found = Hints.find(Op.Addr);
        if (Found != Hints.end())
          Result.emplace(*Found);
      }
      if (Paths.size() < 64)
        Paths.push_back(std::move(Extended));
    }
  }
  return Result;
}
} // namespace neverd

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

std::optional<unsigned> resultWidth(const LowBlock &Block, size_t CallIndex,
                                    const TargetRegInfo &TRI,
                                    Arch Architecture) {
  std::optional<unsigned> Width;
  bool IntegerLive = true;
  bool FloatingLive = true;
  for (size_t I = CallIndex + 1; I < Block.Ops.size(); ++I) {
    const auto &Op = Block.Ops[I];
    if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
      return Width;
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
} // namespace

std::map<va_t, SourceCallTypeHint>
buildObjCBlockCallHints(const BinaryImage &Image, const LowFunc &Function,
                        const SourceFunctionTypeHint *EntrySignature) {
  std::map<va_t, SourceCallTypeHint> Result;
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Function.Blocks.size() != 1 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64))
    return Result;
  const auto &Block = Function.Blocks.front();
  if (!Block.Succs.empty() || !Block.Preds.empty() || Block.Ops.size() > 65536)
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
  for (size_t Index = 0; Index < Block.Ops.size(); ++Index) {
    const auto &Op = Block.Ops[Index];
    if (Op.Opcode == NdOp::INTRINSIC || Op.Opcode == NdOp::BRANCH)
      return {};
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
          auto ReturnBytes = resultWidth(Block, Index, TRI, Image.Arch);
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
                  Argument->K == Value::Kind::Frame ||
                  Argument->K == Value::Kind::Invoke) {
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
      // Do not carry an identity or a spill through an unknown callee.
      Values.clear();
      FrameSlots.clear();
      WrittenArguments.clear();
      FloatingArgumentWrite = false;
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
} // namespace neverd

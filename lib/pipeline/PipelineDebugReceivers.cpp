//===- PipelineDebugReceivers.cpp - Scoped receiver types ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prove receiver copies before consuming occurrence-sensitive debug types.
//===----------------------------------------------------------------------===//

#include "PipelineDebugReceivers.h"

#include "neverd/debug/DebugContext.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/SourceParameterPlacement.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <optional>
#include <set>
#include <utility>

namespace neverd {
DebugPointerOperands debugPointerOperandTypes(const BinaryImage &Image,
                                              const LowFunc &Low,
                                              const MedFunc &Med,
                                              DebugContext &Debug) {
  DebugPointerOperands Result;
  if (!Debug.hasAuthenticatedObjectExtents() || Low.Entry != Med.Entry)
    return Result;
  const unsigned Bytes = getTargetRegInfo(Image.Arch).PointerSize;
  using Occurrence = std::pair<va_t, int>;
  llvm::DenseMap<Occurrence, const MedOp *> Operations;
  for (const auto &Block : Med.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Addr != InvalidVA && Op.OriginSeq >= 0) {
        auto [At, Fresh] =
            Operations.try_emplace(Occurrence{Op.Addr, Op.OriginSeq}, &Op);
        if (!Fresh)
          At->second = nullptr;
      }
  llvm::DenseMap<std::pair<va_t, uint64_t>, TypeRef> Declarations;
  for (const auto &Block : Low.Blocks) {
    llvm::DenseMap<uint64_t, uint64_t> Copies;
    llvm::SmallVector<std::pair<uint64_t, unsigned>, 4> Writes;
    bool Clobbered = false;
    va_t Instruction = InvalidVA;
    for (const auto &Native : Block.Ops) {
      if (Instruction != Native.Addr) {
        Copies.clear();
        Writes.clear();
        Clobbered = false;
        Instruction = Native.Addr;
      }
      const auto RegisterFor =
          [&](const NdVar &Value) -> std::optional<uint64_t> {
        if (Value.Size != Bytes)
          return std::nullopt;
        if (Value.isReg() && !Clobbered &&
            llvm::none_of(Writes, [&](const auto &Write) {
              return Write.first >= Value.Offset
                         ? Write.first - Value.Offset < Bytes
                         : Value.Offset - Write.first < Write.second;
            }))
          return Value.Offset;
        if (Value.isTemp()) {
          const auto Copy = Copies.find(Value.Offset);
          if (Copy != Copies.end())
            return Copy->second;
        }
        return std::nullopt;
      };
      const auto At = Operations.find({Native.Addr, Native.Seq});
      const MedOp *Op = At == Operations.end() ? nullptr : At->second;
      const bool Arithmetic =
          Native.Opcode == NdOp::INT_ADD || Native.Opcode == NdOp::INT_SUB;
      const bool Memory =
          Native.Opcode == NdOp::LOAD || Native.Opcode == NdOp::STORE;
      if (Op && !Op->Dead && Op->Opcode == Native.Opcode &&
          Op->NumInputs == Native.NumInputs && (Arithmetic || Memory)) {
        const unsigned Count = Arithmetic ? Op->NumInputs : 1;
        for (unsigned I = 0; I < Count; ++I) {
          const auto &Input = Op->Inputs[I];
          if (Input.Size != Bytes ||
              (Input.Kind != MedVar::Reg && Input.Kind != MedVar::Temp &&
               Input.Kind != MedVar::Param))
            continue;
          // LOAD/STORE retain the address operand's value. For arithmetic,
          // an unchanged constant companion also preserves its position and
          // base value. A reassociated offset or commuted variable pair is
          // not such a witness. SSA copy propagation may rename the base to
          // any kind of value, and DCE may remove all intermediate copies.
          if (Arithmetic &&
              (Op->NumInputs != 2 || !Native.Inputs[1 - I].isConst() ||
               !Op->Inputs[1 - I].isConst() ||
               Native.Inputs[1 - I].Size != Op->Inputs[1 - I].Size ||
               Native.Inputs[1 - I].Offset != Op->Inputs[1 - I].ConstVal))
            continue;
          const auto Register = RegisterFor(Native.Inputs[I]);
          if (!Register)
            continue;
          auto [Known, Fresh] =
              Declarations.try_emplace(std::pair{Op->Addr, *Register});
          if (Fresh) {
            const auto Found =
                Debug.resolveRegisterVariableAt(Low.Entry, Op->Addr, *Register);
            const auto &Type = Found.Variable.Type;
            if (Found.Status == VariableExtentLookupStatus::Unique && Type &&
                Type->Kind == NdTypeKind::Ptr && Type->Size == Bytes &&
                Type->Pointee && Type->Pointee->Kind == NdTypeKind::Struct &&
                !Type->Pointee->IsEnum)
              Known->second = Type;
          }
          if (Known->second)
            Result[{Op->Addr, Op->OriginSeq}].push_back({Input, Known->second});
        }
      }
      std::optional<uint64_t> Copy;
      if (Native.Opcode == NdOp::COPY && Native.NumInputs == 1 &&
          Native.Output.Size == Bytes)
        Copy = RegisterFor(Native.Inputs[0]);
      if (Native.Output.isTemp()) {
        Copies.erase(Native.Output.Offset);
        if (Copy)
          Copies.try_emplace(Native.Output.Offset, *Copy);
      }
      if (Native.Output.isReg() && Native.Output.Size)
        Writes.emplace_back(Native.Output.Offset, Native.Output.Size);
      Clobbered |= Native.Opcode == NdOp::CALL ||
                   Native.Opcode == NdOp::INDIR_CALL ||
                   Native.Opcode == NdOp::INTRINSIC;
    }
  }
  return Result;
}

DebugVirtualCallArguments debugVirtualCallArguments(
    const BinaryImage &Image, const LowFunc &Function, DebugContext &Debug,
    const std::function<const ItaniumVTableIndex &()> &Tables) {
  DebugVirtualCallArguments Result;
  if ((Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) ||
      (Image.Format != BinaryFormat::ELF &&
       Image.Format != BinaryFormat::MachO) ||
      Function.Blocks.empty())
    return Result;
  const auto &Block = Function.Blocks.front();
  if (Block.StartAddr != Function.Entry || !Block.Preds.empty() ||
      llvm::none_of(Block.Ops, [](const LowOp &Op) {
        return Op.Opcode == NdOp::INDIR_CALL;
      }))
    return Result;
  const auto Entry = Debug.resolveFunction(Function.Entry);
  if (!Entry || !Entry->PlatformConvention || Entry->IsVariadic ||
      Entry->Params.empty())
    return Result;
  std::vector<TypeRef> Parameters;
  for (const auto &[Name, Type] : Entry->Params)
    Parameters.push_back(Type);
  const auto Placement = placeSourceParameters(Image.Arch, Image.abiFormat(),
                                               Entry->ReturnType, Parameters);
  if (!Placement || Placement->Parameters.size() != Parameters.size())
    return Result;
  const auto &TRI = getTargetRegInfo(Image.Arch);
  const unsigned Bytes = TRI.PointerSize;
  const auto Registers = TRI.integerArgumentLayout(Image.abiFormat()).Registers;
  if (Registers.empty())
    return Result;
  struct Value {
    enum Kind { Object, Table, Target } Kind;
    unsigned Parameter;
    uint64_t Offset = 0;
  };
  using Key = std::pair<VnodeSpace, uint64_t>;
  std::map<Key, Value> Values;
  for (size_t I = 0; I < Parameters.size(); ++I) {
    const auto &Type = Parameters[I];
    const auto &Parts = Placement->Parameters[I];
    if (!Type || Type->Kind != NdTypeKind::Ptr || Type->Size != Bytes ||
        !Type->Pointee || Type->Pointee->Kind != NdTypeKind::Struct ||
        Type->Pointee->IsEnum || Type->Pointee->SourceName.empty() ||
        Parts.size() != 1 || Parts[0].Indirect || Parts[0].Offset ||
        Parts[0].Location.Kind != SourceABICarrierKind::IntegerRegister ||
        Parts[0].Location.ValueBytes != Bytes)
      continue;
    Values.emplace(Key{VnodeSpace::REG, Parts[0].Location.RegisterOffset},
                   Value{Value::Object, unsigned(I)});
  }
  if (Values.empty())
    return Result;
  const auto Read = [&](const NdVar &V) -> std::optional<Value> {
    if (V.Size != Bytes)
      return {};
    const auto Found = Values.find({V.Space, V.Offset});
    return Found != Values.end() ? std::optional(Found->second) : std::nullopt;
  };
  va_t Instruction = InvalidVA;
  for (const auto &Op : Block.Ops) {
    if (Instruction != Op.Addr) {
      std::erase_if(Values, [](const auto &Item) {
        return Item.first.first == VnodeSpace::TEMP;
      });
      Instruction = Op.Addr;
    }
    if (Op.Opcode == NdOp::INDIR_CALL && Op.NumInputs == 1) {
      const auto Target = Read(Op.Inputs[0]);
      const auto Object = Read(NdVar::reg(Registers.front(), Bytes));
      if (Target && Target->Kind == Value::Target && Object &&
          Object->Kind == Value::Object && !Object->Offset &&
          Object->Parameter == Target->Parameter) {
        const auto &Class = Parameters[Object->Parameter]->Pointee->SourceName;
        const auto Slot = Tables().resolve(Image, Class, Target->Offset);
        const auto Callee =
            Slot ? Debug.resolveFunction(Slot->Target) : std::nullopt;
        if (Callee && Callee->Addr == Slot->Target &&
            Callee->PlatformConvention && !Callee->IsVariadic &&
            !Callee->Params.empty() && Callee->ParamEntryLocations.empty() &&
            Callee->CallConv != DebugCallConv::Fastcall &&
            Callee->CallConv != DebugCallConv::Thiscall &&
            Callee->Params.front().second &&
            Callee->Params.front().second->Kind == NdTypeKind::Ptr) {
          std::vector<TypeRef> Types;
          for (const auto &[Name, Type] : Callee->Params)
            Types.push_back(Type);
          const SourceCallOccurrenceKey Site{Op.Addr, Op.Seq, Op.Opcode,
                                             std::nullopt};
          if (auto Args = fixedIntegerCallArguments(
                  Image.Arch, Image.abiFormat(), Callee->ReturnType, Types))
            Result.Integers.emplace(Site, std::move(*Args));
          else if (auto Args =
                       floatingCallArguments(Image.Arch, Image.abiFormat(),
                                             Callee->ReturnType, Types);
                   Args && Args->ParameterElements.empty() &&
                   !Args->ReturnElements)
            Result.Floating.emplace(Site, std::move(*Args));
        }
      }
    }
    // This proof covers straight-line entry values only. Calls, side-effect
    // intrinsics and transfers end it; a later region needs its own reaching
    // state rather than borrowing the incoming object's identity.
    if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
        Op.Opcode == NdOp::INTRINSIC || Op.Opcode == NdOp::BRANCH ||
        Op.Opcode == NdOp::COND_BR || Op.Opcode == NdOp::INDIR_BR)
      break;
    std::optional<Value> Next;
    if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
      Next = Read(Op.Inputs[0]);
    else if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs == 2 &&
             Op.Inputs[1].isConst()) {
      Next = Read(Op.Inputs[0]);
      if (Next && Next->Kind == Value::Table &&
          Op.Inputs[1].Offset <= UINT64_MAX - Next->Offset)
        Next->Offset += Op.Inputs[1].Offset;
      else
        Next.reset();
    } else if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1 &&
               Op.MemoryOrdering == NdMemoryOrdering::None &&
               Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      Next = Read(Op.Inputs[0]);
      if (Next && Next->Kind == Value::Object && !Next->Offset)
        Next->Kind = Value::Table;
      else if (Next && Next->Kind == Value::Table)
        Next->Kind = Value::Target;
      else
        Next.reset();
    }
    const auto &Out = Op.Output;
    if (!Out.Size || (!Out.isReg() && !Out.isTemp()))
      continue;
    auto Offset = Out.Offset;
    auto Size = Out.Size;
    if (Out.isReg() && TRI.writeZeroExtends(Offset, Size))
      std::tie(Offset, Size) = TRI.findWideReg(Offset, Size);
    std::erase_if(Values, [&](const auto &Item) {
      return Item.first.first == Out.Space &&
             (Item.first.second >= Offset ? Item.first.second - Offset < Size
                                          : Offset - Item.first.second < Bytes);
    });
    if (Next && Out.Size == Bytes)
      Values.emplace(Key{Out.Space, Out.Offset}, *Next);
  }
  return Result;
}

std::map<va_t, TypeRef> debugCallReceiverTypes(const BinaryImage &Image,
                                               const LowFunc &Low,
                                               const MedFunc &Med,
                                               const HighFunc &High,
                                               DebugContext &Debug) {
  std::map<va_t, TypeRef> Result;
  if (!Debug.hasAuthenticatedObjectExtents() || Low.Entry != Med.Entry ||
      Low.Entry != High.Entry)
    return Result;
  const auto &TRI = getTargetRegInfo(Image.Arch);
  const auto Layout = TRI.integerArgumentLayout(Image.abiFormat());
  if (Layout.Registers.empty())
    return Result;
  const auto Register = Layout.Registers.front();
  const auto Bytes = TRI.PointerSize;
  std::map<std::pair<int, int>, const MedCallInfo *> CallInfo;
  for (const auto &Call : Med.CallInfos)
    CallInfo.emplace(std::pair{Call.BlockId, Call.OpIdx}, &Call);

  // Some conventions recover arguments during HighIR lowering. In that
  // case require the actual first operand to name the very same MedIR value;
  // do not infer an argument order from the presence of a register write.
  std::optional<std::map<va_t, std::optional<MedVar>>> HighArguments;
  const auto HighReceiverIs = [&](va_t PC, const MedVar &Receiver) {
    if (!HighArguments) {
      HighArguments.emplace();
      HighExprSet Seen;
      const auto Visit = [&](auto &&Self, const ExprPtr &E) -> void {
        if (!E || !Seen.insert(E.get()).second)
          return;
        E->forEachChildExpr([&](const ExprPtr &Child) { Self(Self, Child); });
        if (E->Kind != ExprKind::Call || !E->IsIndirectCall ||
            !E->CallSiteAddr || E->CallSiteAddr == InvalidVA)
          return;
        std::optional<MedVar> Argument;
        if (!E->SourceCallHint && E->IntrinsicId == Intrinsic::None &&
            !E->Operands.empty()) {
          const auto &First = E->Operands.front();
          if (First && First->Kind == ExprKind::Var && First->Type &&
              First->Type->Size == Bytes && First->Var.Size == Bytes)
            Argument = First->Var;
        }
        auto [At, Fresh] =
            HighArguments->try_emplace(E->CallSiteAddr, Argument);
        if (!Fresh && At->second != Argument)
          At->second.reset();
      };
      walkStmts(High.Body, [&](const HighStmt &S) {
        forEachExpr(S, [&](const ExprPtr &E) { Visit(Visit, E); });
      });
    }
    const auto At = HighArguments->find(PC);
    return At != HighArguments->end() && At->second == Receiver;
  };

  // ABI recovery owns operand order. Also require the surviving full-width
  // copy in the call's own block, so reordered/custom calls cannot borrow
  // the native first register's declaration for another operand.
  std::set<std::pair<va_t, int>> Calls;
  for (const auto &B : Med.Blocks) {
    std::optional<MedVar> Receiver;
    for (size_t I = 0; I < B.Ops.size(); ++I) {
      const auto &Op = B.Ops[I];
      if (Op.Dead)
        continue;
      if (Op.Opcode == NdOp::INDIR_CALL && Receiver && !Op.SourceCallHint) {
        const auto At = CallInfo.find({B.Id, int(I)});
        const auto *Call = At == CallInfo.end() ? nullptr : At->second;
        const bool Matches = Call ? Call->IsIndirect && !Call->SourceCallHint &&
                                        !Call->ArgumentsInCalleeRegisterOrder &&
                                        !Call->Args.empty() &&
                                        Call->Args.front().Size == Bytes &&
                                        Call->Args.front() == *Receiver
                                  : HighReceiverIs(Op.Addr, *Receiver);
        if (Matches)
          Calls.emplace(Op.Addr, Op.OriginSeq);
      }
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
          Op.Opcode == NdOp::INTRINSIC) {
        Receiver.reset();
        continue;
      }
      if (Op.Output.Kind == MedVar::Reg &&
          Layout.registerIndex(Op.Output.RegOff) == 0) {
        Receiver.reset();
        if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
            Op.Output.Size == Bytes && Op.Inputs[0].Size == Bytes)
          Receiver = argRegSourceValueInBlock(B, I, TRI, Layout);
      }
    }
  }
  if (Calls.empty())
    return Result;

  using Key = std::pair<VnodeSpace, uint64_t>;
  std::set<va_t> SeenCalls;
  for (const auto &B : Low.Blocks) {
    if (llvm::none_of(B.Ops, [&](const LowOp &Op) {
          return Calls.contains({Op.Addr, Op.Seq});
        }))
      continue;
    // Values describe copies, not equality of memory or numeric constants.
    // Every load/arithmetic result is fresh, and a partial write kills the
    // containing pointer. Low temporaries live within one instruction.
    std::map<Key, size_t> Values;
    size_t Next = 0;
    va_t Instruction = InvalidVA;
    const auto Read = [&](const NdVar &V) {
      const Key K{V.Space, V.Offset};
      auto [At, Fresh] = Values.try_emplace(K, 0);
      if (Fresh)
        At->second = ++Next;
      return At->second;
    };
    for (const auto &Op : B.Ops) {
      if (Op.Addr != Instruction) {
        std::erase_if(Values, [](const auto &V) {
          return V.first.first == VnodeSpace::TEMP;
        });
        Instruction = Op.Addr;
      }
      if (Calls.contains({Op.Addr, Op.Seq})) {
        if (!SeenCalls.insert(Op.Addr).second) {
          Result.erase(Op.Addr);
        } else {
          const auto Receiver = Read(NdVar::reg(Register, Bytes));
          TypeRef Type;
          bool Conflict = false;
          for (const auto &[K, Value] : Values) {
            if (K.first != VnodeSpace::REG || Value != Receiver)
              continue;
            auto Local =
                Debug.resolveRegisterVariableAt(Low.Entry, Op.Addr, K.second);
            if (Local.Status == VariableExtentLookupStatus::Ambiguous) {
              Conflict = true;
              break;
            }
            if (Local.Status != VariableExtentLookupStatus::Unique)
              continue;
            const auto &Candidate = Local.Variable.Type;
            if (!Candidate || Candidate->Kind != NdTypeKind::Ptr ||
                Candidate->Size != Bytes || !Candidate->Pointee ||
                Candidate->Pointee->Kind != NdTypeKind::Struct ||
                Candidate->Pointee->IsEnum ||
                (Type && !equalSourceTypes(Type, Candidate))) {
              Conflict = true;
              break;
            }
            Type = Candidate;
          }
          if (Type && !Conflict)
            Result.emplace(Op.Addr, std::move(Type));
        }
      }
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
          Op.Opcode == NdOp::INTRINSIC || Op.Opcode == NdOp::BRANCH ||
          Op.Opcode == NdOp::COND_BR || Op.Opcode == NdOp::INDIR_BR) {
        Values.clear();
        continue;
      }
      const auto &Out = Op.Output;
      if (!Out.Size || (!Out.isReg() && !Out.isTemp()))
        continue;
      const size_t Value =
          Op.Opcode == NdOp::COPY && Op.NumInputs == 1 && Out.Size == Bytes &&
                  Op.Inputs[0].Size == Bytes &&
                  (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp())
              ? Read(Op.Inputs[0])
              : ++Next;
      auto Offset = Out.Offset;
      auto Size = Out.Size;
      if (Out.isReg() && TRI.writeZeroExtends(Offset, Size))
        std::tie(Offset, Size) = TRI.findWideReg(Offset, Size);
      std::erase_if(Values, [&](const auto &V) {
        return V.first.first == Out.Space &&
               (V.first.second >= Offset ? V.first.second - Offset < Size
                                         : Offset - V.first.second < Bytes);
      });
      if (Out.Size == Bytes)
        Values[{Out.Space, Out.Offset}] = Value;
    }
  }
  return Result;
}
} // namespace neverd

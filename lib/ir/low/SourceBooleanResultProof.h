#ifndef NEVERD_IR_LOW_SOURCEBOOLEANRESULTPROOF_H
#define NEVERD_IR_LOW_SOURCEBOOLEANRESULTPROOF_H

#include "SourceBooleanResultContract.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/SourceCallOccurrence.h"

#include <algorithm>
#include <deque>
#include <iterator>
#include <limits>
#include <map>
#include <set>

namespace neverd {

/// Ephemeral evidence for one current LowIR call occurrence. This proves only
/// that replacing x0 bits 63:1 with zero cannot affect this function's
/// observable behavior. It does not declare the callee's result type,
/// authenticate its runtime provider, certify its ABI or authorize machine-code
/// rewriting. Consumers must separately establish those facts and rerun this
/// proof before publishing a source projection; a saved certificate is not
/// authority.
struct SourceBooleanResultCertificate {
  const LowFunc *Function = nullptr;
  SourceCallOccurrenceKey Site;
};

namespace source_boolean_result_detail {
using Bits = std::map<uint64_t, uint8_t>;
constexpr size_t MaxFacts = 4096;

inline uint8_t lookup(const Bits &Values, uint64_t Byte) {
  const auto It = Values.find(Byte);
  return It == Values.end() ? 0 : It->second;
}

inline void put(Bits &Values, uint64_t Byte, uint8_t Mask) {
  if (Mask)
    Values[Byte] = Mask;
  else
    Values.erase(Byte);
}

inline bool validValue(const NdVar &Value) {
  return Value.Size && Value.Size <= 64 &&
         (Value.isConst() || ((Value.isReg() || Value.isTemp()) &&
                              Value.Offset <= UINT64_MAX - Value.Size));
}

inline bool ordinaryBinary(NdOp Opcode) {
  switch (Opcode) {
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_MULT:
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_SLESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::INT_CARRY:
  case NdOp::INT_SOVF:
  case NdOp::INT_SBOR:
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
    return true;
  default:
    return false;
  }
}

struct Transfer {
  const std::map<SourceCallOccurrenceKey, SourceBooleanOtherCallContract>
      &Calls;
  const SourceCallOccurrenceKey &Selected;
  const std::vector<SourceABIParameter> &SelectedInputs;
  const SourceFunctionTypeHint &EntrySignature;
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::AArch64);
  std::set<uint64_t> ArchitectureBytes;
  std::set<uint64_t> Preserved;
  size_t Remaining = 262144;

  Transfer(const std::map<SourceCallOccurrenceKey,
                          SourceBooleanOtherCallContract> &Calls,
           const SourceCallOccurrenceKey &Selected,
           const std::vector<SourceABIParameter> &SelectedInputs,
           const SourceFunctionTypeHint &EntrySignature)
      : Calls(Calls), Selected(Selected), SelectedInputs(SelectedInputs),
        EntrySignature(EntrySignature) {
    auto Add = [&](uint64_t Offset, unsigned Width) {
      for (unsigned I = 0; I < Width; ++I)
        ArchitectureBytes.insert(Offset + I);
    };
    for (const auto Register : TRI.GeneralRegs)
      Add(Register, TRI.FullRegWidth);
    // AArch64's scalar and vector banks are defined by its complete subregister
    // table; GeneralRegs and VecRegWidth are optional on this target.
    for (const auto &Register : TRI.SubRegs)
      Add(Register.WideRegOff, Register.WideSize);
    for (uint64_t Flag = TRI.FlagRangeStart; Flag <= TRI.FlagRangeEnd; ++Flag)
      Add(Flag, 1);
    for (const auto Register :
         {TRI.StackPointer, TRI.FramePointer, TRI.LinkRegister})
      Add(Register, TRI.PointerSize);
    for (const auto &Range : TRI.callPreservedRanges(BinaryFormat::MachO))
      for (unsigned I = 0; I < Range.Bytes; ++I)
        Preserved.insert(Range.Offset + I);
    for (unsigned I = 0; I < 8; ++I)
      Preserved.insert(TRI.StackPointer + I);
  }

  bool validValue(const NdVar &Value) const {
    if (!source_boolean_result_detail::validValue(Value))
      return false;
    if (Value.isReg())
      for (unsigned I = 0; I < Value.Size; ++I)
        if (!ArchitectureBytes.count(Value.Offset + I))
          return false;
    return true;
  }

  bool operator()(const LowBlock &Block, Bits &Registers) {
    Bits Temps;
    std::set<uint64_t> DefinedTemps;
    va_t Instruction = InvalidVA;
    auto Read = [&](const NdVar &V, unsigned Byte) {
      return V.isConst()
                 ? uint8_t(0)
                 : lookup(V.isReg() ? Registers : Temps, V.Offset + Byte);
    };
    auto Any = [&](const NdVar &V) {
      for (unsigned I = 0; I < V.Size; ++I)
        if (Read(V, I))
          return true;
      return false;
    };
    auto ObserveLocation = [&](const SourceABIValueLocation &Location) {
      if (Location.Kind == SourceABICarrierKind::None)
        return true;
      if (Location.Kind != SourceABICarrierKind::IntegerRegister &&
          Location.Kind != SourceABICarrierKind::FloatingRegister)
        return false;
      const auto Width = Location.ExtendTo32Bits ? 4U : Location.ValueBytes;
      const auto Value = NdVar::reg(Location.RegisterOffset, Width);
      return validValue(Value) && !Any(Value);
    };
    for (const auto &Op : Block.Ops) {
      if (Op.Addr != Instruction) {
        Temps.clear();
        DefinedTemps.clear();
        Instruction = Op.Addr;
      }
      if (Op.NumInputs > 6 || Op.Output.Size > 64 ||
          (Op.Output.Size && (!validValue(Op.Output) || Op.Output.isConst())) ||
          Op.MemoryOrdering != NdMemoryOrdering::None ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return false;
      size_t Cost = 1 + Op.Output.Size;
      bool InputDiffers = false;
      for (unsigned I = 0; I < Op.NumInputs; ++I) {
        const auto &Input = Op.Inputs[I];
        if (!validValue(Input))
          return false;
        Cost += Input.Size;
        for (unsigned J = 0; Input.isTemp() && J < Input.Size; ++J)
          if (!DefinedTemps.count(Input.Offset + J))
            return false;
        InputDiffers |= Any(Input);
      }
      if (Cost > Remaining)
        return false;
      Remaining -= Cost;
      if (Op.Opcode == NdOp::CALL) {
        const auto Key = sourceCallOccurrenceKey(Op);
        if (!Key || InputDiffers ||
            Op.Output != NdVar::reg(TRI.IntReturnReg, 8))
          return false;
        const bool IsSelected = !(*Key < Selected) && !(Selected < *Key);
        const auto It = Calls.find(*Key);
        if (!IsSelected && It == Calls.end())
          return false;
        // SP is an implicit input to every callee, even with no stack-passed
        // arguments. Its frame saves and loads can observe a different stack
        // before the caller restores SP, so later restoration is insufficient.
        if (Any(NdVar::reg(TRI.StackPointer, 8)))
          return false;
        const auto Parameters =
            IsSelected ? SelectedInputs
                       : sourceABIParameters(*It->second.Signature);
        for (const auto &Parameter : Parameters) {
          const auto &Location = Parameter.Location;
          // Differing stores are forbidden and SP is identical above, so
          // stack arguments observe the same memory.
          if (Location.Kind != SourceABICarrierKind::Stack &&
              !ObserveLocation(Location))
            return false;
        }
        // Even with identical logical arguments, incidental caller-saved
        // outputs can copy any other incoming physical register. Preserve the
        // ABI's exact saved prefixes, but propagate a live difference to every
        // volatile architectural byte, including flags and unused scratch.
        // This also applies before a repeated selected call in a loop.
        if (!Registers.empty()) {
          if (Remaining < ArchitectureBytes.size())
            return false;
          Remaining -= ArchitectureBytes.size();
          for (const auto Byte : ArchitectureBytes)
            if (!Preserved.count(Byte))
              Registers[Byte] = 0xff;
        }
        if (IsSelected) {
          for (unsigned I = 0; I < 8; ++I)
            Registers[TRI.IntReturnReg + I] = I ? 0xff : 0xfe;
          continue;
        }
        // Only complete declared result bytes are independent of incidental
        // machine inputs. Narrow return padding keeps the propagated
        // difference.
        auto ClearResult = [&](const SourceABIValueLocation &Location) {
          if (Location.Kind == SourceABICarrierKind::None)
            return;
          const auto Width = Location.ExtendTo32Bits ? 4U : Location.ValueBytes;
          for (unsigned I = 0; I < Width; ++I)
            Registers.erase(Location.RegisterOffset + I);
        };
        ClearResult(It->second.Signature->ReturnLocation);
        for (const auto &Location : It->second.Signature->ReturnComponents)
          ClearResult(Location);
        continue;
      }
      if (Op.Opcode == NdOp::RETURN) {
        if (InputDiffers || !ObserveLocation(EntrySignature.ReturnLocation))
          return false;
        for (const auto &Location : EntrySignature.ReturnComponents)
          if (!ObserveLocation(Location))
            return false;
        for (uint64_t Byte : Preserved)
          if (lookup(Registers, Byte))
            return false;
        if (Any(NdVar::reg(TRI.LinkRegister, 8)))
          return false;
        continue;
      }
      if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR) {
        if (InputDiffers)
          return false;
        continue;
      }
      std::vector<uint8_t> Value(Op.Output.Size, InputDiffers ? 0xff : 0);
      if (Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
          Op.Opcode == NdOp::INT_SEXT) {
        if (Op.NumInputs != 1 || !Op.Output.Size ||
            (Op.Opcode == NdOp::COPY ? Op.Output.Size > Op.Inputs[0].Size
                                     : Op.Output.Size < Op.Inputs[0].Size))
          return false;
        for (unsigned I = 0; I < Value.size(); ++I)
          Value[I] = I < Op.Inputs[0].Size ? Read(Op.Inputs[0], I)
                     : Op.Opcode == NdOp::INT_SEXT &&
                             (Read(Op.Inputs[0], Op.Inputs[0].Size - 1) & 0x80)
                         ? 0xff
                         : 0;
      } else if (Op.Opcode == NdOp::SUBBYTES) {
        if (Op.NumInputs != 2 || !Op.Inputs[1].isConst() ||
            Op.Inputs[1].Offset > Op.Inputs[0].Size ||
            Value.size() > Op.Inputs[0].Size - Op.Inputs[1].Offset)
          return false;
        for (unsigned I = 0; I < Value.size(); ++I)
          Value[I] = Read(Op.Inputs[0], Op.Inputs[1].Offset + I);
      } else if (Op.Opcode == NdOp::CONCAT) {
        if (Op.NumInputs != 2 ||
            Op.Inputs[0].Size + Op.Inputs[1].Size != Value.size())
          return false;
        for (unsigned I = 0; I < Value.size(); ++I)
          Value[I] = I < Op.Inputs[1].Size
                         ? Read(Op.Inputs[1], I)
                         : Read(Op.Inputs[0], I - Op.Inputs[1].Size);
      } else if (Op.Opcode == NdOp::INT_LEFT || Op.Opcode == NdOp::INT_RIGHT ||
                 Op.Opcode == NdOp::INT_ASHR) {
        if (Op.NumInputs != 2 || !Op.Output.Size ||
            Op.Inputs[0].Size != Op.Output.Size || !Op.Inputs[1].isConst() ||
            Op.Inputs[1].Size > 8 ||
            Op.Inputs[1].Offset >= uint64_t(Op.Output.Size) * 8)
          return false;
        const unsigned Bits = Op.Output.Size * 8;
        const unsigned Shift = Op.Inputs[1].Offset;
        std::fill(Value.begin(), Value.end(), 0);
        for (unsigned Bit = 0; Bit < Bits; ++Bit) {
          if (Op.Opcode == NdOp::INT_LEFT && Bit < Shift)
            continue;
          unsigned Source =
              Op.Opcode == NdOp::INT_LEFT ? Bit - Shift : Bit + Shift;
          if (Source >= Bits) {
            if (Op.Opcode != NdOp::INT_ASHR)
              continue;
            Source = Bits - 1;
          }
          if (Read(Op.Inputs[0], Source / 8) & (1U << (Source % 8)))
            Value[Bit / 8] |= 1U << (Bit % 8);
        }
      } else if (Op.Opcode == NdOp::INT_AND || Op.Opcode == NdOp::INT_OR ||
                 Op.Opcode == NdOp::INT_XOR) {
        if (Op.NumInputs != 2 || !Op.Output.Size)
          return false;
        for (unsigned I = 0; I < 2; ++I)
          if (!Op.Inputs[I].isConst() && Op.Inputs[I].Size < Op.Output.Size)
            return false;
        // Bitwise operations may truncate to the destination width, including
        // TBZ's one-byte predicate extracted from a full W register.
        if (Op.Inputs[0].Size >= Value.size() &&
            Op.Inputs[1].Size >= Value.size()) {
          const bool Constant =
              (Op.Inputs[0].isConst() || Op.Inputs[1].isConst()) &&
              Value.size() <= 8;
          const unsigned Index = Op.Inputs[0].isConst() ? 0 : 1;
          for (unsigned I = 0; I < Value.size(); ++I) {
            Value[I] = Read(Op.Inputs[0], I) | Read(Op.Inputs[1], I);
            if (Constant && Op.Opcode != NdOp::INT_XOR) {
              const uint8_t Byte = Op.Inputs[Index].Offset >> (I * 8);
              Value[I] &= Op.Opcode == NdOp::INT_AND ? Byte : uint8_t(~Byte);
            }
          }
        }
      } else if (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) {
        const auto Memory = lowMemoryOperands(Op);
        if (!Memory.Complete || !Memory.Address || Memory.Address->Size != 8 ||
            InputDiffers ||
            (Op.Opcode == NdOp::LOAD && Op.NumInputs != 1 &&
             Op.NumInputs != 2) ||
            (Op.Opcode == NdOp::STORE && Op.NumInputs != 2 &&
             Op.NumInputs != 3) ||
            (Op.Opcode == NdOp::STORE && Op.Output.Size))
          return false;
        std::fill(Value.begin(), Value.end(), 0);
      } else if (ordinaryBinary(Op.Opcode)) {
        if (Op.NumInputs != 2 || !Op.Output.Size)
          return false;
        if (Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB ||
            Op.Opcode == NdOp::INT_MULT) {
          for (unsigned I = 0; I < 2; ++I)
            if (Op.Inputs[I].isConst() ? Op.Inputs[I].Size > Op.Output.Size
                                       : Op.Inputs[I].Size != Op.Output.Size)
              return false;
        } else if (Op.Output.Size != 1 ||
                   Op.Inputs[0].Size != Op.Inputs[1].Size)
          return false;
      } else if (Op.Opcode == NdOp::BOOL_NOT || Op.Opcode == NdOp::INT_NOT ||
                 Op.Opcode == NdOp::INT_NEGATE) {
        if (Op.NumInputs != 1 || !Op.Output.Size ||
            Op.Output.Size != Op.Inputs[0].Size)
          return false;
      } else if (Op.Opcode != NdOp::NOP || Op.NumInputs || Op.Output.Size)
        return false;
      if (Op.Output.Size) {
        auto &Output = Op.Output.isReg() ? Registers : Temps;
        if (Op.Output.isReg() &&
            TRI.writeZeroExtends(Op.Output.Offset, Op.Output.Size)) {
          const auto [Offset, Width] =
              TRI.findWideReg(Op.Output.Offset, Op.Output.Size);
          for (unsigned I = 0; I < Width; ++I)
            Output.erase(Offset + I);
        }
        for (unsigned I = 0; I < Value.size(); ++I) {
          put(Output, Op.Output.Offset + I, Value[I]);
          if (Op.Output.isTemp())
            DefinedTemps.insert(Op.Output.Offset + I);
        }
      }
      if (Registers.size() > MaxFacts || Temps.size() > MaxFacts ||
          DefinedTemps.size() > MaxFacts)
        return false;
    }
    return true;
  }
};
} // namespace source_boolean_result_detail

/// The selected call has a separate raw-i1 result contract; ordinary calls and
/// entry retain complete, independently validated source ABIs. The shared
/// LowIR call-occurrence owner supplies every exact identity.
/// This owner checks their physical shape and exact LowIR occurrence, then
/// proves non-observation of the replaced bits on every physical CFG path.
/// Indirect calls, tail exits, exceptions, differing memory writes and unknown
/// operations remain outside this deliberately bounded projection.
inline std::optional<SourceBooleanResultCertificate>
proveSourceBooleanResultNormalization(
    const LowFunc &Function, Arch Architecture,
    const SourceCallOccurrenceKey &Selected,
    const SourceBooleanResultContract &SelectedContract,
    const std::map<SourceCallOccurrenceKey, SourceBooleanOtherCallContract>
        &Calls,
    const SourceFunctionTypeHint &EntrySignature) {
  using namespace source_boolean_result_detail;
  std::string Error;
  const auto SelectedInputs = sourceBooleanInputParameters(SelectedContract);
  if (!SelectedInputs || Architecture != Arch::AArch64 || Function.Entry % 4 ||
      Function.Blocks.empty() || Function.Blocks.size() > 256 ||
      Calls.size() > 8192 || EntrySignature.Architecture != Architecture ||
      !validateSourceABI(EntrySignature, Error) ||
      EntrySignature.ReturnLocation.Kind ==
          SourceABICarrierKind::IndirectResultPointer ||
      Selected.Opcode != NdOp::CALL || !Selected.StaticTarget ||
      *Selected.StaticTarget == Function.Entry || Calls.count(Selected))
    return std::nullopt;
  for (const auto &[Key, Contract] : Calls)
    if (Key.Opcode != NdOp::CALL || !Key.StaticTarget ||
        *Key.StaticTarget == Function.Entry || Contract.DoesNotReturn ||
        !Contract.Signature ||
        Contract.Signature->Architecture != Architecture ||
        Contract.Signature->ReturnLocation.Kind ==
            SourceABICarrierKind::IndirectResultPointer ||
        !validateSourceABI(*Contract.Signature, Error))
      return std::nullopt;
  std::map<int, size_t> Ids;
  std::map<va_t, size_t> Starts;
  std::optional<size_t> Entry;
  std::set<SourceCallOccurrenceKey> SeenCalls;
  size_t Count = 0;
  for (size_t I = 0; I < Function.Blocks.size(); ++I) {
    const auto &B = Function.Blocks[I];
    if (B.Id < 0 || !Ids.emplace(B.Id, I).second ||
        !Starts.emplace(B.StartAddr, I).second || B.StartAddr % 4 ||
        B.EndAddr <= B.StartAddr || (B.EndAddr - B.StartAddr) % 4 ||
        B.EndAddr - B.StartAddr > 32768 || B.Ops.empty() ||
        !B.ExceptionalPreds.empty() || !B.ExceptionalSuccs.empty())
      return std::nullopt;
    if (B.StartAddr == Function.Entry)
      Entry = I;
    std::set<va_t> Instructions;
    va_t Previous = B.StartAddr;
    for (const auto &Op : B.Ops) {
      if (++Count > 8192 || Op.Addr < Previous || Op.Addr < B.StartAddr ||
          Op.Addr >= B.EndAddr || Op.Addr % 4 ||
          Op.Opcode == NdOp::INDIR_CALL || Op.Opcode == NdOp::INDIR_BR)
        return std::nullopt;
      Previous = Op.Addr;
      Instructions.insert(Op.Addr);
      if (Op.Opcode == NdOp::CALL) {
        const auto Key = sourceCallOccurrenceKey(Op);
        const bool IsSelected = Key && !(*Key < Selected) && !(Selected < *Key);
        if (!Key || (!IsSelected && !Calls.count(*Key)) ||
            !SeenCalls.insert(*Key).second)
          return std::nullopt;
      }
      if ((Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
           Op.Opcode == NdOp::RETURN) &&
          &Op != &B.Ops.back())
        return std::nullopt;
    }
    if (Instructions.size() != (B.EndAddr - B.StartAddr) / 4)
      return std::nullopt;
  }
  if (!Entry || SeenCalls.size() != Calls.size() + 1)
    return std::nullopt;
  for (auto It = Starts.begin(); It != Starts.end(); ++It)
    if (auto Next = std::next(It);
        Next != Starts.end() &&
        Function.Blocks[It->second].EndAddr > Next->first)
      return std::nullopt;
  std::vector<std::set<size_t>> Successors(Function.Blocks.size()),
      Predecessors(Function.Blocks.size());
  bool HasReturn = false;
  for (size_t I = 0; I < Function.Blocks.size(); ++I) {
    const auto &B = Function.Blocks[I];
    std::set<size_t> Physical;
    auto AddAddress = [&](va_t Address) {
      const auto It = Starts.find(Address);
      if (It == Starts.end())
        return false;
      Physical.insert(It->second);
      return true;
    };
    const auto &Last = B.Ops.back();
    if (Last.Opcode == NdOp::RETURN) {
      const auto &TRI = getTargetRegInfo(Architecture);
      if (Last.NumInputs != 1 || Last.Output.Size ||
          Last.Inputs[0] != NdVar::reg(TRI.LinkRegister, 8))
        return std::nullopt;
      HasReturn = true;
    } else if (Last.Opcode == NdOp::BRANCH || Last.Opcode == NdOp::COND_BR) {
      if (Last.NumInputs != (Last.Opcode == NdOp::BRANCH ? 1 : 2) ||
          Last.Output.Size || !Last.Inputs[0].isConst() ||
          Last.Inputs[0].Size != 8 || !AddAddress(Last.Inputs[0].Offset) ||
          (Last.Opcode == NdOp::COND_BR && !AddAddress(B.EndAddr)))
        return std::nullopt;
    } else if (!AddAddress(B.EndAddr))
      return std::nullopt;
    for (int Id : B.Succs) {
      const auto It = Ids.find(Id);
      if (It == Ids.end() || !Successors[I].insert(It->second).second)
        return std::nullopt;
      Predecessors[It->second].insert(I);
    }
    if (Successors[I] != Physical)
      return std::nullopt;
  }
  if (!HasReturn || !Predecessors[*Entry].empty())
    return std::nullopt;
  for (size_t I = 0; I < Function.Blocks.size(); ++I) {
    std::set<size_t> Supplied;
    for (int Id : Function.Blocks[I].Preds) {
      const auto It = Ids.find(Id);
      if (It == Ids.end() || !Supplied.insert(It->second).second)
        return std::nullopt;
    }
    if (Supplied != Predecessors[I])
      return std::nullopt;
  }
  Transfer Step(Calls, Selected, *SelectedInputs, EntrySignature);
  std::vector<std::optional<Bits>> Incoming(Function.Blocks.size());
  Incoming[*Entry] = Bits{};
  std::deque<size_t> Pending{*Entry};
  while (!Pending.empty()) {
    const auto I = Pending.front();
    Pending.pop_front();
    auto Output = *Incoming[I];
    if (!Step(Function.Blocks[I], Output))
      return std::nullopt;
    for (size_t Next : Successors[I]) {
      auto Joined = Incoming[Next].value_or(Bits{});
      for (const auto &[Byte, Mask] : Output)
        Joined[Byte] |= Mask;
      if (Joined.size() > MaxFacts)
        return std::nullopt;
      if (!Incoming[Next] || Joined != *Incoming[Next]) {
        Incoming[Next] = std::move(Joined);
        Pending.push_back(Next);
      }
    }
  }
  if (std::any_of(Incoming.begin(), Incoming.end(),
                  [](const auto &State) { return !State; }))
    return std::nullopt;
  return SourceBooleanResultCertificate{&Function, Selected};
}
} // namespace neverd
#endif

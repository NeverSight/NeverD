#include "NativeSourcePreservation.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <optional>
#include <set>

namespace neverd {
std::optional<NativeSourceCallKey> nativeSourceCallKey(const LowOp &Operation) {
  return sourceCallOccurrenceKey(Operation);
}

namespace {
constexpr int64_t MaxFrame = 1 << 20;
constexpr size_t MaxFacts = 4096;

bool stackCheckTermination(const NativeSourceCallContract &Contract,
                           Arch Architecture) {
  if (Contract.Termination !=
          NativeSourceCallContract::TerminationKind::StackCheckFailure ||
      Architecture != Arch::AArch64 || !Contract.Signature ||
      Contract.Signature->Origin !=
          SourceFunctionTypeHint::OriginKind::DarwinRuntime ||
      !Contract.ReadOnlyFrameParameters.empty() ||
      !Contract.WritableFrameParameters.empty())
    return false;
  SourceFunctionTypeHint Expected;
  Expected.Origin = SourceFunctionTypeHint::OriginKind::DarwinRuntime;
  Expected.ReturnType = NdType::makeVoid();
  std::string Error;
  return assignDarwinScalarSourceABI(Expected, Architecture, Error) &&
         equalSourceABIs(*Contract.Signature, Expected);
}

// Byte identities make partial writes and overlapping spills explicit. A
// frame address needs all eight ordered bytes before it can name a stack slot.
struct ByteFact {
  enum Kind { Unknown, Entry, Frame } TheKind = Unknown;
  int64_t Value = 0;
  unsigned Index = 0;
  bool MayBeFrame = false;
  bool operator==(const ByteFact &) const = default;
};
using RegisterFacts = std::map<uint64_t, ByteFact>;
using StackFacts = std::map<int64_t, ByteFact>;
struct State {
  RegisterFacts Registers;
  StackFacts Stack;
  bool operator==(const State &) const = default;
};

template <typename Key>
ByteFact lookup(const std::map<Key, ByteFact> &Facts, Key Offset) {
  const auto Found = Facts.find(Offset);
  return Found == Facts.end() ? ByteFact{} : Found->second;
}

template <typename Key>
void put(std::map<Key, ByteFact> &Facts, Key Offset, ByteFact Fact) {
  if (Fact.TheKind == ByteFact::Unknown && !Fact.MayBeFrame)
    Facts.erase(Offset);
  else
    Facts[Offset] = Fact;
}

template <typename Key>
bool meet(std::map<Key, ByteFact> &Left, const std::map<Key, ByteFact> &Right,
          size_t &Remaining) {
  if (Remaining < Left.size() + Right.size())
    return false;
  Remaining -= Left.size() + Right.size();
  auto Result = Left;
  for (const auto &[Offset, Fact] : Right)
    Result.try_emplace(Offset, ByteFact{});
  for (auto &[Offset, Fact] : Result) {
    const auto Other = lookup(Right, Offset);
    if (Fact != Other)
      Fact = {ByteFact::Unknown, 0, 0, Fact.MayBeFrame || Other.MayBeFrame};
  }
  std::erase_if(Result, [](const auto &Item) {
    return Item.second.TheKind == ByteFact::Unknown && !Item.second.MayBeFrame;
  });
  Left = std::move(Result);
  return Left.size() <= MaxFacts;
}

std::optional<int64_t> signedConstant(const NdVar &Value) {
  if (!Value.isConst() || !Value.Size || Value.Size > 8 ||
      isAddressProvenance(Value.Provenance))
    return std::nullopt;
  const unsigned Bits = Value.Size * 8U;
  const uint64_t Mask = Bits == 64 ? UINT64_MAX : (uint64_t{1} << Bits) - 1;
  const uint64_t Number = Value.Offset & Mask;
  // Inputs are read at their own width, then used by an eight-byte ADD/SUB.
  // A narrow constant's high bit does not request sign extension.
  return Bits == 64 && (Number & (uint64_t{1} << 63))
             ? -1 - static_cast<int64_t>((~Number) & Mask)
             : static_cast<int64_t>(Number);
}

class PreservationProof {
public:
  PreservationProof(Arch Architecture, const NativeSourceCalls &Calls,
                    bool TerminalOnly = false,
                    std::set<int64_t> IncomingStackSlots = {})
      : Architecture(Architecture), Calls(Calls),
        TRI(getTargetRegInfo(Architecture)), TerminalOnly(TerminalOnly),
        TrackStackArguments(TerminalOnly),
        IncomingStackSlots(std::move(IncomingStackSlots)) {
    if (Architecture == Arch::AArch64)
      for (const auto &[Site, Contract] : Calls)
        if (Contract.Signature)
          for (const auto &Parameter : Contract.Signature->Parameters)
            TrackStackArguments |=
                Parameter.Location.Kind == SourceABICarrierKind::Stack;
    for (const auto &Range : TRI.callPreservedRanges(BinaryFormat::MachO))
      for (unsigned I = 0; I < Range.Bytes; ++I)
        Preserved.insert(Range.Offset + I);
    for (uint64_t Offset : Preserved)
      Initial.Registers[Offset] = {ByteFact::Entry,
                                   static_cast<int64_t>(Offset)};
    if (TRI.LinkRegister)
      for (unsigned I = 0; I < 8; ++I)
        Initial.Registers[TRI.LinkRegister + I] = {
            ByteFact::Entry, static_cast<int64_t>(TRI.LinkRegister + I)};
    for (unsigned I = 0; I < 8; ++I)
      Initial.Registers[TRI.StackPointer + I] = {ByteFact::Frame, 0, I, true};
  }

  State Initial;
  size_t Remaining = 262144;
  bool DidTerminate = false;

  bool transfer(const LowBlock &Block, State &Current, bool CheckExits,
                std::set<uint64_t> *UsedEntryRegisters = nullptr) {
    RegisterFacts Temps;
    // Outgoing arguments require a complete private write in this same block.
    // This local set deliberately does not infer definitions across CFG edges.
    std::set<int64_t> WrittenStack;
    va_t Instruction = InvalidVA;
    auto Read = [&](const NdVar &Value, unsigned Byte) {
      if (Value.isReg())
        return lookup(Current.Registers, Value.Offset + Byte);
      if (Value.isTemp())
        return lookup(Temps, Value.Offset + Byte);
      return ByteFact{};
    };
    auto FrameOffset = [&](const NdVar &Value) -> std::optional<int64_t> {
      if (Value.Size != 8)
        return std::nullopt;
      const auto First = Read(Value, 0);
      if (First.TheKind != ByteFact::Frame)
        return std::nullopt;
      for (unsigned I = 0; I < 8; ++I)
        if (Read(Value, I) != ByteFact{ByteFact::Frame, First.Value, I, true})
          return std::nullopt;
      return First.Value;
    };
    auto IsRestored = [&] {
      return std::all_of(Initial.Registers.begin(), Initial.Registers.end(),
                         [&](const auto &Item) {
                           return lookup(Current.Registers, Item.first) ==
                                  Item.second;
                         });
    };
    for (size_t Index = 0; Index < Block.Ops.size(); ++Index) {
      const auto &Op = Block.Ops[Index];
      if (Op.Addr != Instruction) {
        Temps.clear();
        Instruction = Op.Addr;
      }
      if (Op.NumInputs > 6 || Op.Output.Size > 64 ||
          Op.Opcode == NdOp::INTRINSIC ||
          Op.MemoryOrdering != NdMemoryOrdering::None ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return false;
      size_t Cost = 1 + Op.Output.Size;
      bool MayBeFrame = false;
      for (unsigned I = 0; I < Op.NumInputs; ++I) {
        if (Op.Inputs[I].Size > 64 ||
            ((Op.Inputs[I].isReg() || Op.Inputs[I].isTemp()) &&
             Op.Inputs[I].Offset > UINT64_MAX - Op.Inputs[I].Size))
          return false;
        Cost += Op.Inputs[I].Size;
        for (unsigned J = 0; J < Op.Inputs[I].Size; ++J)
          MayBeFrame |= Read(Op.Inputs[I], J).MayBeFrame;
      }
      if (Remaining < Cost || Op.Output.Offset > UINT64_MAX - Op.Output.Size)
        return false;
      Remaining -= Cost;
      if (UsedEntryRegisters && Op.Opcode != NdOp::COPY &&
          Op.Opcode != NdOp::RETURN) {
        std::optional<LowMemoryOperandView> Memory;
        std::optional<int64_t> MemoryAddress;
        if (Op.Opcode == NdOp::STORE) {
          Memory = lowMemoryOperands(Op);
          if (!Memory->Complete || !Memory->Address || !Memory->StoredValue)
            return false;
          MemoryAddress = FrameOffset(*Memory->Address);
        }
        for (unsigned InputIndex = 0; InputIndex < Op.NumInputs;
             ++InputIndex) {
          const auto &Input = Op.Inputs[InputIndex];
          if ((!Input.isReg() && !Input.isTemp()) || Input.Size != 8 ||
              (MemoryAddress && Memory->StoredValue &&
               Input == *Memory->StoredValue))
            continue;
          const auto First = Read(Input, 0);
          if (First.TheKind != ByteFact::Entry || First.Value < 0)
            continue;
          bool Complete = true;
          for (unsigned I = 0; I < Input.Size; ++I)
            Complete &= Read(Input, I) ==
                        ByteFact{ByteFact::Entry, First.Value + I};
          if (Complete)
            UsedEntryRegisters->insert(static_cast<uint64_t>(First.Value));
        }
      }
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        const auto Key = nativeSourceCallKey(Op);
        if (!Key)
          return false;
        const auto Found = Calls.find(*Key);
        if (Found == Calls.end())
          return false;
        for (unsigned I = 0; I < Op.Inputs[0].Size; ++I)
          if (Read(Op.Inputs[0], I).MayBeFrame)
            return false;
        if (!Found->second.Signature)
          return false;
        if (Found->second.terminates() && !TerminalOnly &&
            !stackCheckTermination(Found->second, Architecture))
          return false;
        if (Found->second.Termination ==
                NativeSourceCallContract::TerminationKind::StackCheckFailure &&
            (TerminalOnly || Index + 1 != Block.Ops.size() ||
             !Block.Succs.empty()))
          return false;
        const auto &Signature = *Found->second.Signature;
        const bool Tail = Index + 1 < Block.Ops.size() &&
                          Block.Ops[Index + 1].Opcode == NdOp::RETURN &&
                          Block.Ops[Index + 1].Addr == Op.Addr;
        const auto SP = FrameOffset(NdVar::reg(TRI.StackPointer, 8));
        if (!SP || *SP > 0 || *SP < -MaxFrame)
          return false;
        if (Tail) {
          if (CheckExits && !IsRestored())
            return false;
        } else if ((*SP + (Architecture == Arch::X64 ? 8 : 0)) % 16 != 0) {
          return false;
        }
        std::vector<std::pair<int64_t, size_t>> WritableFrameRanges;
        // Inspect the same validated physical members used by call lowering.
        // A record has no single carrier; checking only its primary location
        // would reject valid records or miss a frame address in a later member.
        for (const auto &Physical : sourceABIParameters(Signature)) {
          const size_t ParameterIndex = Physical.ParameterIndex;
          const auto &Parameter = Signature.Parameters[ParameterIndex];
          const auto &Location = Physical.Location;
          if (Location.Kind == SourceABICarrierKind::Stack) {
            const bool TerminalArgument =
                TerminalOnly && Found->second.terminates();
            const bool ReturningArgument =
                !TerminalOnly && Architecture == Arch::AArch64 && !Tail &&
                Parameter.Components.empty() && Parameter.Type &&
                (Parameter.Type->Kind == NdTypeKind::Int ||
                 Parameter.Type->Kind == NdTypeKind::Ptr ||
                 Parameter.Type->Kind == NdTypeKind::Float) &&
                Parameter.Type->Size == 8 && Location.ValueBytes == 8 &&
                Location.EntryStackOffset % 8 == 0;
            if ((!TerminalArgument && !ReturningArgument) ||
                !Location.ValueBytes || Location.ValueBytes > 64 ||
                Location.EntryStackOffset < 0 ||
                Location.EntryStackOffset > MaxFrame)
              return false;
            const int64_t Start = *SP + Location.EntryStackOffset;
            if (Start < *SP || Start > -int64_t(Location.ValueBytes))
              return false;
            for (unsigned I = 0; I < Location.ValueBytes; ++I)
              if (!WrittenStack.count(Start + I) ||
                  lookup(Current.Stack, Start + I).MayBeFrame)
                return false;
            // AAPCS64 permits the callee to overwrite its incoming argument
            // area. A by-value argument does not lend a frame pointer, but its
            // complete area, including padding before/between parameters,
            // cannot restore a spill after this call. These prefix ranges
            // together cover call SP through the final stacked argument.
            if (ReturningArgument)
              WritableFrameRanges.emplace_back(*SP, Start - *SP + 8);
            continue;
          }
          if ((Location.Kind != SourceABICarrierKind::IntegerRegister &&
               Location.Kind != SourceABICarrierKind::FloatingRegister) ||
              Location.ValueBytes > 64)
            return false;
          bool HasFrameByte = false;
          for (unsigned I = 0; I < Location.ValueBytes; ++I)
            HasFrameByte |=
                lookup(Current.Registers, Location.RegisterOffset + I)
                    .MayBeFrame;
          if (HasFrameByte) {
            // Borrowing certificates describe a complete scalar pointer, not
            // one member of a source record with the same parameter index.
            if (!Parameter.Components.empty())
              return false;
            const auto ReadOnly =
                Found->second.ReadOnlyFrameParameters.find(ParameterIndex);
            const auto Writable =
                Found->second.WritableFrameParameters.find(ParameterIndex);
            if ((ReadOnly ==
                     Found->second.ReadOnlyFrameParameters.end()) ==
                    (Writable ==
                     Found->second.WritableFrameParameters.end()) ||
                Location.Kind != SourceABICarrierKind::IntegerRegister ||
                Location.ValueBytes != 8)
              return false;
            const size_t BorrowedBytes =
                ReadOnly != Found->second.ReadOnlyFrameParameters.end()
                    ? ReadOnly->second
                    : Writable->second;
            if (!BorrowedBytes || BorrowedBytes > MaxFrame)
              return false;
            const auto Address = FrameOffset(
                NdVar::reg(Location.RegisterOffset, Location.ValueBytes));
            if (!Address || *Address < *SP ||
                *Address > -static_cast<int64_t>(BorrowedBytes))
              return false;
            if (Writable != Found->second.WritableFrameParameters.end())
              WritableFrameRanges.emplace_back(*Address, BorrowedBytes);
          }
          if (UsedEntryRegisters &&
              Location.Kind == SourceABICarrierKind::IntegerRegister &&
              Location.ValueBytes == 8) {
            const auto First =
                lookup(Current.Registers, Location.RegisterOffset);
            if (First.TheKind == ByteFact::Entry) {
              bool Complete = true;
              for (unsigned I = 0; I < Location.ValueBytes; ++I)
                Complete &=
                    lookup(Current.Registers, Location.RegisterOffset + I) ==
                    ByteFact{ByteFact::Entry, First.Value + I};
              if (Complete && First.Value >= 0)
                UsedEntryRegisters->insert(
                    static_cast<uint64_t>(First.Value));
            }
          }
        }
        for (const auto &[Address, Bytes] : WritableFrameRanges) {
          std::erase_if(Current.Stack, [&](const auto &Item) {
            return Item.first >= Address &&
                   Item.first - Address < static_cast<int64_t>(Bytes);
          });
          std::erase_if(WrittenStack, [&](int64_t Byte) {
            return Byte >= Address &&
                   Byte - Address < static_cast<int64_t>(Bytes);
          });
        }
        if (Found->second.terminates()) {
          DidTerminate = true;
          return true;
        }
        if (!Tail) {
          std::erase_if(Current.Registers, [&](const auto &Item) {
            return !Preserved.count(Item.first) ||
                   (TRI.LinkRegister && Item.first >= TRI.LinkRegister &&
                    Item.first - TRI.LinkRegister < 8);
          });
          // Unallocated bytes and the x86 red zone cannot retain a spill
          // through an ordinary call. The callee owns storage below call SP.
          std::erase_if(Current.Stack,
                        [&](const auto &Item) { return Item.first < *SP; });
          std::erase_if(WrittenStack,
                        [&](int64_t Byte) { return Byte < *SP; });
          Temps.clear();
        }
        continue;
      }
      if (Op.Opcode == NdOp::RETURN) {
        if (Index + 1 != Block.Ops.size() || !Block.Succs.empty() ||
            (CheckExits && !IsRestored()))
          return false;
        const bool TailReturn =
            Index && Block.Ops[Index - 1].Addr == Op.Addr &&
            (Block.Ops[Index - 1].Opcode == NdOp::CALL ||
             Block.Ops[Index - 1].Opcode == NdOp::INDIR_CALL);
        if (CheckExits && Architecture == Arch::AArch64 && !TailReturn) {
          if (Op.NumInputs != 1 || Op.Inputs[0].Size != 8)
            return false;
          for (unsigned I = 0; I < 8; ++I)
            if (Read(Op.Inputs[0], I) !=
                ByteFact{ByteFact::Entry,
                         static_cast<int64_t>(TRI.LinkRegister + I)})
              return false;
        }
        continue;
      }
      if (Op.Opcode == NdOp::ATOMIC_XCHG || Op.Opcode == NdOp::ATOMIC_ADD ||
          Op.Opcode == NdOp::ATOMIC_CMPXCHG || Op.Opcode == NdOp::INDIR_BR)
        return false;
      std::vector<ByteFact> Value(Op.Output.Size,
                                  {ByteFact::Unknown, 0, 0, MayBeFrame});
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
          Op.Inputs[0].Size == Op.Output.Size) {
        for (unsigned I = 0; I < Value.size(); ++I)
          Value[I] = Read(Op.Inputs[0], I);
      } else if (Op.Opcode == NdOp::INT_ZEXT && Op.NumInputs == 1 &&
                 Op.Inputs[0].Size <= Op.Output.Size) {
        // LowIR register normalization can widen a restored D register to Q.
        // The extension's upper bytes are new zeroes, but its low bytes retain
        // their exact entry or frame identity.
        for (unsigned I = 0; I < Op.Inputs[0].Size; ++I)
          Value[I] = Read(Op.Inputs[0], I);
      } else if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs == 2 &&
                 Op.Inputs[1].isConst() &&
                 Op.Inputs[1].Offset <= Op.Inputs[0].Size &&
                 Op.Output.Size <=
                     Op.Inputs[0].Size - Op.Inputs[1].Offset) {
        const auto Offset = static_cast<unsigned>(Op.Inputs[1].Offset);
        for (unsigned I = 0; I < Value.size(); ++I)
          Value[I] = Read(Op.Inputs[0], Offset + I);
      } else if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
                 Op.NumInputs == 2 && Op.Output.Size == 8) {
        unsigned Base = 0, Constant = 1;
        if (Op.Opcode == NdOp::INT_ADD && Op.Inputs[0].isConst())
          std::swap(Base, Constant);
        const auto Address = FrameOffset(Op.Inputs[Base]);
        const auto Delta = signedConstant(Op.Inputs[Constant]);
        if (Address && Delta && *Delta >= -MaxFrame && *Delta <= MaxFrame) {
          const int64_t Next =
              *Address + (Op.Opcode == NdOp::INT_SUB ? -*Delta : *Delta);
          if (Next >= -MaxFrame && Next <= MaxFrame)
            for (unsigned I = 0; I < 8; ++I)
              Value[I] = {ByteFact::Frame, Next, I, true};
        }
      } else if (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) {
        const auto Memory = lowMemoryOperands(Op);
        if (!Memory.Complete || !Memory.Address || Memory.AccessSize > 64)
          return false;
        const auto Address = FrameOffset(*Memory.Address);
        const auto SP = FrameOffset(NdVar::reg(TRI.StackPointer, 8));
        const bool IncomingRead = Address && Op.Opcode == NdOp::LOAD &&
                                  Memory.AccessSize == 8 &&
                                  IncomingStackSlots.count(*Address);
        if (IncomingRead && (!SP || *SP > 0))
          return false;
        if (Address && !IncomingRead) {
          if (!SP || *Address > -int64_t(Memory.AccessSize))
            return false;
          int64_t StackFloor = *SP;
          if (*Address < StackFloor) {
            // Some instructions, notably AArch64 pre-indexed stores, expose
            // their memory effects before the final SP write in LowIR. Accept
            // that newly allocated interval only when this same instruction
            // has one exact, final SP copy from an already known frame value.
            std::optional<size_t> StackWrite;
            for (size_t J = Index + 1;
                 J < Block.Ops.size() && Block.Ops[J].Addr == Op.Addr; ++J) {
              const auto &Future = Block.Ops[J];
              if (!Future.Output.isReg() || !Future.Output.Size ||
                  Future.Output.Offset >= TRI.StackPointer + 8 ||
                  Future.Output.Offset + Future.Output.Size <= TRI.StackPointer)
                continue;
              if (StackWrite || Future.Opcode != NdOp::COPY ||
                  Future.Output != NdVar::reg(TRI.StackPointer, 8) ||
                  Future.NumInputs != 1 || Future.Inputs[0].Size != 8)
                return false;
              StackWrite = J;
            }
            if (!StackWrite)
              return false;
            const auto &Source = Block.Ops[*StackWrite].Inputs[0];
            if (!Source.isTemp() || Source.Offset > UINT64_MAX - Source.Size)
              return false;
            for (size_t J = Index + 1; J < *StackWrite; ++J) {
              const auto &Output = Block.Ops[J].Output;
              if (Output.isTemp() && Output.Size &&
                  Output.Offset < Source.Offset + Source.Size &&
                  Source.Offset < Output.Offset + Output.Size)
                return false;
            }
            const auto AllocatedSP = FrameOffset(Source);
            if (!AllocatedSP || *AllocatedSP >= StackFloor ||
                *AllocatedSP < -MaxFrame)
              return false;
            StackFloor = *AllocatedSP;
          }
          if (*Address < StackFloor)
            return false;
        }
        if (Op.Opcode == NdOp::STORE) {
          if (!Memory.StoredValue)
            return false;
          // The initial spill domain carries entry bytes, not addresses of
          // this frame. Reject these stores even into another private slot,
          // so invalidating memory facts cannot lose a possible escape.
          for (unsigned I = 0; I < Memory.AccessSize; ++I)
            if (Read(*Memory.StoredValue, I).MayBeFrame)
              return false;
          if (Address) {
            if (*Address < -MaxFrame)
              return false;
            for (unsigned I = 0; I < Memory.AccessSize; ++I)
              put(Current.Stack, *Address + I, Read(*Memory.StoredValue, I));
            if (TrackStackArguments)
              for (unsigned I = 0; I < Memory.AccessSize; ++I)
                WrittenStack.insert(*Address + I);
          } else
            // A value with any frame-derived byte may be a partial or
            // inexact alias of private storage. A completely non-frame
            // address names storage outside this invocation's private frame,
            // so the write cannot invalidate its exact spill identities.
            for (unsigned I = 0; I < Memory.Address->Size; ++I)
              if (Read(*Memory.Address, I).MayBeFrame)
                return false;
        } else {
          for (unsigned I = 0; I < Value.size(); ++I)
            // Incoming arguments are external values. Even if a slot happens
            // to contain a saved register's bits, it cannot certify restoration
            // of this invocation's preserved state or a private-frame address.
            Value[I] = Address && !IncomingRead
                           ? lookup(Current.Stack, *Address + I)
                           : ByteFact{};
        }
      }
      if (Op.Output.isReg() || Op.Output.isTemp()) {
        auto &Output = Op.Output.isReg() ? Current.Registers : Temps;
        if (Op.Output.isReg() &&
            TRI.writeZeroExtends(Op.Output.Offset, Op.Output.Size)) {
          const auto [Offset, Bytes] =
              TRI.findWideReg(Op.Output.Offset, Op.Output.Size);
          for (unsigned I = 0; I < Bytes; ++I)
            Output.erase(Offset + I);
        }
        for (unsigned I = 0; I < Value.size(); ++I)
          put(Output, Op.Output.Offset + I, Value[I]);
      } else if (Op.Output.Size) {
        return false;
      }
      if (Current.Registers.size() > MaxFacts ||
          Current.Stack.size() > MaxFacts || Temps.size() > MaxFacts ||
          WrittenStack.size() > MaxFacts)
        return false;
    }
    return true;
  }

private:
  Arch Architecture;
  const NativeSourceCalls &Calls;
  const TargetRegInfo &TRI;
  std::set<uint64_t> Preserved;
  bool TerminalOnly;
  bool TrackStackArguments;
  std::set<int64_t> IncomingStackSlots;
};
} // namespace

bool restoresNativeSourceState(const LowFunc &Function, Arch Architecture,
                               const NativeSourceCalls &Calls,
                               std::set<uint64_t> *UsedEntryRegisters,
                               const SourceFunctionTypeHint *EntrySignature) {
  const size_t Count = Function.Blocks.size();
  if (!Count || Count > 16384 ||
      (Architecture != Arch::AArch64 && Architecture != Arch::X64))
    return false;
  for (const auto &[Site, Contract] : Calls) {
    const auto *Signature = Contract.Signature;
    std::string Error;
    // Hidden result storage needs its own bounded frame-write proof before
    // this analysis can treat it as preserving saved machine state.
    if ((Contract.terminates() &&
         !stackCheckTermination(Contract, Architecture)) ||
        !Signature || Signature->Architecture != Architecture ||
        Signature->ReturnLocation.Kind ==
            SourceABICarrierKind::IndirectResultPointer ||
        !validateSourceABI(*Signature, Error))
      return false;
  }
  std::map<int, size_t> Blocks;
  std::optional<size_t> Entry;
  bool HasAddresses = false;
  for (size_t I = 0; I < Count; ++I) {
    const auto &Block = Function.Blocks[I];
    if (Block.Id < 0 || !Blocks.emplace(Block.Id, I).second ||
        !Block.ExceptionalSuccs.empty() || !Block.ExceptionalPreds.empty())
      return false;
    HasAddresses |= Block.StartAddr != 0;
    if (Block.StartAddr == Function.Entry) {
      if (Entry)
        return false;
      Entry = I;
    }
  }
  if (!Entry && !HasAddresses && Blocks.count(0))
    Entry = Blocks.at(0);
  if (!Entry)
    return false;
  std::set<int64_t> IncomingStackSlots;
  if (EntrySignature) {
    std::string Error;
    if (EntrySignature->Architecture != Architecture ||
        EntrySignature->Origin !=
            SourceFunctionTypeHint::OriginKind::NativeAnalysis ||
        !validateSourceABI(*EntrySignature, Error))
      return false;
    for (const auto &Parameter : EntrySignature->Parameters) {
      const auto &Location = Parameter.Location;
      if (Location.Kind != SourceABICarrierKind::Stack)
        continue;
      if (Architecture != Arch::AArch64 || !Parameter.Components.empty() ||
          !Parameter.Type || Parameter.Type->Size != 8 ||
          (Parameter.Type->Kind != NdTypeKind::Int &&
           Parameter.Type->Kind != NdTypeKind::Ptr) ||
          Location.ValueBytes != 8 || Location.EntryStackOffset < 0 ||
          Location.EntryStackOffset > MaxFrame - 8 ||
          Location.EntryStackOffset % 8 != 0 ||
          !IncomingStackSlots.insert(Location.EntryStackOffset).second)
        return false;
    }
  }
  PreservationProof Proof(Architecture, Calls, false,
                          std::move(IncomingStackSlots));
  std::vector<std::set<size_t>> Preds(Count), Succs(Count);
  for (size_t I = 0; I < Count; ++I)
    for (int Id : Function.Blocks[I].Succs) {
      const auto Found = Blocks.find(Id);
      if (!Proof.Remaining-- || Found == Blocks.end() ||
          !Succs[I].insert(Found->second).second)
        return false;
      Preds[Found->second].insert(I);
    }
  bool HasReturn = false;
  for (size_t I = 0; I < Count; ++I) {
    std::set<size_t> Declared;
    for (int Id : Function.Blocks[I].Preds) {
      const auto Found = Blocks.find(Id);
      if (!Proof.Remaining-- || Found == Blocks.end() ||
          !Declared.insert(Found->second).second)
        return false;
    }
    if (Declared != Preds[I] || Function.Blocks[I].Ops.empty())
      return false;
    const bool Returns = Function.Blocks[I].Ops.back().Opcode == NdOp::RETURN;
    const auto LastCall = nativeSourceCallKey(Function.Blocks[I].Ops.back());
    const auto Terminal = LastCall ? Calls.find(*LastCall) : Calls.end();
    const bool StackFailure =
        Terminal != Calls.end() &&
        stackCheckTermination(Terminal->second, Architecture);
    if ((Succs[I].empty() && !Returns && !StackFailure) ||
        (StackFailure && !Succs[I].empty()))
      return false;
    HasReturn |= Returns;
  }
  if (!HasReturn)
    return false;
  std::vector<std::optional<State>> Incoming(Count);
  Incoming[*Entry] = Proof.Initial;
  std::deque<size_t> Pending{*Entry};
  std::vector<bool> Queued(Count);
  Queued[*Entry] = true;
  while (!Pending.empty()) {
    const size_t I = Pending.front();
    Pending.pop_front();
    Queued[I] = false;
    State Out = *Incoming[I];
    if (!Proof.transfer(Function.Blocks[I], Out, false))
      return false;
    for (size_t Successor : Succs[I]) {
      State Next = Out;
      if (Incoming[Successor]) {
        Next = *Incoming[Successor];
        if (!meet(Next.Registers, Out.Registers, Proof.Remaining) ||
            !meet(Next.Stack, Out.Stack, Proof.Remaining))
          return false;
      }
      if (!Incoming[Successor] || Next != *Incoming[Successor]) {
        Incoming[Successor] = std::move(Next);
        if (!Queued[Successor]) {
          Pending.push_back(Successor);
          Queued[Successor] = true;
        }
      }
    }
  }
  std::set<uint64_t> Used;
  bool HasReachableReturn = false;
  for (size_t I = 0; I < Count; ++I) {
    if (!Incoming[I] ||
        !Proof.transfer(Function.Blocks[I], *Incoming[I], true, &Used))
      return false;
    HasReachableReturn |= Function.Blocks[I].Ops.back().Opcode == NdOp::RETURN;
  }
  if (!HasReachableReturn)
    return false;
  if (UsedEntryRegisters)
    *UsedEntryRegisters = std::move(Used);
  return true;
}

bool observesTerminalNativeSourceState(
    const LowFunc &Function, Arch Architecture, const NativeSourceCalls &Calls,
    std::set<uint64_t> &UsedEntryRegisters) {
  // This narrow mode proves only incoming context uses in a straight-line
  // helper with at most two independently declared runtime calls. Exactly
  // the last call terminates; a returning prefix uses the same clobber and
  // frame-escape transfer as ordinary preservation proofs.
  if (Architecture != Arch::AArch64 || Function.Blocks.size() != 1 ||
      Calls.empty() || Calls.size() > 2)
    return false;
  const auto &Block = Function.Blocks.front();
  if (Block.Id < 0 ||
      (Block.StartAddr != Function.Entry &&
       (Block.StartAddr || Block.Id != 0)) ||
      !Block.Preds.empty() || !Block.Succs.empty() ||
      !Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty() ||
      Block.Ops.empty() || Block.Ops.size() > 262144)
    return false;
  unsigned TerminatingCalls = 0;
  for (const auto &[Site, Contract] : Calls) {
    const auto *Signature = Contract.Signature;
    std::string Error;
    if ((Contract.terminates() &&
         Contract.Termination !=
             NativeSourceCallContract::TerminationKind::RuntimeEntry) ||
        !Signature || Signature->Architecture != Architecture ||
        !Signature->ReturnType ||
        Signature->ReturnLocation.Kind ==
            SourceABICarrierKind::IndirectResultPointer ||
        !Signature->ReturnComponents.empty() ||
        (Contract.terminates() &&
         Signature->ReturnType->Kind != NdTypeKind::Void) ||
        !validateSourceABI(*Signature, Error))
      return false;
    for (const auto &Parameter : Signature->Parameters)
      if (!Parameter.Components.empty() || !Parameter.Type ||
          (Parameter.Type->Kind != NdTypeKind::Int &&
           Parameter.Type->Kind != NdTypeKind::Ptr))
        return false;
    TerminatingCalls += Contract.terminates();
  }
  if (TerminatingCalls != 1)
    return false;
  unsigned NativeCalls = 0;
  bool SawTerminatingCall = false;
  for (const auto &Op : Block.Ops) {
    if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
        Op.Opcode == NdOp::INDIR_BR || Op.Opcode == NdOp::RETURN)
      return false;
    if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
      continue;
    const auto Key = nativeSourceCallKey(Op);
    if (!Key || !Calls.count(*Key) || SawTerminatingCall)
      return false;
    ++NativeCalls;
    SawTerminatingCall = Calls.at(*Key).terminates();
  }
  PreservationProof Proof(Architecture, Calls, true);
  auto State = Proof.Initial;
  std::set<uint64_t> Used;
  if (NativeCalls != Calls.size() || !SawTerminatingCall ||
      !Proof.transfer(Block, State, false, &Used) ||
      !Proof.DidTerminate)
    return false;
  UsedEntryRegisters = std::move(Used);
  return true;
}

bool preservesNativeSourceLeafState(const LowFunc &Function, Arch Architecture,
                                    const NativeSourceCalls &Calls) {
  const size_t Count = Function.Blocks.size();
  if (!Count || Count > 16384 || Calls.empty() ||
      (Architecture != Arch::AArch64 && Architecture != Arch::X64))
    return false;
  const auto &TRI = getTargetRegInfo(Architecture);
  for (const auto &[Site, Contract] : Calls) {
    const auto *Signature = Contract.Signature;
    std::string Error;
    if (Contract.terminates() || !Signature ||
        Signature->Architecture != Architecture ||
        Signature->ReturnLocation.Kind ==
            SourceABICarrierKind::IndirectResultPointer ||
        !validateSourceABI(*Signature, Error))
      return false;
  }

  size_t Remaining = 262144;
  std::set<uint64_t> Protected;
  for (const auto &Range : TRI.callPreservedRanges(BinaryFormat::MachO))
    for (unsigned I = 0; I < Range.Bytes; ++I)
      Protected.insert(Range.Offset + I);
  for (unsigned I = 0; I < 8; ++I) {
    Protected.insert(TRI.StackPointer + I);
    Protected.insert(TRI.FramePointer + I);
    if (TRI.LinkRegister)
      Protected.insert(TRI.LinkRegister + I);
  }

  std::map<int, size_t> Blocks;
  std::optional<size_t> Entry;
  bool HasAddresses = false;
  std::vector<std::set<size_t>> Preds(Count), Succs(Count);
  std::set<NativeSourceCallKey> NativeCalls;
  for (size_t I = 0; I < Count; ++I) {
    const auto &Block = Function.Blocks[I];
    if (Block.Id < 0 || !Blocks.emplace(Block.Id, I).second ||
        !Block.ExceptionalSuccs.empty() || !Block.ExceptionalPreds.empty())
      return false;
    HasAddresses |= Block.StartAddr != 0;
    if (Block.StartAddr == Function.Entry) {
      if (Entry)
        return false;
      Entry = I;
    }
  }
  if (!Entry && !HasAddresses && Blocks.count(0))
    Entry = Blocks.at(0);
  if (!Entry)
    return false;
  for (size_t I = 0; I < Count; ++I) {
    const auto &Block = Function.Blocks[I];
    for (int Id : Block.Succs) {
      const auto Found = Blocks.find(Id);
      if (!Remaining-- || Found == Blocks.end() ||
          !Succs[I].insert(Found->second).second)
        return false;
      Preds[Found->second].insert(I);
    }
  }
  for (size_t I = 0; I < Count; ++I) {
    const auto &Block = Function.Blocks[I];
    std::set<size_t> Declared;
    for (int Id : Block.Preds) {
      const auto Found = Blocks.find(Id);
      if (!Remaining-- || Found == Blocks.end() ||
          !Declared.insert(Found->second).second)
        return false;
    }
    if (Declared != Preds[I] || Block.Ops.empty() ||
        (Succs[I].empty() && Block.Ops.back().Opcode != NdOp::RETURN))
      return false;
    for (size_t Index = 0; Index < Block.Ops.size(); ++Index) {
      const auto &Op = Block.Ops[Index];
      if (Op.Output.isReg())
        for (unsigned Byte = 0; Byte < Op.Output.Size; ++Byte)
          if (Protected.count(Op.Output.Offset + Byte))
            return false;
      if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
        continue;
      const auto Key = nativeSourceCallKey(Op);
      if (!Key || Index + 1 >= Block.Ops.size() ||
          Block.Ops[Index + 1].Opcode != NdOp::RETURN ||
          Block.Ops[Index + 1].Addr != Op.Addr)
        return false;
      if (!Calls.count(*Key) || !NativeCalls.insert(*Key).second)
        return false;
    }
  }

  using Taint = std::set<uint64_t>;
  Taint Initial;
  for (unsigned I = 0; I < 8; ++I)
    Initial.insert(TRI.StackPointer + I);
  std::vector<std::optional<Taint>> Incoming(Count);
  Incoming[*Entry] = Initial;
  std::deque<size_t> Pending{*Entry};
  std::vector<bool> Queued(Count);
  Queued[*Entry] = true;
  auto Transfer = [&](const LowBlock &Block, Taint &Registers) {
    Taint Temps;
    va_t Instruction = InvalidVA;
    auto Tainted = [&](const NdVar &Value, unsigned Byte) {
      const auto &Facts = Value.isTemp() ? Temps : Registers;
      return (Value.isReg() || Value.isTemp()) &&
             Facts.count(Value.Offset + Byte);
    };
    for (size_t Index = 0; Index < Block.Ops.size(); ++Index) {
      const auto &Op = Block.Ops[Index];
      if (Op.Addr != Instruction) {
        Temps.clear();
        Instruction = Op.Addr;
      }
      if (!Remaining-- || Op.NumInputs > 6 || Op.Output.Size > 64 ||
          Op.Output.Offset > UINT64_MAX - Op.Output.Size)
        return false;
      bool AnyTaint = false;
      for (unsigned I = 0; I < Op.NumInputs; ++I) {
        const auto &Input = Op.Inputs[I];
        if (Input.Size > 64 || ((Input.isReg() || Input.isTemp()) &&
                                Input.Offset > UINT64_MAX - Input.Size))
          return false;
        if (Remaining < Input.Size)
          return false;
        Remaining -= Input.Size;
        for (unsigned J = 0; J < Input.Size; ++J)
          AnyTaint |= Tainted(Input, J);
      }
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        const auto Key = nativeSourceCallKey(Op);
        if (!Key)
          return false;
        const auto Found = Calls.find(*Key);
        if (Found == Calls.end())
          return false;
        for (unsigned I = 0; I < Op.Inputs[0].Size; ++I)
          if (Tainted(Op.Inputs[0], I))
            return false;
        if (!Found->second.Signature)
          return false;
        for (const auto &Parameter :
             sourceABIParameters(*Found->second.Signature)) {
          const auto &Location = Parameter.Location;
          if ((Location.Kind != SourceABICarrierKind::IntegerRegister &&
               Location.Kind != SourceABICarrierKind::FloatingRegister) ||
              Location.ValueBytes > 64)
            return false;
          for (unsigned I = 0; I < Location.ValueBytes; ++I)
            if (Registers.count(Location.RegisterOffset + I))
              return false;
        }
        continue;
      }
      if (Op.Opcode == NdOp::INDIR_BR)
        return false;

      std::vector<bool> OutputTaint(Op.Output.Size, false);
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
          Op.Inputs[0].Size == Op.Output.Size) {
        for (unsigned I = 0; I < OutputTaint.size(); ++I)
          OutputTaint[I] = Tainted(Op.Inputs[0], I);
      } else if (Op.Opcode == NdOp::LOAD) {
        const auto Memory = lowMemoryOperands(Op);
        if (!Memory.Complete || !Memory.Address)
          return false;
        // A load returns the slot contents, not its address. A frame-derived
        // address cannot have been stored because that case rejects below.
      } else if (Op.Opcode == NdOp::STORE) {
        const auto Memory = lowMemoryOperands(Op);
        if (!Memory.Complete || !Memory.StoredValue)
          return false;
        for (unsigned I = 0; I < Memory.StoredValue->Size; ++I)
          if (Tainted(*Memory.StoredValue, I))
            return false;
      } else if (AnyTaint) {
        // Unknown operations could consume a frame address through an
        // implicit side effect. Arithmetic and other pure producers remain
        // tainted, but only operations with an explicit output are admissible.
        if (!Op.Output.isReg() && !Op.Output.isTemp())
          return false;
        std::fill(OutputTaint.begin(), OutputTaint.end(), true);
      }

      if (Op.Output.isReg() || Op.Output.isTemp()) {
        auto &Output = Op.Output.isReg() ? Registers : Temps;
        if (Op.Output.isReg() &&
            TRI.writeZeroExtends(Op.Output.Offset, Op.Output.Size)) {
          const auto [Offset, Bytes] =
              TRI.findWideReg(Op.Output.Offset, Op.Output.Size);
          for (unsigned I = 0; I < Bytes; ++I)
            Output.erase(Offset + I);
        }
        for (unsigned I = 0; I < OutputTaint.size(); ++I) {
          if (OutputTaint[I])
            Output.insert(Op.Output.Offset + I);
          else
            Output.erase(Op.Output.Offset + I);
        }
      } else if (Op.Output.Size) {
        return false;
      }
    }
    return true;
  };

  while (!Pending.empty()) {
    const size_t I = Pending.front();
    Pending.pop_front();
    Queued[I] = false;
    auto Out = *Incoming[I];
    if (!Transfer(Function.Blocks[I], Out))
      return false;
    for (size_t Successor : Succs[I]) {
      auto Next = Incoming[Successor].value_or(Taint{});
      Next.insert(Out.begin(), Out.end());
      if (!Incoming[Successor] || Next != *Incoming[Successor]) {
        Incoming[Successor] = std::move(Next);
        if (!Queued[Successor]) {
          Pending.push_back(Successor);
          Queued[Successor] = true;
        }
      }
    }
  }
  if (NativeCalls.size() != Calls.size())
    return false;
  for (size_t I = 0; I < Count; ++I)
    if (!Incoming[I])
      return false;
  return true;
}
} // namespace neverd

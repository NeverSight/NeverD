#include "neverd/ir/low/SourceFrameAnalysis.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedNoReturn.h"

#include <algorithm>
#include <array>
#include <deque>
#include <limits>
#include <optional>
#include <set>

namespace neverd {
std::optional<NativeSourceCallKey> nativeSourceCallKey(const LowOp &Operation) {
  return sourceCallOccurrenceKey(Operation);
}

bool hasNativeScalarIntrinsicEvidence(const LowOp &Operation,
                                      Arch Architecture) {
  if (Architecture != Arch::AArch64 || Operation.Opcode != NdOp::INTRINSIC ||
      Operation.NumInputs != 2 || !Operation.Inputs[0].isConst() ||
      Operation.Inputs[0].Size != 2 ||
      Operation.Inputs[0].Offset !=
          static_cast<uint64_t>(Intrinsic::A64_Rbit) ||
      (!Operation.Output.isReg() && !Operation.Output.isTemp()) ||
      Operation.Output.Size != Operation.Inputs[1].Size ||
      (Operation.Output.Size != 1 && Operation.Output.Size != 2 &&
       Operation.Output.Size != 4 && Operation.Output.Size != 8) ||
      (!Operation.Inputs[1].isConst() && !Operation.Inputs[1].isReg() &&
       !Operation.Inputs[1].isTemp()) ||
      Operation.MemoryOrdering != NdMemoryOrdering::None ||
      Operation.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  return true;
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
      !Contract.empty())
    return false;
  SourceFunctionTypeHint Expected;
  Expected.Origin = SourceFunctionTypeHint::OriginKind::DarwinRuntime;
  Expected.ReturnType = NdType::makeVoid();
  std::string Error;
  return assignDarwinScalarSourceABI(Expected, Architecture, Error) &&
         equalSourceABIs(*Contract.Signature, Expected);
}

bool swiftDictionaryViolationTermination(
    const NativeSourceCallContract &Contract, Arch Architecture) {
  if (Contract.Termination !=
          NativeSourceCallContract::TerminationKind::SwiftDictionaryViolation ||
      Architecture != Arch::AArch64 || !Contract.Signature ||
      Contract.Signature->Origin !=
          SourceFunctionTypeHint::OriginKind::SwiftSDK ||
      !Contract.empty())
    return false;
  SourceFunctionTypeHint Expected;
  Expected.Origin = SourceFunctionTypeHint::OriginKind::SwiftSDK;
  Expected.ReturnType = NdType::makeVoid();
  Expected.Parameters = {{"arg0", NdType::makePtr(NdType::makeVoid())}};
  std::string Error;
  return assignDarwinSwiftSourceABI(Expected, Architecture, Error) &&
         equalSourceABIs(*Contract.Signature, Expected);
}

bool ordinaryFrameTermination(const NativeSourceCallContract &Contract,
                              Arch Architecture) {
  return stackCheckTermination(Contract, Architecture) ||
         swiftDictionaryViolationTermination(Contract, Architecture);
}

// Byte identities make partial writes and overlapping spills explicit. A
// frame address needs all eight ordered bytes before it can name a stack slot.
struct ByteFact {
  enum Kind {
    Unknown,
    Entry,
    Frame,
    FrameOrExternal,
    Constant,
    Definition
  } TheKind = Unknown;
  int64_t Value = 0;
  unsigned Index = 0;
  bool MayBeFrame = false;
  size_t FrameBytes = 0;
  bool operator==(const ByteFact &) const = default;
};
using RegisterFacts = std::map<uint64_t, ByteFact>;
using StackFacts = std::map<int64_t, ByteFact>;
struct State {
  RegisterFacts Registers;
  StackFacts Stack;
  // A written unknown value has no identity in Stack, but still initializes
  // its bytes. This must-set is used only by the strict source-frame proof.
  std::set<int64_t> InitializedStack;
  struct ScratchIdentity {
    SourceFrameScratchEffect::Domain Domain;
    size_t Bytes;
    bool operator==(const ScratchIdentity &) const = default;
  };
  std::map<int64_t, ScratchIdentity> Scratch;
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

// Only total scalar bit operations may transport frame taint internally.
// Division and floating-point operations can expose traps or floating status
// even if their result is dead, so they cannot borrow a relocated frame value.
bool privateFrameScalarTransfer(NdOp Opcode) {
  switch (Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
  case NdOp::INT_LEFT:
  case NdOp::INT_RIGHT:
  case NdOp::INT_ASHR:
  case NdOp::INT_MULT:
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_SLESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
  case NdOp::INT_NEGATE:
  case NdOp::INT_NOT:
  case NdOp::INT_CARRY:
  case NdOp::INT_SOVF:
  case NdOp::INT_SBOR:
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
  case NdOp::BOOL_NOT:
  case NdOp::CONCAT:
  case NdOp::SUBBYTES:
  case NdOp::INT_NEG2:
  case NdOp::POPCOUNT:
  case NdOp::SELECT:
  case NdOp::LZCOUNT:
  case NdOp::INSERT:
  case NdOp::EXTRACT:
    return true;
  default:
    return false;
  }
}

class PreservationProof {
public:
  PreservationProof(Arch Architecture, const NativeSourceCalls &Calls,
                    bool TerminalOnly = false,
                    std::set<int64_t> IncomingStackSlots = {},
                    bool RequirePrivateFrame = false,
                    bool ExternalMemoryDisjoint = false,
                    const SourceFunctionTypeHint *EntrySignature = nullptr)
      : Architecture(Architecture), Calls(Calls),
        TRI(getTargetRegInfo(Architecture)), TerminalOnly(TerminalOnly),
        TrackStackArguments(TerminalOnly),
        IncomingStackSlots(std::move(IncomingStackSlots)),
        RequirePrivateFrame(RequirePrivateFrame),
        ExternalMemoryDisjoint(ExternalMemoryDisjoint),
        EntrySignature(EntrySignature) {
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
  int64_t RequiredFrameSize = 0;
  bool TrackDefinitions = false;
  std::optional<SourceFrameLoadDefinition> LoadedDefinition;

  bool transfer(const LowBlock &Block, State &Current, bool CheckExits,
                std::set<uint64_t> *UsedEntryRegisters = nullptr,
                std::optional<size_t> QueryIndex = std::nullopt) {
    if (QueryIndex && *QueryIndex >= Block.Ops.size())
      return false;
    const size_t End = QueryIndex ? *QueryIndex + 1 : Block.Ops.size();
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
      if (Value.isConst() && Value.Size <= 8 && Byte < Value.Size &&
          !isAddressProvenance(Value.Provenance))
        return ByteFact{
            ByteFact::Constant,
            static_cast<int64_t>((Value.Offset >> (Byte * 8)) & 255)};
      return ByteFact{};
    };
    auto InvalidateScratch = [&](int64_t Address, size_t Bytes) {
      if (Remaining < Current.Scratch.size())
        return false;
      Remaining -= Current.Scratch.size();
      std::erase_if(Current.Scratch, [&](const auto &Item) {
        return Address < Item.first + static_cast<int64_t>(Item.second.Bytes) &&
               Item.first < Address + static_cast<int64_t>(Bytes);
      });
      return true;
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
    auto BorrowedFrame = [&](const NdVar &Value,
                             size_t Bytes) -> std::optional<int64_t> {
      if (const auto Exact = FrameOffset(Value))
        return Exact;
      if (Value.Size != 8)
        return std::nullopt;
      const auto First = Read(Value, 0);
      if (First.TheKind != ByteFact::FrameOrExternal || !First.FrameBytes ||
          Bytes > First.FrameBytes)
        return std::nullopt;
      for (unsigned I = 0; I < 8; ++I)
        if (Read(Value, I) != ByteFact{ByteFact::FrameOrExternal, First.Value,
                                       I, true, First.FrameBytes})
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
    for (size_t Index = 0; Index < End; ++Index) {
      const auto &Op = Block.Ops[Index];
      if (Op.Addr != Instruction) {
        Temps.clear();
        Instruction = Op.Addr;
      }
      if (isArchitecturalNoReturn(Op, Architecture)) {
        if (QueryIndex)
          return false;
        const unsigned ExpectedInputs = Architecture == Arch::AArch64 ? 1 : 2;
        if (Op.NumInputs != ExpectedInputs ||
            Op.MemoryOrdering != NdMemoryOrdering::None ||
            Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
            Index + 1 != Block.Ops.size() || !Block.Succs.empty())
          return false;
        DidTerminate = true;
        return true;
      }
      if (Op.NumInputs > 6 || Op.Output.Size > 64 ||
          (Op.Opcode == NdOp::INTRINSIC &&
           !hasNativeScalarIntrinsicEvidence(Op, Architecture)) ||
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
      // Scalar frame arithmetic may build private addresses, but its result
      // must never influence a source-visible control or value effect.
      if (RequirePrivateFrame && MayBeFrame && Op.Opcode != NdOp::LOAD &&
          Op.Opcode != NdOp::STORE && !privateFrameScalarTransfer(Op.Opcode))
        return false;
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
        for (unsigned InputIndex = 0; InputIndex < Op.NumInputs; ++InputIndex) {
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
            Complete &=
                Read(Input, I) == ByteFact{ByteFact::Entry, First.Value + I};
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
        if (const auto *Copy = Found->second.RegisterCopy) {
          auto SnapshotValue = [&](const SourceRegisterValue &Source) {
            std::array<ByteFact, 8> Value{};
            if (const auto *Entry = std::get_if<SourceEntryRegister>(&Source))
              for (unsigned I = 0; I < 8; ++I)
                Value[I] = Read(NdVar::reg(Entry->Offset, 8), I);
            return Value;
          };
          std::vector<std::pair<uint64_t, std::array<ByteFact, 8>>> Snapshots;
          for (const auto &[Destination, Source] : Copy->Registers)
            Snapshots.emplace_back(Destination, SnapshotValue(Source));
          if (Copy->StackStore) {
            const auto SP = FrameOffset(NdVar::reg(TRI.StackPointer, 8));
            const auto Value = SnapshotValue(Copy->StackStore->Value);
            if (!SP || !sourceStackStoreFitsFrame(*SP) ||
                std::any_of(Value.begin(), Value.end(),
                            [](const auto &Byte) { return Byte.MayBeFrame; }))
              return false;
            // A physical write replaces every old spill byte. Unknown source
            // bytes remain unknown; WrittenStack does not define their value.
            for (unsigned I = 0; I < 8; ++I) {
              put(Current.Stack, *SP + I, Value[I]);
              if (TrackStackArguments)
                WrittenStack.insert(*SP + I);
            }
            if (!InvalidateScratch(*SP, 8))
              return false;
          }
          for (const auto &[Destination, Value] : Snapshots)
            for (unsigned I = 0; I < 8; ++I)
              put(Current.Registers, Destination + I, Value[I]);
          continue;
        }
        if (!Found->second.Signature)
          return false;
        if (Found->second.terminates() && !TerminalOnly &&
            !ordinaryFrameTermination(Found->second, Architecture))
          return false;
        if (Found->second.Termination ==
                NativeSourceCallContract::TerminationKind::StackCheckFailure &&
            (TerminalOnly || Index + 1 != Block.Ops.size() ||
             !Block.Succs.empty()))
          return false;
        if (Found->second.Termination ==
                NativeSourceCallContract::TerminationKind::
                    SwiftDictionaryViolation &&
            (TerminalOnly || !Block.Succs.empty() ||
             (Index + 1 != Block.Ops.size() &&
              (Index + 2 != Block.Ops.size() ||
               !isArchitecturalNoReturn(Block.Ops.back(), Architecture)))))
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
        std::optional<int64_t> ReturnedFrame;
        std::optional<int64_t> ScratchAddress;
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
            if (UsedEntryRegisters && ReturningArgument &&
                Location.ValueBytes == 8) {
              const auto First = lookup(Current.Stack, Start);
              if (First.TheKind == ByteFact::Entry && First.Value >= 0 &&
                  First.Value <= 28 * 8 && First.Value % 8 == 0 &&
                  First.Value != 18 * 8) {
                bool Complete = true;
                for (unsigned I = 0; I < 8; ++I)
                  Complete &= lookup(Current.Stack, Start + I) ==
                              ByteFact{ByteFact::Entry, First.Value + I};
                if (Complete)
                  UsedEntryRegisters->insert(
                      static_cast<uint64_t>(First.Value));
              }
            }
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
            if ((ReadOnly == Found->second.ReadOnlyFrameParameters.end()) ==
                    (Writable == Found->second.WritableFrameParameters.end()) ||
                Location.Kind != SourceABICarrierKind::IntegerRegister ||
                Location.ValueBytes != 8)
              return false;
            const size_t BorrowedBytes =
                ReadOnly != Found->second.ReadOnlyFrameParameters.end()
                    ? ReadOnly->second
                    : Writable->second;
            if (!BorrowedBytes || BorrowedBytes > MaxFrame)
              return false;
            const auto Address = BorrowedFrame(
                NdVar::reg(Location.RegisterOffset, Location.ValueBytes),
                BorrowedBytes);
            if (!Address || *Address < *SP ||
                *Address > -static_cast<int64_t>(BorrowedBytes))
              return false;
            const auto &Scratch = Found->second.Scratch;
            if (Scratch && Scratch->Parameter == ParameterIndex) {
              // Possible frame-or-external provenance cannot identify an
              // opaque record. Require the exact complete frame address.
              if (!FrameOffset(NdVar::reg(Location.RegisterOffset, 8)))
                return false;
              if (const auto &Condition = Scratch->Condition) {
                const auto &Carrier =
                    Signature.Parameters[Condition->Parameter].Location;
                uint64_t Number = 0;
                for (unsigned I = 0; I < 8; ++I) {
                  const auto Byte =
                      lookup(Current.Registers, Carrier.RegisterOffset + I);
                  if (Byte.TheKind != ByteFact::Constant)
                    return false;
                  Number |= static_cast<uint64_t>(Byte.Value) << (I * 8);
                }
                if (!Condition->Values.count(Number))
                  return false;
              }
              if (Scratch->TheAction ==
                  SourceFrameScratchEffect::Action::Finish) {
                const auto Record = Current.Scratch.find(*Address);
                if (Record == Current.Scratch.end() ||
                    Record->second != State::ScratchIdentity{Scratch->TheDomain,
                                                             Scratch->Bytes})
                  return false;
              }
              ScratchAddress = *Address;
            }
            if (Writable != Found->second.WritableFrameParameters.end())
              WritableFrameRanges.emplace_back(*Address, BorrowedBytes);
            if (Found->second.ReturnFrameOrExternal &&
                Found->second.ReturnFrameOrExternal->Parameter ==
                    ParameterIndex)
              ReturnedFrame = *Address;
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
                UsedEntryRegisters->insert(static_cast<uint64_t>(First.Value));
            }
          }
        }
        for (const auto &[Address, Bytes] : WritableFrameRanges) {
          if (!InvalidateScratch(Address, Bytes))
            return false;
          std::erase_if(Current.Stack, [&](const auto &Item) {
            return Item.first >= Address &&
                   Item.first - Address < static_cast<int64_t>(Bytes);
          });
          std::erase_if(WrittenStack, [&](int64_t Byte) {
            return Byte >= Address &&
                   Byte - Address < static_cast<int64_t>(Bytes);
          });
        }
        if (ScratchAddress) {
          const auto &Scratch = *Found->second.Scratch;
          if (Scratch.TheAction == SourceFrameScratchEffect::Action::Initialize)
            Current.Scratch[*ScratchAddress] = {Scratch.TheDomain,
                                                Scratch.Bytes};
          else
            Current.Scratch.erase(*ScratchAddress);
        }
        if (Current.Scratch.size() > MaxFacts)
          return false;
        if (Found->second.terminates()) {
          if (QueryIndex)
            return false;
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
          std::erase_if(WrittenStack, [&](int64_t Byte) { return Byte < *SP; });
          std::erase_if(Current.Scratch,
                        [&](const auto &Item) { return Item.first < *SP; });
          Temps.clear();
        }
        if (ReturnedFrame) {
          const auto &Alias = *Found->second.ReturnFrameOrExternal;
          const auto &Location = Signature.ReturnLocation;
          for (unsigned I = 0; I < 8; ++I)
            put(Current.Registers, Location.RegisterOffset + I,
                {ByteFact::FrameOrExternal, *ReturnedFrame, I, true,
                 Alias.Bytes});
        }
        continue;
      }
      if (Op.Opcode == NdOp::RETURN) {
        if (QueryIndex)
          return false;
        if (Index + 1 != Block.Ops.size() || !Block.Succs.empty() ||
            (CheckExits && !IsRestored()))
          return false;
        if (CheckExits && EntrySignature) {
          const auto Escapes = [&](const SourceABIValueLocation &Location) {
            if (Location.Kind != SourceABICarrierKind::IntegerRegister &&
                Location.Kind != SourceABICarrierKind::FloatingRegister)
              return false;
            for (unsigned I = 0; I < Location.ValueBytes; ++I)
              if (lookup(Current.Registers, Location.RegisterOffset + I)
                      .MayBeFrame)
                return true;
            return false;
          };
          if (Escapes(EntrySignature->ReturnLocation) ||
              std::any_of(EntrySignature->ReturnComponents.begin(),
                          EntrySignature->ReturnComponents.end(), Escapes))
            return false;
        }
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
        for (unsigned I = Op.Inputs[0].Size; I < Value.size(); ++I)
          Value[I] = {ByteFact::Constant, 0};
      } else if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs == 2 &&
                 Op.Inputs[1].isConst() &&
                 Op.Inputs[1].Offset <= Op.Inputs[0].Size &&
                 Op.Output.Size <= Op.Inputs[0].Size - Op.Inputs[1].Offset) {
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
          if (RequirePrivateFrame && (Next < -MaxFrame || Next > 0))
            return false;
          if (Next >= -MaxFrame && Next <= MaxFrame) {
            if (RequirePrivateFrame)
              RequiredFrameSize = std::max(RequiredFrameSize, -Next);
            for (unsigned I = 0; I < 8; ++I)
              Value[I] = {ByteFact::Frame, Next, I, true};
          }
        }
      } else if (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) {
        const auto Memory = lowMemoryOperands(Op);
        if (!Memory.Complete || !Memory.Address || Memory.AccessSize > 64)
          return false;
        const auto Address = FrameOffset(*Memory.Address);
        if (RequirePrivateFrame && !Address && !ExternalMemoryDisjoint)
          return false;
        if (RequirePrivateFrame && !Address)
          for (unsigned I = 0; I < Memory.Address->Size; ++I)
            if (Read(*Memory.Address, I).MayBeFrame)
              return false;
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
          if (RequirePrivateFrame)
            RequiredFrameSize = std::max(RequiredFrameSize, -*Address);
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
            if (!InvalidateScratch(*Address, Memory.AccessSize))
              return false;
            for (unsigned I = 0; I < Memory.AccessSize; ++I) {
              put(Current.Stack, *Address + I, Read(*Memory.StoredValue, I));
              if (RequirePrivateFrame)
                Current.InitializedStack.insert(*Address + I);
            }
            if (TrackStackArguments)
              for (unsigned I = 0; I < Memory.AccessSize; ++I)
                WrittenStack.insert(*Address + I);
          } else {
            // A value with any frame-derived byte may be a partial or
            // inexact alias of private storage. A completely non-frame
            // address names storage outside this invocation's private frame,
            // so the write cannot invalidate its exact spill identities.
            for (unsigned I = 0; I < Memory.Address->Size; ++I)
              if (Read(*Memory.Address, I).MayBeFrame)
                return false;
          }
        } else {
          if (RequirePrivateFrame && Address)
            for (unsigned I = 0; I < Memory.AccessSize; ++I)
              if (!Current.InitializedStack.count(*Address + I))
                return false;
          for (unsigned I = 0; I < Value.size(); ++I)
            // Incoming arguments are external values. Even if a slot happens
            // to contain a saved register's bits, it cannot certify restoration
            // of this invocation's preserved state or a private-frame address.
            Value[I] = Address && !IncomingRead
                           ? lookup(Current.Stack, *Address + I)
                           : ByteFact{};
        }
        if (QueryIndex && Index == *QueryIndex) {
          if (!TrackDefinitions || Op.Opcode != NdOp::LOAD || !Address ||
              IncomingRead || Memory.AccessSize != 8 || Value.size() != 8)
            return false;
          const auto First = Value.front();
          if (First.TheKind != ByteFact::Definition || First.Value < 0 ||
              static_cast<size_t>(First.Value) >= Definitions.size())
            return false;
          for (unsigned I = 0; I < 8; ++I)
            if (Value[I] != ByteFact{ByteFact::Definition, First.Value, I})
              return false;
          LoadedDefinition = {Definitions[First.Value], *Address};
        }
      }
      if (TrackDefinitions && Value.size() == 8 &&
          std::all_of(Value.begin(), Value.end(), [](const auto &Byte) {
            return Byte.TheKind == ByteFact::Unknown && !Byte.MayBeFrame;
          })) {
        // This identifies the producer only. In particular, an unknown load
        // remains a load to authenticate, never a recovered pointer constant.
        const auto Key = std::make_pair(Block.Id, Index);
        auto Found = DefinitionIds.find(Key);
        if (Found == DefinitionIds.end()) {
          if (!Remaining || Definitions.size() >= MaxFacts)
            return false;
          --Remaining;
          Found = DefinitionIds.emplace(Key, Definitions.size()).first;
          Definitions.push_back({Block.Id, Index, Op.Addr, Op.Seq, Op.Output});
        }
        for (unsigned I = 0; I < 8; ++I)
          Value[I] = {ByteFact::Definition, static_cast<int64_t>(Found->second),
                      I};
      }
      if (Op.Output.isReg() || Op.Output.isTemp()) {
        auto &Output = Op.Output.isReg() ? Current.Registers : Temps;
        if (Op.Output.isReg() &&
            TRI.writeZeroExtends(Op.Output.Offset, Op.Output.Size)) {
          const auto [Offset, Bytes] =
              TRI.findWideReg(Op.Output.Offset, Op.Output.Size);
          for (unsigned I = 0; I < Bytes; ++I)
            Output.erase(Offset + I);
          for (unsigned I = Op.Output.Size; I < Bytes; ++I)
            put(Output, Offset + I, {ByteFact::Constant, 0});
        }
        for (unsigned I = 0; I < Value.size(); ++I)
          put(Output, Op.Output.Offset + I, Value[I]);
        if (Op.Output.isReg() && Op.Output.Offset < TRI.StackPointer + 8 &&
            TRI.StackPointer < Op.Output.Offset + Op.Output.Size) {
          if (Remaining < Current.Scratch.size())
            return false;
          Remaining -= Current.Scratch.size();
          const auto SP = FrameOffset(NdVar::reg(TRI.StackPointer, 8));
          if (!SP)
            Current.Scratch.clear();
          else
            std::erase_if(Current.Scratch,
                          [&](const auto &Item) { return Item.first < *SP; });
          if (TrackDefinitions) {
            if (Remaining < Current.Stack.size())
              return false;
            Remaining -= Current.Stack.size();
            if (!SP)
              Current.Stack.clear();
            else
              std::erase_if(Current.Stack,
                            [&](const auto &Item) { return Item.first < *SP; });
          }
        }
      } else if (Op.Output.Size) {
        return false;
      }
      if (Current.Registers.size() > MaxFacts ||
          Current.Stack.size() > MaxFacts || Temps.size() > MaxFacts ||
          WrittenStack.size() > MaxFacts ||
          Current.InitializedStack.size() > MaxFacts ||
          Current.Scratch.size() > MaxFacts)
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
  bool RequirePrivateFrame;
  bool ExternalMemoryDisjoint;
  const SourceFunctionTypeHint *EntrySignature;
  std::map<std::pair<int, size_t>, size_t> DefinitionIds;
  std::vector<SourceFrameDefinition> Definitions;
};
} // namespace

static bool validateFrameCalls(const LowFunc &Function, Arch Architecture,
                               const NativeSourceCalls &Calls,
                               bool &HasStackStore) {
  if (Calls.size() > MaxFacts)
    return false;
  HasStackStore = false;
  for (const auto &[Site, Contract] : Calls) {
    if (const auto *Copy = Contract.RegisterCopy) {
      if (Architecture != Arch::AArch64 || Contract.Signature ||
          Contract.terminates() || !Contract.empty() ||
          Copy->Caller != Function.Entry || Copy->Site != Site ||
          (Copy->Registers.empty() && !Copy->isReturnOnly()) ||
          Copy->Registers.size() > 16)
        return false;
      if (Copy->StackStore) {
        const auto &Store = *Copy->StackStore;
        if (Store.InstructionIndex >= 16 ||
            Store.InstructionIndex >= Copy->LeafWords.size() ||
            Copy->LeafWords[Store.InstructionIndex] != Store.Word ||
            (Store.Word & 0xffffffe0) != 0xf90003e0 || (Store.Word & 31) > 28 ||
            (Store.Word & 31) == 18)
          return false;
        if (const auto *Entry =
                std::get_if<SourceEntryRegister>(&Store.Value)) {
          if (Entry->Offset > 28 * 8 || Entry->Offset % 8 ||
              Entry->Offset == 18 * 8)
            return false;
        } else if (!std::get<SourceConstantStringAddress>(Store.Value)
                        .Address) {
          return false;
        }
        HasStackStore = true;
      }
      for (const auto &[Destination, Source] : Copy->Registers) {
        if (Destination > 28 * 8 || Destination % 8 || Destination == 18 * 8)
          return false;
        if (const auto *Entry = std::get_if<SourceEntryRegister>(&Source)) {
          if (Entry->Offset > 28 * 8 || Entry->Offset % 8 ||
              Entry->Offset == 18 * 8)
            return false;
        } else if (!std::get<SourceConstantStringAddress>(Source).Address) {
          return false;
        }
      }
      continue;
    }
    const auto *Signature = Contract.Signature;
    std::string Error;
    // Hidden result storage needs its own bounded frame-write proof before
    // this analysis can treat it as preserving saved machine state.
    if ((Contract.terminates() &&
         !ordinaryFrameTermination(Contract, Architecture)) ||
        !Signature || Signature->Architecture != Architecture ||
        Signature->ReturnLocation.Kind ==
            SourceABICarrierKind::IndirectResultPointer ||
        !validateSourceABI(*Signature, Error) ||
        !sourceFrameEffectsMatchABI(Contract, *Signature))
      return false;
  }
  return true;
}

struct SourceFrameGraph {
  std::map<int, size_t> Blocks;
  size_t Entry;
  std::vector<std::set<size_t>> Preds, Succs;
  bool HasReturn;
};

static std::optional<SourceFrameGraph>
frameGraph(const LowFunc &Function, Arch Architecture,
           const NativeSourceCalls &Calls, size_t &Remaining) {
  const size_t Count = Function.Blocks.size();
  if (!Count || Count > 16384 || Remaining < Count)
    return std::nullopt;
  Remaining -= Count;
  std::map<int, size_t> Blocks;
  std::optional<size_t> Entry;
  bool HasAddresses = false;
  for (size_t I = 0; I < Count; ++I) {
    const auto &Block = Function.Blocks[I];
    if (Block.Id < 0 || !Blocks.emplace(Block.Id, I).second ||
        !Block.ExceptionalSuccs.empty() || !Block.ExceptionalPreds.empty())
      return std::nullopt;
    HasAddresses |= Block.StartAddr != 0;
    if (Block.StartAddr == Function.Entry) {
      if (Entry)
        return std::nullopt;
      Entry = I;
    }
  }
  if (!Entry && !HasAddresses && Blocks.count(0))
    Entry = Blocks.at(0);
  if (!Entry)
    return std::nullopt;
  std::vector<std::set<size_t>> Preds(Count), Succs(Count);
  for (size_t I = 0; I < Count; ++I)
    for (int Id : Function.Blocks[I].Succs) {
      const auto Found = Blocks.find(Id);
      if (!Remaining-- || Found == Blocks.end() ||
          !Succs[I].insert(Found->second).second)
        return std::nullopt;
      Preds[Found->second].insert(I);
    }
  bool HasReturn = false;
  for (size_t I = 0; I < Count; ++I) {
    std::set<size_t> Declared;
    for (int Id : Function.Blocks[I].Preds) {
      const auto Found = Blocks.find(Id);
      if (!Remaining-- || Found == Blocks.end() ||
          !Declared.insert(Found->second).second)
        return std::nullopt;
    }
    if (Declared != Preds[I] || Function.Blocks[I].Ops.empty())
      return std::nullopt;
    const bool Returns = Function.Blocks[I].Ops.back().Opcode == NdOp::RETURN;
    const auto LastCall = nativeSourceCallKey(Function.Blocks[I].Ops.back());
    const auto Terminal = LastCall ? Calls.find(*LastCall) : Calls.end();
    const bool ExceptionalCall =
        Terminal != Calls.end() &&
        ordinaryFrameTermination(Terminal->second, Architecture);
    const bool ArchitecturalTrap =
        isArchitecturalNoReturn(Function.Blocks[I].Ops.back(), Architecture);
    if ((Succs[I].empty() && !Returns && !ExceptionalCall &&
         !ArchitecturalTrap) ||
        ((ExceptionalCall || ArchitecturalTrap) && !Succs[I].empty()))
      return std::nullopt;
    HasReturn |= Returns;
  }
  return SourceFrameGraph{std::move(Blocks), *Entry, std::move(Preds),
                          std::move(Succs), HasReturn};
}

static bool meetFrameState(State &Next, const State &Other, size_t &Remaining,
                           bool RequirePrivateFrame) {
  if (!meet(Next.Registers, Other.Registers, Remaining) ||
      !meet(Next.Stack, Other.Stack, Remaining))
    return false;
  if (Remaining < Next.Scratch.size())
    return false;
  Remaining -= Next.Scratch.size();
  std::erase_if(Next.Scratch, [&](const auto &Item) {
    const auto Found = Other.Scratch.find(Item.first);
    return Found == Other.Scratch.end() || Found->second != Item.second;
  });
  if (RequirePrivateFrame) {
    if (Remaining < Next.InitializedStack.size())
      return false;
    Remaining -= Next.InitializedStack.size();
    std::erase_if(Next.InitializedStack, [&](int64_t Byte) {
      return !Other.InitializedStack.count(Byte);
    });
  }
  return true;
}

static bool restoresNativeSourceStateImpl(
    const LowFunc &Function, Arch Architecture, const NativeSourceCalls &Calls,
    std::set<uint64_t> *UsedEntryRegisters,
    const SourceFunctionTypeHint *EntrySignature, bool RequirePrivateFrame,
    bool ExternalMemoryDisjoint, int64_t *RequiredFrameSize) {
  const size_t Count = Function.Blocks.size();
  if (!Count || Count > 16384 ||
      (Architecture != Arch::AArch64 && Architecture != Arch::X64))
    return false;
  bool HasStackStore = false;
  if (!validateFrameCalls(Function, Architecture, Calls, HasStackStore))
    return false;
  std::set<int64_t> IncomingStackSlots;
  if (EntrySignature) {
    std::string Error;
    if (EntrySignature->Architecture != Architecture ||
        (EntrySignature->Origin !=
             SourceFunctionTypeHint::OriginKind::NativeAnalysis &&
         !HasStackStore) ||
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
                          std::move(IncomingStackSlots), RequirePrivateFrame,
                          ExternalMemoryDisjoint, EntrySignature);
  const auto Graph = frameGraph(Function, Architecture, Calls, Proof.Remaining);
  if (!Graph || !Graph->HasReturn)
    return false;
  const auto Entry = Graph->Entry;
  const auto &Succs = Graph->Succs;
  std::vector<std::optional<State>> Incoming(Count);
  Incoming[Entry] = Proof.Initial;
  std::deque<size_t> Pending{Entry};
  std::vector<bool> Queued(Count);
  Queued[Entry] = true;
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
        if (!meetFrameState(Next, Out, Proof.Remaining, RequirePrivateFrame))
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
  if (RequiredFrameSize)
    *RequiredFrameSize = Proof.RequiredFrameSize;
  return true;
}

std::optional<SourceFrameLoadDefinition>
sourceFrameLoadedDefinition(const LowFunc &Function, Arch Architecture,
                            const NativeSourceCalls &Calls, int BlockId,
                            size_t OperationIndex) {
  if (Architecture != Arch::AArch64)
    return std::nullopt;
  bool HasStackStore = false;
  if (!validateFrameCalls(Function, Architecture, Calls, HasStackStore))
    return std::nullopt;
  PreservationProof Proof(Architecture, Calls);
  Proof.TrackDefinitions = true;
  const auto Graph = frameGraph(Function, Architecture, Calls, Proof.Remaining);
  if (!Graph || !Graph->Blocks.count(BlockId))
    return std::nullopt;
  const size_t Query = Graph->Blocks.at(BlockId);
  const auto &Block = Function.Blocks[Query];
  if (OperationIndex >= Block.Ops.size() ||
      Block.Ops[OperationIndex].Opcode != NdOp::LOAD ||
      Block.Ops[OperationIndex].Output.Size != 8)
    return std::nullopt;

  // Validate the original graph before taking a prefix. In particular, a
  // later edge back into the prefix cannot disappear with the later calls.
  const size_t Count = Function.Blocks.size();
  std::vector<bool> Reachable(Count), Relevant(Count);
  std::deque<size_t> Pending{Graph->Entry};
  Reachable[Graph->Entry] = true;
  while (!Pending.empty()) {
    const size_t I = Pending.front();
    Pending.pop_front();
    for (size_t Next : Graph->Succs[I]) {
      if (!Proof.Remaining--)
        return std::nullopt;
      if (!Reachable[Next]) {
        Reachable[Next] = true;
        Pending.push_back(Next);
      }
    }
  }
  if (std::find(Reachable.begin(), Reachable.end(), false) != Reachable.end())
    return std::nullopt;
  Relevant[Query] = true;
  Pending.push_back(Query);
  while (!Pending.empty()) {
    const size_t I = Pending.front();
    Pending.pop_front();
    for (size_t Previous : Graph->Preds[I]) {
      if (!Proof.Remaining--)
        return std::nullopt;
      if (!Relevant[Previous]) {
        Relevant[Previous] = true;
        Pending.push_back(Previous);
      }
    }
  }
  if (!Relevant[Graph->Entry])
    return std::nullopt;
  std::vector<size_t> Indegree(Count);
  size_t RelevantCount = 0;
  for (size_t I = 0; I < Count; ++I) {
    if (!Relevant[I])
      continue;
    ++RelevantCount;
    // Ancestor closure includes every predecessor, including a cycle that
    // returns after the query. A bounded topological pass rejects such cycles.
    Indegree[I] = Graph->Preds[I].size();
    if (!Indegree[I])
      Pending.push_back(I);
  }
  std::vector<size_t> Order;
  while (!Pending.empty()) {
    const size_t I = Pending.front();
    Pending.pop_front();
    Order.push_back(I);
    for (size_t Next : Graph->Succs[I]) {
      if (!Proof.Remaining--)
        return std::nullopt;
      if (Relevant[Next] && !--Indegree[Next])
        Pending.push_back(Next);
    }
  }
  if (Order.size() != RelevantCount || Order.front() != Graph->Entry ||
      Order.back() != Query)
    return std::nullopt;

  std::vector<std::optional<State>> Outgoing(Count);
  for (size_t I : Order) {
    const auto &CurrentBlock = Function.Blocks[I];
    State Current = Proof.Initial;
    bool HasIncoming = I == Graph->Entry;
    for (size_t Previous : Graph->Preds[I]) {
      if (!Outgoing[Previous])
        return std::nullopt;
      if (!HasIncoming)
        Current = *Outgoing[Previous];
      else if (!meetFrameState(Current, *Outgoing[Previous], Proof.Remaining,
                               false))
        return std::nullopt;
      HasIncoming = true;
    }
    const size_t End =
        I == Query ? OperationIndex + 1 : CurrentBlock.Ops.size();
    if (Proof.Remaining < End)
      return std::nullopt;
    Proof.Remaining -= End;
    for (size_t J = 0; J < End; ++J)
      if (CurrentBlock.Ops[J].Opcode == NdOp::RETURN)
        return std::nullopt;
    if (!HasIncoming ||
        !Proof.transfer(CurrentBlock, Current, false, nullptr,
                        I == Query ? std::optional(OperationIndex)
                                   : std::nullopt) ||
        Proof.DidTerminate)
      return std::nullopt;
    // Charge retained state, not only executed operations. A long chain of
    // tiny blocks must not multiply a large frame map without a bound.
    const size_t StateCost = Current.Registers.size() + Current.Stack.size() +
                             Current.Scratch.size() +
                             Current.InitializedStack.size();
    if (Proof.Remaining < StateCost)
      return std::nullopt;
    Proof.Remaining -= StateCost;
    Outgoing[I] = std::move(Current);
  }
  return Proof.LoadedDefinition;
}

bool restoresNativeSourceState(const LowFunc &Function, Arch Architecture,
                               const NativeSourceCalls &Calls,
                               std::set<uint64_t> *UsedEntryRegisters,
                               const SourceFunctionTypeHint *EntrySignature) {
  return restoresNativeSourceStateImpl(Function, Architecture, Calls,
                                       UsedEntryRegisters, EntrySignature,
                                       false, false, nullptr);
}

bool certifiesPrivateSourceFrame(const LowFunc &Function, Arch Architecture,
                                 bool ExternalMemoryDisjoint,
                                 int64_t *RequiredFrameSize) {
  if (Architecture != Arch::X64)
    return false;
  return restoresNativeSourceStateImpl(Function, Architecture, {}, nullptr,
                                       nullptr, true, ExternalMemoryDisjoint,
                                       RequiredFrameSize);
}

bool observesTerminalNativeSourceState(const LowFunc &Function,
                                       Arch Architecture,
                                       const NativeSourceCalls &Calls,
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
    if (Contract.RegisterCopy || !Contract.empty())
      return false;
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
      !Proof.transfer(Block, State, false, &Used) || !Proof.DidTerminate)
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
    if (Contract.RegisterCopy)
      return false;
    const auto *Signature = Contract.Signature;
    std::string Error;
    if (Contract.terminates() || !Contract.empty() || !Signature ||
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

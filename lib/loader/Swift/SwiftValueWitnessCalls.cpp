#include "neverd/loader/Swift/SwiftValueWitnessCalls.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <array>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace neverd {
namespace {
struct Step {
  enum class Kind { Add, Load } TheKind = Kind::Add;
  int64_t Offset = 0;
  uint16_t Bytes = 0;
  bool operator==(const Step &) const = default;
};

struct Root {
  enum class Kind { Entry, Definition, CallResult, Constant } TheKind =
      Kind::Entry;
  size_t Block = 0;
  size_t Operation = 0;
  VnodeSpace Space = VnodeSpace::REG;
  uint64_t Offset = 0;
  uint16_t Bytes = 0;
  ConstantAddressProvenance Provenance = ConstantAddressProvenance::Unknown;
  uint64_t AddressOwnerVA = InvalidVA;
  bool operator==(const Root &) const = default;
};

struct Path {
  Root Base;
  std::vector<Step> Steps;
  bool operator==(const Path &) const = default;

  bool add(int64_t Offset) {
    if (!Offset)
      return true;
    if (!Steps.empty() && Steps.back().TheKind == Step::Kind::Add) {
      const int64_t Previous = Steps.back().Offset;
      if ((Offset > 0 && Previous > INT64_MAX - Offset) ||
          (Offset < 0 && Previous < INT64_MIN - Offset))
        return false;
      Steps.back().Offset += Offset;
      if (!Steps.back().Offset)
        Steps.pop_back();
    } else {
      Steps.push_back({Step::Kind::Add, Offset, 0});
    }
    return Steps.size() <= 32;
  }

  bool load(uint16_t Bytes) {
    if (Bytes != 8 || Steps.size() == 32)
      return false;
    Steps.push_back({Step::Kind::Load, 0, Bytes});
    return true;
  }
};

std::optional<va_t> literalMetadataAddress(const Path &Metadata) {
  if (Metadata.Base.TheKind != Root::Kind::Constant)
    return std::nullopt;
  va_t Address = Metadata.Base.Offset;
  for (const auto &Part : Metadata.Steps) {
    if (Part.TheKind != Step::Kind::Add)
      return std::nullopt;
    if (Part.Offset < 0) {
      const uint64_t Magnitude = uint64_t{0} - uint64_t(Part.Offset);
      if (Address < Magnitude)
        return std::nullopt;
      Address -= Magnitude;
    } else {
      if (Address > UINT64_MAX - uint64_t(Part.Offset))
        return std::nullopt;
      Address += uint64_t(Part.Offset);
    }
  }
  return Address;
}

using Variable = std::tuple<VnodeSpace, uint64_t, uint16_t>;
Variable variable(const NdVar &Value) {
  return {Value.Space, Value.Offset, Value.Size};
}

bool overlaps(const NdVar &Left, const NdVar &Right) {
  if (Left.Space != Right.Space || !Left.Size || !Right.Size)
    return false;
  return Left.Offset <= Right.Offset ? Right.Offset - Left.Offset < Left.Size
                                     : Left.Offset - Right.Offset < Right.Size;
}

class Tracer {
  const BinaryImage &Image;
  const LowFunc &Function;
  const TargetRegInfo &TRI;
  std::map<int, size_t> Blocks;
  bool Valid = true;
  size_t Budget = 0;
  size_t RemainingBudget = 1U << 20;
  bool Exhausted = false;
  std::set<std::tuple<size_t, size_t, Variable>> Active;

  std::optional<Path> trace(size_t BlockIndex, size_t Before,
                            const NdVar &Value, va_t TemporaryAddress) {
    if (!Budget || !RemainingBudget) {
      Exhausted |= !RemainingBudget;
      return std::nullopt;
    }
    if (Value.Size != 8)
      return std::nullopt;
    if (Value.isConst()) {
      // A numeric immediate cannot stand in for a Swift metadata pointer.
      // Keep only loader-authenticated image addresses; the final metadata
      // slot is checked after the complete ADRP/add path is reconstructed.
      if ((Value.Provenance != ConstantAddressProvenance::Address &&
           Value.Provenance != ConstantAddressProvenance::DataAddress &&
           Value.Provenance != ConstantAddressProvenance::AddressFragment) ||
          !Image.isDataAddress(Value.Offset))
        return std::nullopt;
      return Path{Root{Root::Kind::Constant, 0, 0, VnodeSpace::CONST,
                       Value.Offset, 8, Value.Provenance,
                       Value.AddressOwnerVA},
                  {}};
    }
    if (Value.isRam() ||
        (Value.Space != VnodeSpace::REG && Value.Space != VnodeSpace::TEMP))
      return std::nullopt;
    --Budget;
    --RemainingBudget;
    const auto Guard = std::tuple{BlockIndex, Before, variable(Value)};
    if (!Active.insert(Guard).second)
      return std::nullopt;
    struct Pop {
      std::set<std::tuple<size_t, size_t, Variable>> &Values;
      decltype(Guard) Key;
      ~Pop() { Values.erase(Key); }
    } PopGuard{Active, Guard};

    const auto &Block = Function.Blocks[BlockIndex];
    for (size_t Index = Before; Index-- > 0;) {
      const auto &Operation = Block.Ops[Index];
      if (Value.isTemp() && Operation.Addr != TemporaryAddress)
        return std::nullopt;
      if ((Operation.Opcode == NdOp::CALL ||
           Operation.Opcode == NdOp::INDIR_CALL) &&
          Value.isReg() && !TRI.isCallPreserved(Value.Offset, Value.Size))
        return Path{Root{Root::Kind::CallResult, BlockIndex, Index, Value.Space,
                         Value.Offset, Value.Size},
                    {}};
      if (!Operation.Output.Size || !overlaps(Operation.Output, Value))
        continue;
      if (Operation.Output.Space != Value.Space ||
          Operation.Output.Offset != Value.Offset ||
          Operation.Output.Size != Value.Size)
        return std::nullopt;
      auto Input = [&](unsigned I) -> std::optional<Path> {
        if (I >= Operation.NumInputs)
          return std::nullopt;
        return trace(BlockIndex, Index, Operation.Inputs[I], Operation.Addr);
      };
      const bool Plain =
          Operation.MemoryOrdering == NdMemoryOrdering::None &&
          Operation.MemoryAddressSpace == NdMemoryAddressSpace::Default;
      if (Operation.Opcode == NdOp::COPY && Operation.NumInputs == 1 && Plain)
        return Input(0);
      if ((Operation.Opcode == NdOp::INT_ADD ||
           Operation.Opcode == NdOp::INT_SUB) &&
          Operation.NumInputs == 2 && Plain) {
        unsigned Base = 0, Constant = 1;
        if (!Operation.Inputs[Constant].isConst() &&
            Operation.Opcode == NdOp::INT_ADD &&
            Operation.Inputs[Base].isConst())
          std::swap(Base, Constant);
        if (!Operation.Inputs[Constant].isConst())
          return std::nullopt;
        auto Result = Input(Base);
        if (!Result)
          return std::nullopt;
        int64_t Delta = static_cast<int64_t>(Operation.Inputs[Constant].Offset);
        if (Operation.Opcode == NdOp::INT_SUB) {
          if (Delta == INT64_MIN)
            return std::nullopt;
          Delta = -Delta;
        }
        return Result->add(Delta) ? Result : std::nullopt;
      }
      if (Operation.Opcode == NdOp::LOAD &&
          Operation.MemoryOrdering == NdMemoryOrdering::None &&
          Operation.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
        const auto Memory = lowMemoryOperands(Operation);
        if (!Memory.Complete || !Memory.Address || Memory.AccessSize != 8)
          return std::nullopt;
        auto Result = trace(BlockIndex, Index, *Memory.Address, Operation.Addr);
        return Result && Result->load(8) ? Result : std::nullopt;
      }
      return Path{Root{Root::Kind::Definition, BlockIndex, Index, Value.Space,
                       Value.Offset, Value.Size},
                  {}};
    }

    if (Value.isTemp())
      return std::nullopt;
    if (Block.Preds.empty()) {
      if (Block.StartAddr != Function.Entry)
        return std::nullopt;
      return Path{Root{Root::Kind::Entry, BlockIndex, 0, Value.Space,
                       Value.Offset, Value.Size},
                  {}};
    }
    std::optional<Path> Result;
    for (int PredecessorId : Block.Preds) {
      const auto Found = Blocks.find(PredecessorId);
      if (Found == Blocks.end())
        return std::nullopt;
      const auto &Predecessor = Function.Blocks[Found->second];
      if (std::find(Predecessor.Succs.begin(), Predecessor.Succs.end(),
                    Block.Id) == Predecessor.Succs.end())
        return std::nullopt;
      auto Incoming = trace(Found->second, Predecessor.Ops.size(), Value, 0);
      if (!Incoming || (Result && *Result != *Incoming))
        return std::nullopt;
      Result = std::move(Incoming);
    }
    return Result;
  }

public:
  Tracer(const BinaryImage &Image, const LowFunc &Function,
         const TargetRegInfo &TRI)
      : Image(Image), Function(Function), TRI(TRI) {
    size_t EntryBlocks = 0;
    size_t Edges = 0;
    std::map<int, std::set<int>> Predecessors, Successors;
    for (size_t I = 0; I < Function.Blocks.size(); ++I) {
      const auto &Block = Function.Blocks[I];
      Valid &= Blocks.emplace(Block.Id, I).second;
      EntryBlocks += Block.StartAddr == Function.Entry;
      if (Block.Preds.size() > 4096 || Block.Succs.size() > 4096 ||
          Edges > 131072 - Block.Preds.size() ||
          Edges + Block.Preds.size() > 131072 - Block.Succs.size()) {
        Valid = false;
        continue;
      }
      Edges += Block.Preds.size() + Block.Succs.size();
      auto &Pred = Predecessors[Block.Id];
      auto &Succ = Successors[Block.Id];
      Pred.insert(Block.Preds.begin(), Block.Preds.end());
      Succ.insert(Block.Succs.begin(), Block.Succs.end());
      Valid &= Pred.size() == Block.Preds.size();
      Valid &= Succ.size() == Block.Succs.size();
    }
    Valid &= EntryBlocks == 1;
    if (!Valid)
      return;
    for (const auto &[Id, Index] : Blocks) {
      (void)Index;
      for (int Pred : Predecessors[Id])
        if (!Blocks.count(Pred) || !Successors[Pred].count(Id))
          Valid = false;
      for (int Succ : Successors[Id])
        if (!Blocks.count(Succ) || !Predecessors[Succ].count(Id))
          Valid = false;
    }
  }

  bool valid() const { return Valid; }
  bool exhausted() const { return Exhausted || !RemainingBudget; }

  std::optional<Path> at(size_t Block, size_t Operation, const NdVar &Value) {
    Budget = 4096;
    Active.clear();
    return trace(Block, Operation, Value,
                 Operation < Function.Blocks[Block].Ops.size()
                     ? Function.Blocks[Block].Ops[Operation].Addr
                     : 0);
  }
};
} // namespace

std::map<va_t, SourceCallTypeHint>
buildSwiftValueWitnessCallHints(const BinaryImage &Image,
                                const LowFunc &Function) {
  std::map<va_t, SourceCallTypeHint> Result;
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) ||
      Function.Blocks.size() > 16384)
    return Result;
  struct Witness {
    SourceCallTypeHint Hint;
    unsigned Slot = 0;
    SourceABIValueLocation Metadata;
  };
  std::vector<Witness> Witnesses;
  constexpr std::array Operations = {
      SourceCallTypeHint::SwiftValueWitnessKind::
          InitializeBufferWithCopyOfBuffer,
      SourceCallTypeHint::SwiftValueWitnessKind::Destroy,
      SourceCallTypeHint::SwiftValueWitnessKind::InitializeWithCopy,
      SourceCallTypeHint::SwiftValueWitnessKind::AssignWithCopy,
      SourceCallTypeHint::SwiftValueWitnessKind::InitializeWithTake,
      SourceCallTypeHint::SwiftValueWitnessKind::AssignWithTake,
      SourceCallTypeHint::SwiftValueWitnessKind::GetEnumTagSinglePayload,
      SourceCallTypeHint::SwiftValueWitnessKind::StoreEnumTagSinglePayload};
  for (const auto Operation : Operations) {
    auto Hint = swiftValueWitnessSourceCallHint(Image.Arch, Operation);
    const auto Slot = swiftValueWitnessSlot(Operation);
    if (!Hint || !Slot || Hint->Signature.Parameters.empty())
      return Result;
    const auto &Metadata = Hint->Signature.Parameters.back().Location;
    if (Metadata.Kind != SourceABICarrierKind::IntegerRegister ||
        Metadata.ValueBytes != 8)
      return Result;
    Witnesses.push_back({std::move(*Hint), *Slot, Metadata});
  }

  size_t OperationCount = 0;
  std::map<va_t, unsigned> Occurrences;
  for (const auto &Block : Function.Blocks) {
    if (Block.Ops.size() > (1U << 20) - OperationCount)
      return Result;
    OperationCount += Block.Ops.size();
    for (const auto &Operation : Block.Ops)
      if (Operation.Opcode == NdOp::CALL ||
          Operation.Opcode == NdOp::INDIR_CALL) {
        auto &Count = Occurrences[Operation.Addr];
        Count += Count < 2;
      }
  }

  Tracer Trace(Image, Function, getTargetRegInfo(Image.Arch));
  if (!Trace.valid())
    return Result;
  for (size_t BlockIndex = 0; BlockIndex < Function.Blocks.size();
       ++BlockIndex) {
    const auto &Block = Function.Blocks[BlockIndex];
    for (size_t Index = 0; Index < Block.Ops.size(); ++Index) {
      const auto &Operation = Block.Ops[Index];
      if (Operation.Opcode != NdOp::INDIR_CALL || Operation.NumInputs != 1 ||
          Operation.MemoryOrdering != NdMemoryOrdering::None ||
          Operation.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          Occurrences[Operation.Addr] != 1)
        continue;
      const auto Target = Trace.at(BlockIndex, Index, Operation.Inputs[0]);
      if (!Target)
        continue;
      const SourceCallTypeHint *Match = nullptr;
      for (const auto &Witness : Witnesses) {
        const auto Type =
            Trace.at(BlockIndex, Index,
                     NdVar::reg(Witness.Metadata.RegisterOffset, uint16_t(8)));
        if (!Type)
          continue;
        if (Type->Base.TheKind == Root::Kind::Constant) {
          // The witness table pointer lives immediately before metadata.
          // Verify that exact image-backed slot, not merely the page base.
          const auto Address = literalMetadataAddress(*Type);
          if (!Address || *Address < 8 || *Address % 8 ||
              !Image.isDataAddress(*Address) ||
              !Image.isDataAddress(*Address - 8) ||
              !Image.readVA(*Address - 8, 8))
            continue;
        }
        Path Expected = *Type;
        if (!Expected.add(-8) || !Expected.load(8) ||
            !Expected.add(int64_t(Witness.Slot) * 8) || !Expected.load(8) ||
            *Target != Expected)
          continue;
        if (Match) {
          Match = nullptr;
          break;
        }
        Match = &Witness.Hint;
      }
      if (Match)
        Result.emplace(Operation.Addr, *Match);
    }
  }
  if (Trace.exhausted())
    return {};
  return Result;
}

} // namespace neverd

//===- SourceFrameBytes.cpp - Immutable stack byte observations ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Keep original-machine byte evidence separate from C string presentation.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/SourceFrameBytes.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/ImportCallee.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/SourceFrameAnalysis.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/RelocationProvenance.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace neverd {
namespace {

std::optional<std::vector<uint8_t>>
immutableDefinition(const LowFunc &Function, const BinaryImage &Image,
                    const SourceFrameDefinition &Definition,
                    size_t &Remaining) {
  if (Remaining < Function.Blocks.size())
    return std::nullopt;
  Remaining -= Function.Blocks.size();
  const auto Block = std::find_if(
      Function.Blocks.begin(), Function.Blocks.end(),
      [&](const LowBlock &B) { return B.Id == Definition.BlockId; });
  if (Block == Function.Blocks.end() ||
      Definition.OperationIndex >= Block->Ops.size() ||
      Definition.OperationIndex + 1 >= Block->Ops.size())
    return std::nullopt;
  const auto &Op = Block->Ops[Definition.OperationIndex];
  const auto Memory = lowMemoryOperands(Op);
  if (Op.Addr != Definition.Instruction || Op.Seq != Definition.Sequence ||
      Op.Output != Definition.Output || Op.Opcode != NdOp::LOAD ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !Memory.Complete || !Memory.Address || !Memory.AccessSize ||
      Memory.AccessSize != Op.Output.Size)
    return std::nullopt;
  LoadedCallSlotProof Proof;
  Proof.WorkRemaining = &Remaining;
  Proof.AllowAlignmentChecks = Image.Arch == Arch::X64;
  const auto Address =
      loadedCallSlot(Function, *Block, Definition.OperationIndex + 1,
                     Definition.Output, &Proof);
  if (!Address || *Address > InvalidVA - Memory.AccessSize)
    return std::nullopt;
  const va_t End = *Address + Memory.AccessSize;
  const Segment *Owner = nullptr;
  for (const auto &Segment : Image.Segments) {
    if (!Remaining)
      return std::nullopt;
    --Remaining;
    if (Segment.VA > InvalidVA - Segment.Size)
      return std::nullopt;
    if (*Address >= Segment.VA + Segment.Size || End <= Segment.VA)
      continue;
    if (Owner || *Address < Segment.VA || End > Segment.VA + Segment.Size ||
        !Segment.isReadable() || Segment.isWritable() || Segment.isExecutable())
      return std::nullopt;
    Owner = &Segment;
  }
  if (!Owner || Remaining < Image.Sections.size())
    return std::nullopt;
  // Loader queries inspect the same section inventory. Charge it once per
  // producer and reject ambiguous/partial ownership before reading bytes.
  Remaining -= Image.Sections.size();
  if (!Image.hasObjectDataProvenance(*Address))
    return std::nullopt;
  const Section *SectionOwner = nullptr;
  for (const auto &Section : Image.Sections) {
    if (!Section.isReadable())
      continue;
    if (Section.VA > InvalidVA - Section.Size)
      return std::nullopt;
    if (*Address >= Section.VA + Section.Size || End <= Section.VA)
      continue;
    if (SectionOwner || *Address < Section.VA ||
        End > Section.VA + Section.Size || Section.isWritable() ||
        Section.isExecutable())
      return std::nullopt;
    SectionOwner = &Section;
  }
  const unsigned PointerBytes = getTargetRegInfo(Image.Arch).PointerSize;
  const va_t First =
      *Address >= PointerBytes - 1 ? *Address - (PointerBytes - 1) : 0;
  // Charge the format-neutral relocation inventory once for this producer,
  // rather than scanning it independently for every byte of a vector.
  const size_t Relocations =
      Image.BaseRelocations.size() + Image.Relocations.size();
  if (Remaining < Relocations)
    return std::nullopt;
  Remaining -= Relocations;
  if (hasRelocationProvenanceIn(Image, First, End))
    return std::nullopt;
  const auto *Bytes = Image.readVA(*Address, Memory.AccessSize);
  if (!Bytes)
    return std::nullopt;
  return std::vector<uint8_t>(Bytes, Bytes + Memory.AccessSize);
}

} // namespace

std::vector<SourceFrameBytes>
observeSourceFrameBytes(const LowFunc &Function, const BinaryImage &Image) {
  std::vector<SourceFrameBytes> Result;
  if (Image.Arch != Arch::X64 && Image.Arch != Arch::AArch64)
    return Result;
  bool HasStores = false;
  size_t Remaining = limits::ir::kSourceFrameAnalysisRemaining;
  std::vector<std::pair<int, size_t>> Calls;
  for (const auto &Block : Function.Blocks)
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      if (!Remaining)
        return {};
      --Remaining;
      HasStores |= Block.Ops[I].Opcode == NdOp::STORE;
      if (Block.Ops[I].Opcode == NdOp::CALL ||
          Block.Ops[I].Opcode == NdOp::INDIR_CALL)
        Calls.emplace_back(Block.Id, I);
    }
  if (!HasStores || Calls.empty())
    return Result;
  std::vector<NdVar> Pointers;
  for (uint64_t Register :
       getTargetRegInfo(Image.Arch).integerParamRegs(Image.abiFormat()))
    Pointers.push_back(NdVar::reg(Register, 8));
  using Key = std::pair<int, size_t>;
  std::map<Key, std::optional<std::vector<uint8_t>>> Definitions;
  for (const auto &[BlockId, Index] : Calls) {
    if (!Remaining)
      break;
    const auto Storage = sourceFrameStorageBefore(
        Function, Image.Arch, Image.abiFormat(), BlockId, Index, Pointers,
        limits::ir::kSourceFrameTextBytes, &Remaining);
    const auto Block =
        std::find_if(Function.Blocks.begin(), Function.Blocks.end(),
                     [&](const LowBlock &B) { return B.Id == BlockId; });
    for (const auto &Prefix : Storage) {
      SourceFrameBytes Observation{
          Block->Ops[Index].Addr, Prefix.FrameOffset, {}};
      for (const auto &Byte : Prefix.Bytes) {
        if (!Remaining || !Byte.Initialized)
          break;
        --Remaining;
        if (Byte.Constant) {
          Observation.Bytes.push_back(*Byte.Constant);
          continue;
        }
        if (!Byte.Definition)
          break;
        const auto &Definition = *Byte.Definition;
        auto [Found, Inserted] = Definitions.try_emplace(
            Key{Definition.BlockId, Definition.OperationIndex});
        if (Inserted)
          Found->second =
              immutableDefinition(Function, Image, Definition, Remaining);
        if (!Found->second || Byte.DefinitionByte >= Found->second->size())
          break;
        Observation.Bytes.push_back((*Found->second)[Byte.DefinitionByte]);
      }
      if (!Observation.Bytes.empty()) {
        Result.push_back(std::move(Observation));
        if (Result.size() == limits::ir::kSourceFrameTextObservations)
          return Result;
      }
    }
  }
  return Result;
}

} // namespace neverd

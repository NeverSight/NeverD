//===- RegistrationCalleeABI.cpp - PE32 callee contracts ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationABIPrivate.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include <deque>
#include <set>

namespace neverd::registration_abi {

static const Import *checkedImportSlot(const BinaryImage &Image, va_t Target) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF)
    return nullptr;
  const auto *Import = Image.findImportAt(Target);
  if (!Import || Import->IATAddr != Target ||
      !Image.isValidImportStorageSlot(Import->IATAddr, Import->Name))
    return nullptr;
  for (const auto &Other : Image.Imports)
    if (Other.IATAddr == Import->IATAddr &&
        (Other.Name != Import->Name ||
         !llvm::StringRef(Other.Module).equals_insensitive(Import->Module)))
      return nullptr;
  const auto Storage = Image.collectImportStorageSlot(Import->IATAddr);
  const auto Slot = Storage.Slots.find(Import->IATAddr);
  if (Storage.Conflicts.count(Import->IATAddr) || Slot == Storage.Slots.end() ||
      Slot->second.Name != Import->Name || Slot->second.Addend)
    return nullptr;
  return Import;
}

std::optional<uint32_t>
checkedRegistrationImportStackPop(const BinaryImage &Image, va_t Target) {
  const auto *Named = Image.findImportAt(Target);
  const auto *Import =
      Named ? checkedImportSlot(Image, Named->IATAddr) : nullptr;
  if (!Import || Import->Name != "RaiseException" ||
      (!llvm::StringRef(Import->Module).equals_insensitive("kernel32.dll") &&
       !llvm::StringRef(Import->Module).equals_insensitive("kernelbase.dll")))
    return std::nullopt;
  return 16;
}

bool callerPCIsNotReadBack(const ImageFrameEffects &Effects) {
  auto Read = Effects.Reads.begin();
  for (const auto &[Begin, End] : Effects.CallerPCWrites) {
    while (Read != Effects.Reads.end() && Read->second <= Begin)
      ++Read;
    if (Read != Effects.Reads.end() && Read->first < End)
      return false;
  }
  return true;
}

bool chargeCalleeWork(size_t &Work, size_t Amount) {
  if (Work > limits::kMaxRegistrationEHStateWork ||
      Amount > limits::kMaxRegistrationEHStateWork - Work) {
    Work = limits::kMaxRegistrationEHStateWork;
    return false;
  }
  Work += Amount;
  return true;
}

bool chargeDecodedCallee(size_t &Work, const LowFunc &Function) {
  for (const auto &Block : Function.Blocks)
    if (!chargeCalleeWork(Work, Block.Ops.size() +
                                    Block.InstructionBoundaries.size() + 1))
      return false;
  return true;
}

bool collectCalleeCodeRanges(const LowFunc &Function, const BinaryImage &Image,
                             size_t &Work,
                             std::vector<ExceptionAddressRange> &Ranges) {
  if (auto Error = validateLowInstructionBoundaries(
          Function, LowInstructionBoundaryRequirement::Required)) {
    llvm::consumeError(std::move(Error));
    return false;
  }
  std::set<std::pair<va_t, va_t>> Instructions;
  for (const auto &Block : Function.Blocks)
    for (const auto &Boundary : Block.InstructionBoundaries) {
      if (!chargeCalleeWork(Work, 1) || !Boundary.Size ||
          Boundary.Address > UINT32_MAX ||
          Boundary.Size > uint64_t(UINT32_MAX) + 1 - Boundary.Address ||
          !Image.isCodeAddress(Boundary.Address) ||
          !Image.readVA(Boundary.Address, Boundary.Size))
        return false;
      Instructions.emplace(Boundary.Address, Boundary.Address + Boundary.Size);
    }
  copyExtents(Instructions, Ranges);
  return !Ranges.empty() && llvm::any_of(Ranges, [&](const auto &Range) {
    return Range.contains(Function.Entry);
  });
}

} // namespace neverd::registration_abi

namespace neverd {

std::optional<RegistrationLocalUnwindContract>
getCheckedX86LocalUnwindContract(const BinaryImage &Image, va_t Target,
                                 bool Indirect) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || Target > UINT32_MAX)
    return std::nullopt;
  va_t Slot = Target;
  if (!Indirect) {
    const auto Bytes =
        readImmutablePE32CodeBytes(Image, Target, 6, {Target + 2});
    if (!Bytes || (*Bytes)[0] != 0xff || (*Bytes)[1] != 0x25)
      return std::nullopt;
    Slot = readLE<uint32_t>(Bytes->data() + 2);
  }
  const auto *Import = registration_abi::checkedImportSlot(Image, Slot);
  if (!Import || Import->Name != "_local_unwind2" ||
      (!llvm::StringRef(Import->Module).equals_insensitive("msvcrt.dll") &&
       !llvm::StringRef(Import->Module)
            .equals_insensitive("vcruntime140.dll") &&
       !llvm::StringRef(Import->Module)
            .equals_insensitive("vcruntime140d.dll")))
    return std::nullopt;
  return RegistrationLocalUnwindContract{Target, Indirect};
}

std::optional<std::vector<RegistrationLocalUnwindContract>>
RegistrationCallCalleeIndex::localUnwindContracts(const LowFunc &Function) {
  std::vector<RegistrationLocalUnwindContract> Result;
  std::set<std::pair<va_t, bool>> Seen;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      if (!registration_abi::chargeCalleeWork(Work, 1))
        return std::nullopt;
      if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
          Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
          Op.Inputs[0].Size != 4)
        continue;
      const auto Identity =
          std::make_pair(Op.Inputs[0].Offset, Op.Opcode == NdOp::INDIR_CALL);
      if (!Seen.insert(Identity).second)
        continue;
      if (Seen.size() > 256)
        return std::nullopt;
      if (const auto Contract = getCheckedX86LocalUnwindContract(
              Image, Identity.first, Identity.second))
        Result.push_back(*Contract);
    }
  return Result;
}

namespace {
using registration_abi::callerPCIsNotReadBack;
using registration_abi::chargeCalleeWork;
using registration_abi::chargeDecodedCallee;
using registration_abi::collectCalleeCodeRanges;
using registration_abi::copyExtents;
using registration_abi::hasPrivateCallerFrame;
using registration_abi::ImageFrameEffects;

bool retainLeafEntryBlocks(LowFunc &Function, size_t &Work) {
  std::map<int, const LowBlock *> Blocks;
  std::deque<int> Pending;
  for (const auto &Block : Function.Blocks) {
    if (!chargeCalleeWork(Work, 1) || !Blocks.emplace(Block.Id, &Block).second)
      return false;
    if (Block.StartAddr == Function.Entry)
      Pending.push_back(Block.Id);
  }
  if (Pending.size() != 1)
    return false;
  std::set<int> Reached;
  while (!Pending.empty()) {
    const int Id = Pending.front();
    Pending.pop_front();
    if (!chargeCalleeWork(Work, 1) || !Blocks.count(Id))
      return false;
    if (!Reached.insert(Id).second)
      continue;
    const auto &Block = *Blocks.at(Id);
    if (!chargeCalleeWork(Work, Block.InstructionBoundaries.size() +
                                    Block.Succs.size()))
      return false;
    for (const auto &Boundary : Block.InstructionBoundaries)
      if (Boundary.Control == LowInstructionControl::Return &&
          Boundary.Immediate.value_or(0) != 0)
        return false;
    Pending.insert(Pending.end(), Block.Succs.begin(), Block.Succs.end());
  }
  // Only this physical entry is called. Address-taken roots without an
  // ordinary path from it do not contribute calls, effects or return cleanup.
  // Every retained edge and machine return was checked above; this projection
  // grants no ownership or rewrite permission for the discarded roots.
  llvm::erase_if(Function.Blocks,
                 [&](const auto &Block) { return !Reached.count(Block.Id); });
  for (auto &Block : Function.Blocks)
    llvm::erase_if(Block.Preds, [&](int Predecessor) {
      return !Reached.count(Predecessor);
    });
  Function.CalleePopBytes = 0;
  return true;
}

} // namespace

std::optional<RegistrationLeafCalleeABI>
getCheckedX86RegistrationLeafCalleeABI(const BinaryImage &Image, va_t Target,
                                       size_t *CumulativeWork) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || Target > UINT32_MAX ||
      !Image.isCodeAddress(Target) ||
      Image.ExceptionMetadata.findFunction(Target))
    return std::nullopt;
  size_t LocalWork = 0;
  size_t &Work = CumulativeWork ? *CumulativeWork : LocalWork;
  if (!chargeCalleeWork(Work, 1))
    return std::nullopt;
  Decoder Decoder;
  if (!Decoder.init(Image))
    return std::nullopt;
  CFGBuilder Builder;
  LowFunc Callee = Builder.build(Image, Decoder, Target, "abi-leaf");
  if (!chargeDecodedCallee(Work, Callee) || !Callee.hasCompleteLiftCoverage() ||
      Callee.Blocks.empty() || Callee.ExceptionMetadata ||
      !retainLeafEntryBlocks(Callee, Work))
    return std::nullopt;
  for (const auto &Block : Callee.Blocks) {
    if (Block.Succs.empty() &&
        (Block.Ops.empty() || Block.Ops.back().Opcode != NdOp::RETURN))
      return std::nullopt;
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
        return std::nullopt;
  }
  ImageFrameEffects Effects;
  bool IndependentScalarReturn = false;
  if (!hasPrivateCallerFrame(Callee, Image, Work, Effects, true, nullptr,
                             &IndependentScalarReturn))
    return std::nullopt;
  if (!callerPCIsNotReadBack(Effects))
    return std::nullopt;
  RegistrationLeafCalleeABI Result;
  Result.Target = Target;
  Result.HasIndependentScalarReturn = IndependentScalarReturn;
  if (!collectCalleeCodeRanges(Callee, Image, Work, Result.CodeRanges))
    return std::nullopt;
  copyExtents(Effects.ECXReads, Result.ECXReads);
  copyExtents(Effects.ECXWrites, Result.ECXWrites);
  copyExtents(Effects.Reads, Result.ImageReads);
  copyExtents(Effects.Writes, Result.ImageWrites);
  copyExtents(Effects.CallerPCWrites, Result.CallerPCWrites);
  return Result;
}

} // namespace neverd

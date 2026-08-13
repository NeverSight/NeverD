//===- MachOExceptionPatch.cpp - Mach-O unwind-record rewrite ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/MachO/MachOExceptionPatch.h"

#include "neverd/Object/MachOLayout.h"
#include "neverd/Object/SectionNames.h"
#include "neverd/Support/BinaryEncoding.h"
#include "neverd/backend/codegen/BinaryRewriter.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

#define DEBUG_TYPE "neverd-macho-patch"

namespace neverd {

using namespace llvm::MachO;

namespace {

llvm::Error patchError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "macho exception patch: " + Message);
}

/// Return the byte offset at which another .eh_frame sequence can be appended.
/// A zero-length record is a terminator, so it is replaced rather than left
/// between the original and regenerated records.
std::optional<uint64_t> getEHFrameAppendOffset(llvm::ArrayRef<uint8_t> Bytes) {
  uint64_t Off = 0;
  while (Off < Bytes.size()) {
    if (!rangeInBounds(Off, sizeof(uint32_t), Bytes.size()))
      return std::nullopt;
    uint32_t Length = readLE<uint32_t>(Bytes.data() + Off);
    if (Length == 0)
      return Off;

    uint64_t RecordSize = 0;
    if (Length == std::numeric_limits<uint32_t>::max()) {
      if (!rangeInBounds(Off, sizeof(uint32_t) + sizeof(uint64_t),
                         Bytes.size()))
        return std::nullopt;
      uint64_t ExtendedLength =
          readLE<uint64_t>(Bytes.data() + Off + sizeof(uint32_t));
      if (ExtendedLength >
          std::numeric_limits<uint64_t>::max() -
              (sizeof(uint32_t) + sizeof(uint64_t)))
        return std::nullopt;
      RecordSize = sizeof(uint32_t) + sizeof(uint64_t) + ExtendedLength;
    } else {
      RecordSize = sizeof(uint32_t) + static_cast<uint64_t>(Length);
    }
    if (RecordSize == 0 || !rangeInBounds(Off, RecordSize, Bytes.size()))
      return std::nullopt;
    Off += RecordSize;
  }
  return Off;
}

bool appendGeneratedEHFrame(std::vector<uint8_t> &Binary,
                            const MachOEHFrameRegion &Region,
                            const CompiledSection &Generated) {
  if (Generated.Name != section_names::macho::EhFrame ||
      Generated.IsInImage || Generated.VA != Region.AppendVA ||
      Generated.Size != Generated.ExternalBytes.size() ||
      Generated.Size > Region.LimitFileOff - Region.AppendFileOff ||
      !rangeInBounds(Region.AppendFileOff, Generated.Size, Binary.size()))
    return false;

  if (!Generated.ExternalBytes.empty())
    std::memcpy(Binary.data() + Region.AppendFileOff,
                Generated.ExternalBytes.data(), Generated.ExternalBytes.size());

  uint64_t NewSize =
      Region.AppendVA - Region.SectionVA + Generated.ExternalBytes.size();
  if (Region.Is64) {
    if (!rangeInBounds(Region.SectionHeaderOff, sizeof(section_64),
                       Binary.size()))
      return false;
    reinterpret_cast<section_64 *>(Binary.data() + Region.SectionHeaderOff)
        ->size = NewSize;
  } else {
    if (NewSize > std::numeric_limits<uint32_t>::max() ||
        !rangeInBounds(Region.SectionHeaderOff, sizeof(section), Binary.size()))
      return false;
    reinterpret_cast<section *>(Binary.data() + Region.SectionHeaderOff)->size =
        static_cast<uint32_t>(NewSize);
  }
  return true;
}

} // namespace

std::optional<MachOEHFrameRegion>
findMachOEHFrameRegion(llvm::ArrayRef<uint8_t> Binary) {
  std::optional<MachOEHFrameRegion> Found;
  forEachMachOLoadCommand(
      Binary.data(), Binary.size(),
      [&](const uint8_t *LCPtr, uint32_t Cmd, uint32_t CmdSize, bool Is64) {
        if (Found || Cmd != getMachOSegmentCmdID(Is64))
          return;
        MachOSegFields Seg = readMachOSegment(LCPtr, Is64);
        if (readMachOName(Seg.SegName) != section_names::macho::TextSeg ||
            Seg.FileOff > std::numeric_limits<uint64_t>::max() - Seg.FileSize)
          return;

        const uint32_t BaseSize = getMachOSegmentCmdSize(Is64);
        const uint32_t SectionSize = getMachOSectionSize(Is64);
        const uint32_t Nsects =
            Is64 ? reinterpret_cast<const segment_command_64 *>(LCPtr)->nsects
                 : reinterpret_cast<const segment_command *>(LCPtr)->nsects;
        if (Nsects >
                (std::numeric_limits<uint32_t>::max() - BaseSize) /
                    SectionSize ||
            BaseSize + Nsects * SectionSize > CmdSize)
          return;

        uint64_t EHVA = 0;
        uint64_t EHSize = 0;
        uint64_t EHFileOff = 0;
        uint64_t HeaderOff = 0;
        std::vector<uint64_t> SectionOffsets;
        for (uint32_t I = 0; I < Nsects; ++I) {
          const uint8_t *SectionPtr = LCPtr + BaseSize + I * SectionSize;
          uint64_t Addr = 0;
          uint64_t Size = 0;
          uint32_t FileOff = 0;
          const char *Name = nullptr;
          if (Is64) {
            const auto *Section =
                reinterpret_cast<const section_64 *>(SectionPtr);
            Addr = Section->addr;
            Size = Section->size;
            FileOff = Section->offset;
            Name = Section->sectname;
          } else {
            const auto *Section = reinterpret_cast<const section *>(SectionPtr);
            Addr = Section->addr;
            Size = Section->size;
            FileOff = Section->offset;
            Name = Section->sectname;
          }
          if (FileOff != 0)
            SectionOffsets.push_back(FileOff);
          if (readMachOName(Name) != section_names::macho::EhFrame)
            continue;
          EHVA = Addr;
          EHSize = Size;
          EHFileOff = FileOff;
          HeaderOff = static_cast<uint64_t>(SectionPtr - Binary.data());
        }
        if (EHVA == 0 || EHSize == 0 || EHFileOff == 0 ||
            !rangeInBounds(EHFileOff, EHSize, Binary.size()))
          return;

        auto LogicalSize = getEHFrameAppendOffset(llvm::ArrayRef<uint8_t>(
            Binary.data() + EHFileOff, static_cast<size_t>(EHSize)));
        if (!LogicalSize || *LogicalSize > EHSize ||
            EHVA > std::numeric_limits<uint64_t>::max() - *LogicalSize ||
            EHFileOff > std::numeric_limits<uint64_t>::max() - *LogicalSize)
          return;

        uint64_t AppendFileOff = EHFileOff + *LogicalSize;
        uint64_t Limit = Seg.FileOff + Seg.FileSize;
        for (uint64_t Offset : SectionOffsets)
          if (Offset > AppendFileOff)
            Limit = std::min(Limit, Offset);
        if (Limit < AppendFileOff || Limit > Binary.size())
          return;

        MachOEHFrameRegion Region;
        Region.Is64 = Is64;
        Region.SectionVA = EHVA;
        Region.SectionFileOff = EHFileOff;
        Region.AppendVA = EHVA + *LogicalSize;
        Region.AppendFileOff = AppendFileOff;
        Region.LimitFileOff = Limit;
        Region.SectionHeaderOff = HeaderOff;
        Found = Region;
      });
  return Found;
}

bool requiresRegisteredMachOEHFrame(const llvm::Module &Mod) {
  for (const llvm::Function &Function : Mod) {
    if (Function.isDeclaration())
      continue;
    if (Function.hasPersonalityFn())
      return true;
    for (const llvm::BasicBlock &Block : Function)
      for (const llvm::Instruction &Instruction : Block)
        if (llvm::isa<llvm::InvokeInst, llvm::LandingPadInst, llvm::ResumeInst>(
                Instruction))
          return true;
  }
  return false;
}

llvm::Error installMachOEHFrame(std::vector<uint8_t> &Binary,
                                const std::optional<MachOEHFrameRegion> &Region,
                                const CompiledImage &Compiled,
                                const llvm::Module &Mod) {
  const CompiledSection *Generated = nullptr;
  for (const CompiledSection &Section : Compiled.Sections)
    if (Section.IsAllocated && Section.Name == section_names::macho::EhFrame) {
      Generated = &Section;
      break;
    }

  // When the input has no __eh_frame section, the compiler leaves the
  // generated section in CompiledImage::Bytes as before.  Only externally
  // placed bytes need to be appended to an existing section.
  if (!Generated || Generated->IsInImage)
    return llvm::Error::success();

  const bool Registered =
      Region && appendGeneratedEHFrame(Binary, *Region, *Generated);
  if (!Registered && requiresRegisteredMachOEHFrame(Mod))
    return patchError("cannot register regenerated __eh_frame");

  LLVM_DEBUG({
    if (!Registered)
      llvm::dbgs() << "macho exception patch: omitting unregistered CFI-only "
                      "__eh_frame records\n";
  });
  return llvm::Error::success();
}

} // namespace neverd

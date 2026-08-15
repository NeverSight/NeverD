//===- MachOCompactUnwindPatchTests.cpp - Generated record tests ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/MachO/MachOCompactUnwindPatch.h"
#include "neverd/backend/codegen/MachO/MachOExceptionPatch.h"
#include "neverd/backend/codegen/MachO/MachOPatch.h"
#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/object/MachOLayout.h"
#include "neverd/object/SectionNames.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace neverd {
namespace {

using Failure = MachOCompactUnwindParseFailure;
using BindFailure = MachOCompactUnwindDwarfBindFailure;
using MapFailure = MachOCompactUnwindRangeMapFailure;
using MergeFailure = MachOCompactUnwindMergeFailure;
using EncodeFailure = MachOCompactUnwindEncodeFailure;
using InstallFailure = MachOCompactUnwindInstallFailure;

constexpr uint64_t kImageBase = 0x100000000ULL;
constexpr uint64_t kCodeVA = kImageBase + 0x1000;
constexpr uint64_t kLSDAVA = kImageBase + 0x2000;
constexpr uint64_t kPersonalitySlotVA = kImageBase + 0x3000;
constexpr uint64_t kMachO32Base = 0x10000000;

void setMachOName(char (&Destination)[16], llvm::StringRef Name) {
  ASSERT_LE(Name.size(), sizeof(Destination));
  std::memset(Destination, 0, sizeof(Destination));
  std::memcpy(Destination, Name.data(), Name.size());
}

uint64_t sectionHeaderOffset(bool Is64, uint32_t Index) {
  return (Is64 ? sizeof(llvm::MachO::mach_header_64)
               : sizeof(llvm::MachO::mach_header)) +
         (Is64 ? sizeof(llvm::MachO::segment_command_64)
               : sizeof(llvm::MachO::segment_command)) +
         uint64_t(Index) * (Is64 ? sizeof(llvm::MachO::section_64)
                                 : sizeof(llvm::MachO::section));
}

std::vector<uint8_t> makeUnwindInfoMachO(bool Is64, bool IncludeUnwind = true) {
  using namespace llvm::MachO;
  constexpr uint32_t FileSize = 0x600;
  constexpr uint32_t SegmentFileSize = 0x500;
  constexpr uint32_t TextOff = 0x200;
  constexpr uint32_t UnwindOff = 0x280;
  constexpr uint32_t ConstOff = 0x380;
  constexpr uint32_t TextSize = 0x40;
  constexpr uint32_t UnwindSize = 0x80;
  constexpr uint32_t ConstSize = 0x20;
  constexpr uint32_t SectionCount = 3;
  const uint64_t BaseVA = Is64 ? kImageBase : kMachO32Base;
  const uint32_t CommandSize =
      (Is64 ? sizeof(segment_command_64) : sizeof(segment_command)) +
      SectionCount * (Is64 ? sizeof(section_64) : sizeof(section));

  std::vector<uint8_t> Binary(FileSize, 0);
  if (Is64) {
    auto *Header = reinterpret_cast<mach_header_64 *>(Binary.data());
    Header->magic = MH_MAGIC_64;
    Header->cputype = CPU_TYPE_ARM64;
    Header->cpusubtype = CPU_SUBTYPE_ARM64_ALL;
    Header->filetype = MH_EXECUTE;
    Header->ncmds = 1;
    Header->sizeofcmds = CommandSize;

    auto *Segment = reinterpret_cast<segment_command_64 *>(
        Binary.data() + sizeof(mach_header_64));
    Segment->cmd = LC_SEGMENT_64;
    Segment->cmdsize = CommandSize;
    setMachOName(Segment->segname, section_names::macho::TextSeg);
    Segment->vmaddr = BaseVA;
    Segment->vmsize = SegmentFileSize;
    Segment->fileoff = 0;
    Segment->filesize = SegmentFileSize;
    Segment->maxprot = VM_PROT_READ | VM_PROT_EXECUTE;
    Segment->initprot = VM_PROT_READ | VM_PROT_EXECUTE;
    Segment->nsects = SectionCount;

    auto *Sections = reinterpret_cast<section_64 *>(Segment + 1);
    const std::array<const char *, SectionCount> Names = {
        section_names::macho::Text,
        IncludeUnwind ? section_names::macho::Unwind : "__metadata", "__const"};
    const std::array<uint32_t, SectionCount> Offsets = {TextOff, UnwindOff,
                                                        ConstOff};
    const std::array<uint32_t, SectionCount> Sizes = {TextSize, UnwindSize,
                                                      ConstSize};
    for (uint32_t I = 0; I < SectionCount; ++I) {
      setMachOName(Sections[I].sectname, Names[I]);
      setMachOName(Sections[I].segname, section_names::macho::TextSeg);
      Sections[I].addr = BaseVA + Offsets[I];
      Sections[I].size = Sizes[I];
      Sections[I].offset = Offsets[I];
      Sections[I].align = 2;
      Sections[I].flags = S_REGULAR;
    }
  } else {
    auto *Header = reinterpret_cast<mach_header *>(Binary.data());
    Header->magic = MH_MAGIC;
    Header->cputype = CPU_TYPE_X86;
    Header->cpusubtype = CPU_SUBTYPE_I386_ALL;
    Header->filetype = MH_EXECUTE;
    Header->ncmds = 1;
    Header->sizeofcmds = CommandSize;

    auto *Segment = reinterpret_cast<segment_command *>(Binary.data() +
                                                        sizeof(mach_header));
    Segment->cmd = LC_SEGMENT;
    Segment->cmdsize = CommandSize;
    setMachOName(Segment->segname, section_names::macho::TextSeg);
    Segment->vmaddr = static_cast<uint32_t>(BaseVA);
    Segment->vmsize = SegmentFileSize;
    Segment->fileoff = 0;
    Segment->filesize = SegmentFileSize;
    Segment->maxprot = VM_PROT_READ | VM_PROT_EXECUTE;
    Segment->initprot = VM_PROT_READ | VM_PROT_EXECUTE;
    Segment->nsects = SectionCount;

    auto *Sections = reinterpret_cast<section *>(Segment + 1);
    const std::array<const char *, SectionCount> Names = {
        section_names::macho::Text,
        IncludeUnwind ? section_names::macho::Unwind : "__metadata", "__const"};
    const std::array<uint32_t, SectionCount> Offsets = {TextOff, UnwindOff,
                                                        ConstOff};
    const std::array<uint32_t, SectionCount> Sizes = {TextSize, UnwindSize,
                                                      ConstSize};
    for (uint32_t I = 0; I < SectionCount; ++I) {
      setMachOName(Sections[I].sectname, Names[I]);
      setMachOName(Sections[I].segname, section_names::macho::TextSeg);
      Sections[I].addr = static_cast<uint32_t>(BaseVA + Offsets[I]);
      Sections[I].size = Sizes[I];
      Sections[I].offset = Offsets[I];
      Sections[I].align = 2;
      Sections[I].flags = S_REGULAR;
    }
  }
  return Binary;
}

uint8_t *appendFixtureLoadCommand(std::vector<uint8_t> &Binary, bool Is64,
                                  uint32_t CommandID, uint32_t CommandSize) {
  using namespace llvm::MachO;
  constexpr uint64_t FirstSectionFileOff = 0x200;
  const uint64_t HeaderSize =
      Is64 ? sizeof(mach_header_64) : sizeof(mach_header);
  const uint32_t ExistingSize =
      Is64 ? reinterpret_cast<const mach_header_64 *>(Binary.data())->sizeofcmds
           : reinterpret_cast<const mach_header *>(Binary.data())->sizeofcmds;
  const uint64_t CommandOff = HeaderSize + ExistingSize;
  if (CommandOff > FirstSectionFileOff ||
      CommandSize > FirstSectionFileOff - CommandOff) {
    ADD_FAILURE() << "fixture load command overlaps section storage";
    return nullptr;
  }

  std::memset(Binary.data() + CommandOff, 0, CommandSize);
  auto *Command = reinterpret_cast<load_command *>(Binary.data() + CommandOff);
  Command->cmd = CommandID;
  Command->cmdsize = CommandSize;
  if (Is64) {
    auto *Header = reinterpret_cast<mach_header_64 *>(Binary.data());
    ++Header->ncmds;
    Header->sizeofcmds += CommandSize;
  } else {
    auto *Header = reinterpret_cast<mach_header *>(Binary.data());
    ++Header->ncmds;
    Header->sizeofcmds += CommandSize;
  }
  return Binary.data() + CommandOff;
}

void expectLocateFailure(
    llvm::Expected<std::optional<MachOCompactUnwindRegion>> Result,
    MachOCompactUnwindLocateFailure Expected) {
  ASSERT_FALSE(static_cast<bool>(Result));
  bool SawTypedError = false;
  llvm::Error Unhandled = llvm::handleErrors(
      Result.takeError(), [&](const MachOCompactUnwindLocateError &Error) {
        SawTypedError = true;
        EXPECT_EQ(Error.reason(), Expected);
      });
  if (Unhandled)
    ADD_FAILURE() << llvm::toString(std::move(Unhandled));
  EXPECT_TRUE(SawTypedError);
}

TEST(MachOCompactUnwindLocate, FindsExact32And64BitSectionExtents) {
  for (bool Is64 : {false, true}) {
    std::vector<uint8_t> Binary = makeUnwindInfoMachO(Is64);
    Binary[0x300] = 0xa5; // Unowned gap bytes are not advertised as capacity.
    auto RegionOrErr = findMachOCompactUnwindRegion(Binary);
    ASSERT_TRUE(static_cast<bool>(RegionOrErr))
        << llvm::toString(RegionOrErr.takeError());
    ASSERT_TRUE(RegionOrErr->has_value());
    const MachOCompactUnwindRegion &Region = **RegionOrErr;
    EXPECT_EQ(Region.Is64, Is64);
    EXPECT_EQ(Region.MachHeaderVA, Is64 ? kImageBase : kMachO32Base);
    EXPECT_EQ(Region.SectionVA, (Is64 ? kImageBase : kMachO32Base) + 0x280);
    EXPECT_EQ(Region.SectionFileOff, 0x280u);
    EXPECT_EQ(Region.SectionSize, 0x80u);
    EXPECT_EQ(Region.LimitFileOff, 0x300u);
    EXPECT_EQ(Region.SectionHeaderOff, sectionHeaderOffset(Is64, 1));
  }
}

TEST(MachOCompactUnwindLocate, DistinguishesMissingFromMalformed) {
  std::vector<uint8_t> Binary =
      makeUnwindInfoMachO(/*Is64=*/true, /*IncludeUnwind=*/false);
  auto MissingOrErr = findMachOCompactUnwindRegion(Binary);
  ASSERT_TRUE(static_cast<bool>(MissingOrErr))
      << llvm::toString(MissingOrErr.takeError());
  EXPECT_FALSE(MissingOrErr->has_value());

  auto *Header = reinterpret_cast<llvm::MachO::mach_header_64 *>(Binary.data());
  auto *Segment = reinterpret_cast<llvm::MachO::segment_command_64 *>(
      Binary.data() + sizeof(llvm::MachO::mach_header_64));
  Segment->cmdsize += 8;
  Header->sizeofcmds += 8;
  expectLocateFailure(findMachOCompactUnwindRegion(Binary),
                      MachOCompactUnwindLocateFailure::InvalidLoadCommands);
}

TEST(MachOCompactUnwindLocate, RejectsDuplicateAndMisplacedSections) {
  std::vector<uint8_t> Duplicate = makeUnwindInfoMachO(/*Is64=*/true);
  auto *DuplicateSection = reinterpret_cast<llvm::MachO::section_64 *>(
      Duplicate.data() + sectionHeaderOffset(/*Is64=*/true, 2));
  setMachOName(DuplicateSection->sectname, section_names::macho::Unwind);
  expectLocateFailure(findMachOCompactUnwindRegion(Duplicate),
                      MachOCompactUnwindLocateFailure::AmbiguousSection);

  std::vector<uint8_t> Misplaced = makeUnwindInfoMachO(/*Is64=*/true);
  auto *MisplacedSection = reinterpret_cast<llvm::MachO::section_64 *>(
      Misplaced.data() + sectionHeaderOffset(/*Is64=*/true, 1));
  setMachOName(MisplacedSection->segname, "__DATA");
  expectLocateFailure(findMachOCompactUnwindRegion(Misplaced),
                      MachOCompactUnwindLocateFailure::InvalidFileLayout);
}

TEST(MachOCompactUnwindLocate, RejectsInvalidStorageAndLinearMapping) {
  std::vector<uint8_t> ZeroLength = makeUnwindInfoMachO(/*Is64=*/true);
  auto *ZeroLengthSection = reinterpret_cast<llvm::MachO::section_64 *>(
      ZeroLength.data() + sectionHeaderOffset(/*Is64=*/true, 1));
  ZeroLengthSection->size = 0;
  expectLocateFailure(findMachOCompactUnwindRegion(ZeroLength),
                      MachOCompactUnwindLocateFailure::InvalidSection);

  std::vector<uint8_t> Virtual = makeUnwindInfoMachO(/*Is64=*/true);
  auto *VirtualSection = reinterpret_cast<llvm::MachO::section_64 *>(
      Virtual.data() + sectionHeaderOffset(/*Is64=*/true, 1));
  VirtualSection->flags = llvm::MachO::S_ZEROFILL;
  expectLocateFailure(findMachOCompactUnwindRegion(Virtual),
                      MachOCompactUnwindLocateFailure::InvalidSection);

  std::vector<uint8_t> NonLinear = makeUnwindInfoMachO(/*Is64=*/true);
  auto *NonLinearSection = reinterpret_cast<llvm::MachO::section_64 *>(
      NonLinear.data() + sectionHeaderOffset(/*Is64=*/true, 1));
  NonLinearSection->addr += 4;
  expectLocateFailure(findMachOCompactUnwindRegion(NonLinear),
                      MachOCompactUnwindLocateFailure::InvalidFileLayout);
}

TEST(MachOCompactUnwindLocate, RejectsOverlappingFileBackedSections) {
  std::vector<uint8_t> Binary = makeUnwindInfoMachO(/*Is64=*/true);
  auto *Overlapping = reinterpret_cast<llvm::MachO::section_64 *>(
      Binary.data() + sectionHeaderOffset(/*Is64=*/true, 2));
  Overlapping->offset = 0x2f0;
  Overlapping->addr = kImageBase + Overlapping->offset;
  expectLocateFailure(findMachOCompactUnwindRegion(Binary),
                      MachOCompactUnwindLocateFailure::InvalidFileLayout);
}

TEST(MachOCompactUnwindLocate,
     RejectsOppositeWidthAndTruncatedKnownLoadCommands) {
  using namespace llvm::MachO;
  for (bool Is64 : {false, true}) {
    std::vector<uint8_t> WrongWidth =
        makeUnwindInfoMachO(Is64, /*IncludeUnwind=*/false);
    const uint32_t WrongSegmentID = Is64 ? LC_SEGMENT : LC_SEGMENT_64;
    const uint32_t WrongSegmentSize =
        Is64 ? sizeof(segment_command) : sizeof(segment_command_64);
    ASSERT_NE(appendFixtureLoadCommand(WrongWidth, Is64, WrongSegmentID,
                                       WrongSegmentSize),
              nullptr);
    expectLocateFailure(findMachOCompactUnwindRegion(WrongWidth),
                        MachOCompactUnwindLocateFailure::InvalidLoadCommands);

    std::vector<uint8_t> Truncated =
        makeUnwindInfoMachO(Is64, /*IncludeUnwind=*/false);
    ASSERT_NE(appendFixtureLoadCommand(Truncated, Is64, LC_SYMTAB,
                                       sizeof(load_command)),
              nullptr);
    expectLocateFailure(findMachOCompactUnwindRegion(Truncated),
                        MachOCompactUnwindLocateFailure::InvalidLoadCommands);

    std::vector<uint8_t> TruncatedBuild =
        makeUnwindInfoMachO(Is64, /*IncludeUnwind=*/false);
    uint8_t *BuildCommand = appendFixtureLoadCommand(
        TruncatedBuild, Is64, LC_BUILD_VERSION, sizeof(build_version_command));
    ASSERT_NE(BuildCommand, nullptr);
    reinterpret_cast<build_version_command *>(BuildCommand)->ntools = 1;
    expectLocateFailure(findMachOCompactUnwindRegion(TruncatedBuild),
                        MachOCompactUnwindLocateFailure::InvalidLoadCommands);

    std::vector<uint8_t> ValidBuild =
        makeUnwindInfoMachO(Is64, /*IncludeUnwind=*/false);
    BuildCommand = appendFixtureLoadCommand(ValidBuild, Is64, LC_BUILD_VERSION,
                                            sizeof(build_version_command) +
                                                sizeof(build_tool_version));
    ASSERT_NE(BuildCommand, nullptr);
    reinterpret_cast<build_version_command *>(BuildCommand)->ntools = 1;
    auto MissingOrErr = findMachOCompactUnwindRegion(ValidBuild);
    ASSERT_TRUE(static_cast<bool>(MissingOrErr))
        << llvm::toString(MissingOrErr.takeError());
    EXPECT_FALSE(MissingOrErr->has_value());
  }
}

TEST(MachOCompactUnwindLocate, RejectsNonTargetSectionOverlappingHeaders) {
  for (bool Is64 : {false, true}) {
    std::vector<uint8_t> Binary = makeUnwindInfoMachO(Is64);
    const uint64_t BaseVA = Is64 ? kImageBase : kMachO32Base;
    if (Is64) {
      auto *Section = reinterpret_cast<llvm::MachO::section_64 *>(
          Binary.data() + sectionHeaderOffset(Is64, 0));
      Section->offset = 0x100;
      Section->addr = BaseVA + Section->offset;
    } else {
      auto *Section = reinterpret_cast<llvm::MachO::section *>(
          Binary.data() + sectionHeaderOffset(Is64, 0));
      Section->offset = 0x100;
      Section->addr = static_cast<uint32_t>(BaseVA + Section->offset);
    }
    expectLocateFailure(findMachOCompactUnwindRegion(Binary),
                        MachOCompactUnwindLocateFailure::InvalidFileLayout);

    std::vector<uint8_t> Adjacent = makeUnwindInfoMachO(Is64);
    const uint64_t LoadCommandsEnd =
        Is64 ? sizeof(llvm::MachO::mach_header_64) +
                   reinterpret_cast<const llvm::MachO::mach_header_64 *>(
                       Adjacent.data())
                       ->sizeofcmds
             : sizeof(llvm::MachO::mach_header) +
                   reinterpret_cast<const llvm::MachO::mach_header *>(
                       Adjacent.data())
                       ->sizeofcmds;
    if (Is64) {
      auto *Section = reinterpret_cast<llvm::MachO::section_64 *>(
          Adjacent.data() + sectionHeaderOffset(Is64, 0));
      Section->offset = static_cast<uint32_t>(LoadCommandsEnd);
      Section->addr = BaseVA + Section->offset;
    } else {
      auto *Section = reinterpret_cast<llvm::MachO::section *>(
          Adjacent.data() + sectionHeaderOffset(Is64, 0));
      Section->offset = static_cast<uint32_t>(LoadCommandsEnd);
      Section->addr = static_cast<uint32_t>(BaseVA + Section->offset);
    }
    auto RegionOrErr = findMachOCompactUnwindRegion(Adjacent);
    ASSERT_TRUE(static_cast<bool>(RegionOrErr))
        << llvm::toString(RegionOrErr.takeError());
    EXPECT_TRUE(RegionOrErr->has_value());
  }
}

TEST(MachOCompactUnwindLocate, RejectsOverlappingSegmentVMRanges) {
  using namespace llvm::MachO;
  for (bool Is64 : {false, true}) {
    const uint64_t BaseVA = Is64 ? kImageBase : kMachO32Base;
    std::vector<uint8_t> Overlap = makeUnwindInfoMachO(Is64);
    uint8_t *Command = appendFixtureLoadCommand(
        Overlap, Is64, Is64 ? LC_SEGMENT_64 : LC_SEGMENT,
        Is64 ? sizeof(segment_command_64) : sizeof(segment_command));
    ASSERT_NE(Command, nullptr);
    if (Is64) {
      auto *Segment = reinterpret_cast<segment_command_64 *>(Command);
      setMachOName(Segment->segname, "__DATA");
      Segment->vmaddr = BaseVA + 0x280;
      Segment->vmsize = 0x100;
      Segment->fileoff = 0x500;
      Segment->filesize = 0x100;
    } else {
      auto *Segment = reinterpret_cast<segment_command *>(Command);
      setMachOName(Segment->segname, "__DATA");
      Segment->vmaddr = static_cast<uint32_t>(BaseVA + 0x280);
      Segment->vmsize = 0x100;
      Segment->fileoff = 0x500;
      Segment->filesize = 0x100;
    }
    expectLocateFailure(findMachOCompactUnwindRegion(Overlap),
                        MachOCompactUnwindLocateFailure::InvalidFileLayout);

    std::vector<uint8_t> Adjacent = makeUnwindInfoMachO(Is64);
    Command = appendFixtureLoadCommand(
        Adjacent, Is64, Is64 ? LC_SEGMENT_64 : LC_SEGMENT,
        Is64 ? sizeof(segment_command_64) : sizeof(segment_command));
    ASSERT_NE(Command, nullptr);
    if (Is64) {
      auto *Segment = reinterpret_cast<segment_command_64 *>(Command);
      setMachOName(Segment->segname, "__DATA");
      Segment->vmaddr = BaseVA + 0x500;
      Segment->vmsize = 0x100;
      Segment->fileoff = 0x500;
      Segment->filesize = 0x100;
    } else {
      auto *Segment = reinterpret_cast<segment_command *>(Command);
      setMachOName(Segment->segname, "__DATA");
      Segment->vmaddr = static_cast<uint32_t>(BaseVA + 0x500);
      Segment->vmsize = 0x100;
      Segment->fileoff = 0x500;
      Segment->filesize = 0x100;
    }
    auto RegionOrErr = findMachOCompactUnwindRegion(Adjacent);
    ASSERT_TRUE(static_cast<bool>(RegionOrErr))
        << llvm::toString(RegionOrErr.takeError());
    EXPECT_TRUE(RegionOrErr->has_value());
  }
}

TEST(MachOCompactUnwindLocate, AuditsVirtualSectionVMRanges) {
  using namespace llvm::MachO;
  for (bool Is64 : {false, true}) {
    const uint64_t BaseVA = Is64 ? kImageBase : kMachO32Base;
    std::vector<uint8_t> Overlap = makeUnwindInfoMachO(Is64);
    if (Is64) {
      auto *Section = reinterpret_cast<section_64 *>(
          Overlap.data() + sectionHeaderOffset(Is64, 2));
      Section->addr = BaseVA + 0x2f0;
      Section->size = 0x20;
      Section->flags = S_ZEROFILL;
    } else {
      auto *Section = reinterpret_cast<section *>(Overlap.data() +
                                                  sectionHeaderOffset(Is64, 2));
      Section->addr = static_cast<uint32_t>(BaseVA + 0x2f0);
      Section->size = 0x20;
      Section->flags = S_ZEROFILL;
    }
    expectLocateFailure(findMachOCompactUnwindRegion(Overlap),
                        MachOCompactUnwindLocateFailure::InvalidFileLayout);

    std::vector<uint8_t> Outside = makeUnwindInfoMachO(Is64);
    if (Is64) {
      auto *Section = reinterpret_cast<section_64 *>(
          Outside.data() + sectionHeaderOffset(Is64, 2));
      Section->addr = BaseVA + 0x4f0;
      Section->size = 0x20;
      Section->flags = S_ZEROFILL;
    } else {
      auto *Section = reinterpret_cast<section *>(Outside.data() +
                                                  sectionHeaderOffset(Is64, 2));
      Section->addr = static_cast<uint32_t>(BaseVA + 0x4f0);
      Section->size = 0x20;
      Section->flags = S_ZEROFILL;
    }
    expectLocateFailure(findMachOCompactUnwindRegion(Outside),
                        MachOCompactUnwindLocateFailure::InvalidFileLayout);

    std::vector<uint8_t> Adjacent = makeUnwindInfoMachO(Is64);
    if (Is64) {
      auto *Section = reinterpret_cast<section_64 *>(
          Adjacent.data() + sectionHeaderOffset(Is64, 2));
      Section->addr = BaseVA + 0x300;
      Section->size = 0x20;
      Section->flags = S_ZEROFILL;
    } else {
      auto *Section = reinterpret_cast<section *>(Adjacent.data() +
                                                  sectionHeaderOffset(Is64, 2));
      Section->addr = static_cast<uint32_t>(BaseVA + 0x300);
      Section->size = 0x20;
      Section->flags = S_ZEROFILL;
    }
    auto RegionOrErr = findMachOCompactUnwindRegion(Adjacent);
    ASSERT_TRUE(static_cast<bool>(RegionOrErr))
        << llvm::toString(RegionOrErr.takeError());
    EXPECT_TRUE(RegionOrErr->has_value());
  }

  std::vector<uint8_t> Overflow = makeUnwindInfoMachO(/*Is64=*/true);
  auto *OverflowingSection = reinterpret_cast<section_64 *>(
      Overflow.data() + sectionHeaderOffset(/*Is64=*/true, 2));
  OverflowingSection->addr = std::numeric_limits<uint64_t>::max() - 0x10;
  OverflowingSection->size = 0x20;
  OverflowingSection->flags = S_ZEROFILL;
  expectLocateFailure(findMachOCompactUnwindRegion(Overflow),
                      MachOCompactUnwindLocateFailure::InvalidFileLayout);
}

void writePointer(std::vector<uint8_t> &Bytes, uint64_t Value,
                  uint8_t PointerWidth, llvm::endianness ByteOrder) {
  const size_t Offset = Bytes.size();
  Bytes.resize(Offset + PointerWidth);
  if (PointerWidth == 8)
    llvm::support::endian::write<uint64_t>(Bytes.data() + Offset, Value,
                                           ByteOrder);
  else
    llvm::support::endian::write<uint32_t>(
        Bytes.data() + Offset, static_cast<uint32_t>(Value), ByteOrder);
}

void writeU32(std::vector<uint8_t> &Bytes, uint32_t Value,
              llvm::endianness ByteOrder) {
  const size_t Offset = Bytes.size();
  Bytes.resize(Offset + sizeof(Value));
  llvm::support::endian::write<uint32_t>(Bytes.data() + Offset, Value,
                                         ByteOrder);
}

void overwritePointer(std::vector<uint8_t> &Bytes, size_t Offset,
                      uint64_t Value, uint8_t PointerWidth,
                      llvm::endianness ByteOrder) {
  if (PointerWidth == 8)
    llvm::support::endian::write<uint64_t>(Bytes.data() + Offset, Value,
                                           ByteOrder);
  else
    llvm::support::endian::write<uint32_t>(
        Bytes.data() + Offset, static_cast<uint32_t>(Value), ByteOrder);
}

void writeU16At(std::vector<uint8_t> &Bytes, size_t Offset, uint16_t Value) {
  ASSERT_LE(Offset + sizeof(Value), Bytes.size());
  llvm::support::endian::write16le(Bytes.data() + Offset, Value);
}

void writeU32At(std::vector<uint8_t> &Bytes, size_t Offset, uint32_t Value) {
  ASSERT_LE(Offset + sizeof(Value), Bytes.size());
  llvm::support::endian::write32le(Bytes.data() + Offset, Value);
}

struct RawRecipeSpec {
  uint32_t FunctionRVA = 0;
  uint32_t Encoding = 0;
  std::optional<uint32_t> LSDARVA;
};

macho_unwind::CompactUnwindRawSection
makeRawOriginal(const std::vector<RawRecipeSpec> &Recipes,
                uint32_t TerminalFunctionRVA,
                const std::vector<uint32_t> &PersonalitySlotRVAs = {}) {
  EXPECT_FALSE(Recipes.empty());
  EXPECT_LE(PersonalitySlotRVAs.size(), 3u);

  constexpr uint32_t HeaderSize = 28;
  const uint32_t PersonalityOffset = HeaderSize;
  const uint32_t IndexOffset =
      PersonalityOffset + PersonalitySlotRVAs.size() * sizeof(uint32_t);
  const uint32_t LSDAOffset = IndexOffset + 2 * 12;
  const uint32_t LSDACount = static_cast<uint32_t>(std::count_if(
      Recipes.begin(), Recipes.end(),
      [](const RawRecipeSpec &Recipe) { return Recipe.LSDARVA.has_value(); }));
  const uint32_t PageOffset = LSDAOffset + LSDACount * 8;
  const uint32_t PageSize = 8 + Recipes.size() * 8;
  std::vector<uint8_t> Bytes(PageOffset + PageSize, 0);

  writeU32At(Bytes, 0, macho_unwind::kUnwindSectionVersion);
  writeU32At(Bytes, 4, HeaderSize);
  writeU32At(Bytes, 8, 0);
  writeU32At(Bytes, 12, PersonalityOffset);
  writeU32At(Bytes, 16, static_cast<uint32_t>(PersonalitySlotRVAs.size()));
  writeU32At(Bytes, 20, IndexOffset);
  writeU32At(Bytes, 24, 2);
  for (size_t I = 0; I < PersonalitySlotRVAs.size(); ++I)
    writeU32At(Bytes, PersonalityOffset + I * 4, PersonalitySlotRVAs[I]);

  writeU32At(Bytes, IndexOffset, Recipes.front().FunctionRVA);
  writeU32At(Bytes, IndexOffset + 4, PageOffset);
  writeU32At(Bytes, IndexOffset + 8, LSDAOffset);
  writeU32At(Bytes, IndexOffset + 12, TerminalFunctionRVA);
  writeU32At(Bytes, IndexOffset + 16, 0);
  writeU32At(Bytes, IndexOffset + 20, LSDAOffset + LSDACount * 8);

  uint32_t LSDAIndex = 0;
  for (const RawRecipeSpec &Recipe : Recipes) {
    if (!Recipe.LSDARVA)
      continue;
    writeU32At(Bytes, LSDAOffset + LSDAIndex * 8, Recipe.FunctionRVA);
    writeU32At(Bytes, LSDAOffset + LSDAIndex * 8 + 4, *Recipe.LSDARVA);
    ++LSDAIndex;
  }

  writeU32At(Bytes, PageOffset, macho_unwind::kSecondLevelRegular);
  writeU16At(Bytes, PageOffset + 4, 8);
  writeU16At(Bytes, PageOffset + 6, static_cast<uint16_t>(Recipes.size()));
  for (size_t I = 0; I < Recipes.size(); ++I) {
    writeU32At(Bytes, PageOffset + 8 + I * 8, Recipes[I].FunctionRVA);
    writeU32At(Bytes, PageOffset + 12 + I * 8, Recipes[I].Encoding);
  }

  auto Parsed = macho_unwind::parseCompactUnwindRaw(Bytes);
  if (!Parsed) {
    ADD_FAILURE() << llvm::toString(Parsed.takeError());
    return {};
  }
  return std::move(*Parsed);
}

std::vector<uint8_t> makeInstallableUnwindInfoMachO(
    const macho_unwind::CompactUnwindRawSection &Original, size_t Capacity,
    uint8_t TailFill = 0xa5) {
  using namespace llvm::MachO;
  constexpr uint32_t TextOff = 0x200;
  constexpr uint32_t TextSize = 0x40;
  constexpr uint32_t UnwindOff = 0x400;
  constexpr uint32_t SectionCount = 2;
  const uint32_t CommandSize =
      sizeof(segment_command_64) + SectionCount * sizeof(section_64);
  if (Capacity < Original.OriginalBytes.size() ||
      Capacity > std::numeric_limits<uint32_t>::max() - UnwindOff) {
    ADD_FAILURE() << "invalid compact-unwind fixture capacity";
    return {};
  }
  const uint32_t FileSize = UnwindOff + static_cast<uint32_t>(Capacity);
  std::vector<uint8_t> Binary(FileSize, 0);

  auto *Header = reinterpret_cast<mach_header_64 *>(Binary.data());
  Header->magic = MH_MAGIC_64;
  Header->cputype = CPU_TYPE_ARM64;
  Header->cpusubtype = CPU_SUBTYPE_ARM64_ALL;
  Header->filetype = MH_EXECUTE;
  Header->ncmds = 1;
  Header->sizeofcmds = CommandSize;

  auto *Segment = reinterpret_cast<segment_command_64 *>(
      Binary.data() + sizeof(mach_header_64));
  Segment->cmd = LC_SEGMENT_64;
  Segment->cmdsize = CommandSize;
  setMachOName(Segment->segname, section_names::macho::TextSeg);
  Segment->vmaddr = kImageBase;
  Segment->vmsize = FileSize;
  Segment->fileoff = 0;
  Segment->filesize = FileSize;
  Segment->maxprot = VM_PROT_READ | VM_PROT_EXECUTE;
  Segment->initprot = VM_PROT_READ | VM_PROT_EXECUTE;
  Segment->nsects = SectionCount;

  auto *Sections = reinterpret_cast<section_64 *>(Segment + 1);
  setMachOName(Sections[0].sectname, section_names::macho::Text);
  setMachOName(Sections[0].segname, section_names::macho::TextSeg);
  Sections[0].addr = kImageBase + TextOff;
  Sections[0].size = TextSize;
  Sections[0].offset = TextOff;
  Sections[0].align = 2;
  Sections[0].flags = S_REGULAR;

  setMachOName(Sections[1].sectname, section_names::macho::Unwind);
  setMachOName(Sections[1].segname, section_names::macho::TextSeg);
  Sections[1].addr = kImageBase + UnwindOff;
  Sections[1].size = Capacity;
  Sections[1].offset = UnwindOff;
  Sections[1].align = 2;
  Sections[1].flags = S_REGULAR;

  std::fill(Binary.begin() + UnwindOff, Binary.end(), TailFill);
  std::copy(Original.OriginalBytes.begin(), Original.OriginalBytes.end(),
            Binary.begin() + UnwindOff);
  return Binary;
}

MachOCompactUnwindRecords
makeGeneratedRecords(const std::vector<MachOCompactUnwindRecord> &Records,
                     Arch TargetArch = Arch::AArch64) {
  MachOCompactUnwindRecords Result;
  Result.TargetArch = TargetArch;
  Result.PointerWidth =
      TargetArch == Arch::X86 || TargetArch == Arch::ARM ? 4 : 8;
  Result.ByteOrder = llvm::endianness::little;
  Result.Records = Records;
  return Result;
}

MachOCompactUnwindRecord
makeGeneratedRecord(uint64_t FunctionVA, uint64_t FunctionEndVA,
                    uint32_t Encoding,
                    std::optional<uint32_t> PersonalitySlotRVA = std::nullopt,
                    std::optional<uint64_t> LSDAVA = std::nullopt,
                    uint64_t SourceRecordIndex = 0) {
  MachOCompactUnwindRecord Result;
  Result.FunctionRangeId = SourceRecordIndex + 1;
  Result.OwnerSymbol = "_generated_" + std::to_string(SourceRecordIndex);
  Result.OwnerVA = FunctionVA;
  Result.FunctionVA = FunctionVA;
  Result.FunctionEndVA = FunctionEndVA;
  Result.RangeLength = static_cast<uint32_t>(FunctionEndVA - FunctionVA);
  Result.Encoding = Encoding;
  Result.FunctionSymbol = "L_begin_" + std::to_string(SourceRecordIndex);
  if (PersonalitySlotRVA) {
    Result.PersonalitySymbol =
        "_personality_" + std::to_string(SourceRecordIndex);
    Result.PersonalitySlotRVA = *PersonalitySlotRVA;
  }
  if (LSDAVA) {
    Result.Encoding |= macho_unwind::kHasLSDA;
    Result.LSDASymbol = "L_lsda_" + std::to_string(SourceRecordIndex);
    Result.LSDAVA = *LSDAVA;
  }
  Result.SourceRecordIndex = SourceRecordIndex;
  return Result;
}

std::vector<uint8_t> makeEHFrameInstallMachO(Arch TargetArch = Arch::AArch64) {
  using namespace llvm::MachO;
  constexpr uint32_t EHFrameOff = 0x300;
  constexpr uint32_t EHFrameSize = 0x10;
  constexpr uint32_t TextOff = 0x600;
  constexpr uint32_t TextSize = 0x40;
  constexpr uint32_t FileSize = 0x800;
  if (TargetArch != Arch::AArch64 && TargetArch != Arch::ARM) {
    ADD_FAILURE() << "unsupported EH-frame fixture architecture";
    return {};
  }
  const bool Is64 = TargetArch == Arch::AArch64;
  const uint64_t BaseVA = Is64 ? kImageBase : kMachO32Base;
  const uint32_t CommandSize =
      (Is64 ? sizeof(segment_command_64) : sizeof(segment_command)) +
      2 * (Is64 ? sizeof(section_64) : sizeof(section));

  std::vector<uint8_t> Binary(FileSize, 0);
  if (Is64) {
    auto *Header = reinterpret_cast<mach_header_64 *>(Binary.data());
    Header->magic = MH_MAGIC_64;
    Header->cputype = CPU_TYPE_ARM64;
    Header->cpusubtype = CPU_SUBTYPE_ARM64_ALL;
    Header->filetype = MH_EXECUTE;
    Header->ncmds = 1;
    Header->sizeofcmds = CommandSize;

    auto *Segment = reinterpret_cast<segment_command_64 *>(
        Binary.data() + sizeof(mach_header_64));
    Segment->cmd = LC_SEGMENT_64;
    Segment->cmdsize = CommandSize;
    setMachOName(Segment->segname, section_names::macho::TextSeg);
    Segment->vmaddr = BaseVA;
    Segment->vmsize = FileSize;
    Segment->fileoff = 0;
    Segment->filesize = FileSize;
    Segment->maxprot = VM_PROT_READ | VM_PROT_EXECUTE;
    Segment->initprot = VM_PROT_READ | VM_PROT_EXECUTE;
    Segment->nsects = 2;

    auto *Sections = reinterpret_cast<section_64 *>(Segment + 1);
    setMachOName(Sections[0].sectname, section_names::macho::EhFrame);
    setMachOName(Sections[0].segname, section_names::macho::TextSeg);
    Sections[0].addr = BaseVA + EHFrameOff;
    Sections[0].size = EHFrameSize;
    Sections[0].offset = EHFrameOff;
    Sections[0].align = 2;
    Sections[0].flags = S_REGULAR;

    setMachOName(Sections[1].sectname, section_names::macho::Text);
    setMachOName(Sections[1].segname, section_names::macho::TextSeg);
    Sections[1].addr = BaseVA + TextOff;
    Sections[1].size = TextSize;
    Sections[1].offset = TextOff;
    Sections[1].align = 2;
    Sections[1].flags = S_REGULAR;
  } else {
    auto *Header = reinterpret_cast<mach_header *>(Binary.data());
    Header->magic = MH_MAGIC;
    Header->cputype = CPU_TYPE_ARM;
    Header->cpusubtype = CPU_SUBTYPE_ARM_ALL;
    Header->filetype = MH_EXECUTE;
    Header->ncmds = 1;
    Header->sizeofcmds = CommandSize;

    auto *Segment = reinterpret_cast<segment_command *>(Binary.data() +
                                                        sizeof(mach_header));
    Segment->cmd = LC_SEGMENT;
    Segment->cmdsize = CommandSize;
    setMachOName(Segment->segname, section_names::macho::TextSeg);
    Segment->vmaddr = static_cast<uint32_t>(BaseVA);
    Segment->vmsize = FileSize;
    Segment->fileoff = 0;
    Segment->filesize = FileSize;
    Segment->maxprot = VM_PROT_READ | VM_PROT_EXECUTE;
    Segment->initprot = VM_PROT_READ | VM_PROT_EXECUTE;
    Segment->nsects = 2;

    auto *Sections = reinterpret_cast<section *>(Segment + 1);
    setMachOName(Sections[0].sectname, section_names::macho::EhFrame);
    setMachOName(Sections[0].segname, section_names::macho::TextSeg);
    Sections[0].addr = static_cast<uint32_t>(BaseVA + EHFrameOff);
    Sections[0].size = EHFrameSize;
    Sections[0].offset = EHFrameOff;
    Sections[0].align = 2;
    Sections[0].flags = S_REGULAR;

    setMachOName(Sections[1].sectname, section_names::macho::Text);
    setMachOName(Sections[1].segname, section_names::macho::TextSeg);
    Sections[1].addr = static_cast<uint32_t>(BaseVA + TextOff);
    Sections[1].size = TextSize;
    Sections[1].offset = TextOff;
    Sections[1].align = 2;
    Sections[1].flags = S_REGULAR;
  }
  return Binary;
}

std::vector<uint8_t> makeEHFrameFragment(uint64_t FunctionVA,
                                         uint64_t FunctionEndVA,
                                         Arch TargetArch = Arch::AArch64) {
  if (TargetArch != Arch::AArch64 && TargetArch != Arch::ARM) {
    ADD_FAILURE() << "unsupported EH-frame fixture architecture";
    return {};
  }
  const uint8_t PointerWidth = TargetArch == Arch::ARM ? 4 : 8;
  const uint8_t DataAlignment = TargetArch == Arch::ARM ? 0x7c : 0x78;
  const uint8_t ReturnAddressRegister = TargetArch == Arch::ARM ? 14 : 16;
  std::vector<uint8_t> Bytes;
  const std::vector<uint8_t> CIE = {
      0, 0, 0, 0, 1, 'z', 'R', 0, 1, DataAlignment, ReturnAddressRegister,
      1, 0};
  writeU32(Bytes, static_cast<uint32_t>(CIE.size()), llvm::endianness::little);
  Bytes.insert(Bytes.end(), CIE.begin(), CIE.end());
  writeU32(Bytes, 5 + 2 * PointerWidth, llvm::endianness::little);
  writeU32(Bytes, static_cast<uint32_t>(Bytes.size()),
           llvm::endianness::little);
  writePointer(Bytes, FunctionVA, PointerWidth, llvm::endianness::little);
  writePointer(Bytes, FunctionEndVA - FunctionVA, PointerWidth,
               llvm::endianness::little);
  Bytes.push_back(0);
  writeU32(Bytes, 0, llvm::endianness::little);
  return Bytes;
}

llvm::Expected<MachOEHFrameInstallReceipt>
makeEHFrameReceiptForArch(Arch TargetArch, uint64_t FunctionVA,
                          uint64_t FunctionEndVA, llvm::StringRef OwnerSymbol,
                          uint64_t FunctionRangeId,
                          std::optional<uint64_t> OwnerSymbolVA = std::nullopt,
                          std::optional<uint64_t> RangeBeginVA = std::nullopt,
                          std::optional<uint64_t> RangeEndVA = std::nullopt) {
  std::vector<uint8_t> Binary = makeEHFrameInstallMachO(TargetArch);
  const auto Region = findMachOEHFrameRegion(Binary);
  if (!Region)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "test fixture has no EH-frame region");

  CompiledSection Section;
  Section.Name = section_names::macho::EhFrame;
  Section.IsAllocated = true;
  Section.IsInImage = false;
  Section.VA = Region->AppendVA;
  Section.ExternalBytes =
      makeEHFrameFragment(FunctionVA, FunctionEndVA, TargetArch);
  Section.Size = Section.ExternalBytes.size();

  CompiledImage Compiled;
  Compiled.Success = true;
  Compiled.TargetArch = TargetArch;
  Compiled.Format = BinaryFormat::MachO;
  Compiled.PointerWidth = TargetArch == Arch::ARM ? 4 : 8;
  Compiled.ByteOrder = llvm::endianness::little;
  Compiled.Sections.push_back(std::move(Section));
  const uint64_t OwnerVA = OwnerSymbolVA.value_or(FunctionVA);
  Compiled.FunctionOwnerAddrs[OwnerSymbol.str()] = OwnerVA;
  Compiled.FunctionRanges.push_back(
      {FunctionRangeId, OwnerSymbol.str(), OwnerVA,
       "L_begin_" + std::to_string(FunctionRangeId - 1),
       RangeBeginVA.value_or(FunctionVA),
       "L_end_" + std::to_string(FunctionRangeId - 1),
       RangeEndVA.value_or(FunctionEndVA)});

  llvm::LLVMContext Context;
  llvm::Module Module("receipt", Context);
  return installMachOEHFrameWithReceipt(Binary, Region, Compiled, Module);
}

llvm::Expected<MachOEHFrameInstallReceipt>
makeEHFrameReceipt(uint64_t FunctionVA, uint64_t FunctionEndVA,
                   llvm::StringRef OwnerSymbol, uint64_t FunctionRangeId,
                   std::optional<uint64_t> OwnerSymbolVA = std::nullopt,
                   std::optional<uint64_t> RangeBeginVA = std::nullopt,
                   std::optional<uint64_t> RangeEndVA = std::nullopt) {
  return makeEHFrameReceiptForArch(Arch::AArch64, FunctionVA, FunctionEndVA,
                                   OwnerSymbol, FunctionRangeId, OwnerSymbolVA,
                                   RangeBeginVA, RangeEndVA);
}

llvm::Expected<MachOEHFrameInstallReceipt> makeNoOpEHFrameReceipt() {
  std::vector<uint8_t> Binary = makeEHFrameInstallMachO();
  const auto Region = findMachOEHFrameRegion(Binary);
  if (!Region)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "test fixture has no EH-frame region");
  CompiledImage Compiled;
  Compiled.Success = true;
  Compiled.TargetArch = Arch::AArch64;
  Compiled.Format = BinaryFormat::MachO;
  Compiled.PointerWidth = 8;
  Compiled.ByteOrder = llvm::endianness::little;
  llvm::LLVMContext Context;
  llvm::Module Module("no-op-receipt", Context);
  return installMachOEHFrameWithReceipt(Binary, Region, Compiled, Module);
}

MachOCompactUnwindRangeMapping
makeMapping(uint32_t SourceRVA, uint32_t SourceEndRVA, uint32_t DestinationRVA,
            uint32_t DestinationEndRVA, MachOCompactUnwindRangeMode Mode,
            uint64_t FunctionRangeId = 1,
            llvm::StringRef OwnerSymbol = "_generated_0",
            std::optional<uint64_t> OwnerVA = std::nullopt) {
  MachOCompactUnwindRangeMapping Result;
  Result.SourceVA = kImageBase + SourceRVA;
  Result.SourceEndVA = kImageBase + SourceEndRVA;
  Result.DestinationVA = kImageBase + DestinationRVA;
  Result.DestinationEndVA = kImageBase + DestinationEndRVA;
  Result.Mode = Mode;
  Result.FunctionRangeId = FunctionRangeId;
  Result.OwnerSymbol = OwnerSymbol.str();
  Result.OwnerVA = OwnerVA.value_or(Result.DestinationVA);
  return Result;
}

struct CompactUnwindInstallInputs {
  macho_unwind::CompactUnwindRawSection Original;
  MachOCompactUnwindRecords Generated;
  std::vector<MachOCompactUnwindRangeMapping> Mappings;
  MachOCompactUnwindMergeResult Merged;
  std::vector<uint8_t> Encoded;
};

CompactUnwindInstallInputs makeCompactUnwindInstallInputs() {
  CompactUnwindInstallInputs Result;
  Result.Original = makeRawOriginal(
      {{0x100, macho_unwind::kARM64ModeFrame, std::nullopt}}, 0x140);
  Result.Generated = makeGeneratedRecords(
      {makeGeneratedRecord(kImageBase + 0x500, kImageBase + 0x520,
                           macho_unwind::kARM64ModeFrameless)});
  Result.Mappings.push_back(makeMapping(
      0x100, 0x140, 0x500, 0x520, MachOCompactUnwindRangeMode::NewSegment));
  auto Merged =
      mergeMachOCompactUnwind(Arch::AArch64, kImageBase, Result.Original,
                              Result.Generated, Result.Mappings);
  if (!Merged) {
    ADD_FAILURE() << llvm::toString(Merged.takeError());
    return Result;
  }
  Result.Merged = std::move(*Merged);
  auto Encoded =
      encodeMachOCompactUnwindRegular(Result.Merged, llvm::endianness::little);
  if (!Encoded) {
    ADD_FAILURE() << llvm::toString(Encoded.takeError());
    return Result;
  }
  Result.Encoded = std::move(*Encoded);
  return Result;
}

constexpr uint32_t kTransactionTextOff = 0x1000;
constexpr uint32_t kTransactionTextSize = 0x40;
constexpr uint32_t kTransactionEHFrameOff = 0x2000;
constexpr uint32_t kTransactionEHFrameSize = 4;
constexpr uint32_t kTransactionUnwindOff = 0x3000;
constexpr uint32_t kTransactionTextSegmentSize = 0x8000;
constexpr uint32_t kTransactionDataOff = 0x8000;
constexpr uint32_t kTransactionGOTOff = 0x9000;
constexpr uint32_t kTransactionDataSegmentSize = 0x4000;
constexpr uint32_t kTransactionLinkeditOff = 0xc000;
constexpr uint32_t kTransactionLinkeditSize = 0x100;
constexpr uint32_t kTransactionSymtabOff = kTransactionLinkeditOff;
constexpr uint32_t kTransactionStringTableOff = kTransactionLinkeditOff + 0x40;
constexpr uint32_t kTransactionIndirectTableOff =
    kTransactionLinkeditOff + 0x80;
constexpr uint64_t kTransactionFunctionVA = kImageBase + kTransactionTextOff;
constexpr uint64_t kTransactionPersonalityVA = kTransactionFunctionVA + 0x10;
constexpr uint64_t kTransactionMayThrowVA = kTransactionFunctionVA + 0x20;
constexpr uint64_t kTransactionUnwindResumeVA = kTransactionFunctionVA + 0x30;
constexpr uint64_t kTransactionPersonalitySlotVA =
    kImageBase + kTransactionGOTOff;

struct MachOPatchTransactionFixture {
  std::vector<uint8_t> Binary;
  BinaryImage Image;
  macho_unwind::CompactUnwindRawSection OriginalCompact;
};

MachOPatchTransactionFixture
makeMachOPatchTransactionFixture(size_t CompactCapacity) {
  using namespace llvm::MachO;

  MachOPatchTransactionFixture Fixture;
  Fixture.OriginalCompact = makeRawOriginal(
      {{kTransactionTextOff, macho_unwind::kARM64ModeFrame, std::nullopt}},
      kTransactionTextOff + kTransactionTextSize);
  if (CompactCapacity < Fixture.OriginalCompact.OriginalBytes.size() ||
      CompactCapacity > kTransactionTextSegmentSize - kTransactionUnwindOff) {
    ADD_FAILURE() << "invalid top-level patch fixture compact capacity";
    return Fixture;
  }

  constexpr uint32_t TextSectionCount = 3;
  constexpr uint32_t DataSectionCount = 1;
  constexpr uint32_t TextCommandSize =
      sizeof(segment_command_64) + TextSectionCount * sizeof(section_64);
  constexpr uint32_t DataCommandSize =
      sizeof(segment_command_64) + DataSectionCount * sizeof(section_64);
  constexpr uint32_t LinkeditCommandSize = sizeof(segment_command_64);
  constexpr uint32_t SymtabCommandSize = sizeof(symtab_command);
  constexpr uint32_t DysymtabCommandSize = sizeof(dysymtab_command);
  constexpr uint32_t CommandSize = TextCommandSize + DataCommandSize +
                                   LinkeditCommandSize + SymtabCommandSize +
                                   DysymtabCommandSize;
  Fixture.Binary.assign(kTransactionLinkeditOff + kTransactionLinkeditSize, 0);

  auto *Header = reinterpret_cast<mach_header_64 *>(Fixture.Binary.data());
  Header->magic = MH_MAGIC_64;
  Header->cputype = CPU_TYPE_ARM64;
  Header->cpusubtype = CPU_SUBTYPE_ARM64_ALL;
  Header->filetype = MH_EXECUTE;
  Header->ncmds = 5;
  Header->sizeofcmds = CommandSize;

  uint8_t *Command = Fixture.Binary.data() + sizeof(mach_header_64);
  auto *TextSegment = reinterpret_cast<segment_command_64 *>(Command);
  TextSegment->cmd = LC_SEGMENT_64;
  TextSegment->cmdsize = TextCommandSize;
  setMachOName(TextSegment->segname, section_names::macho::TextSeg);
  TextSegment->vmaddr = kImageBase;
  TextSegment->vmsize = kTransactionTextSegmentSize;
  TextSegment->fileoff = 0;
  TextSegment->filesize = kTransactionTextSegmentSize;
  TextSegment->maxprot = VM_PROT_READ | VM_PROT_EXECUTE;
  TextSegment->initprot = VM_PROT_READ | VM_PROT_EXECUTE;
  TextSegment->nsects = TextSectionCount;

  auto *TextSections = reinterpret_cast<section_64 *>(TextSegment + 1);
  setMachOName(TextSections[0].sectname, section_names::macho::Text);
  setMachOName(TextSections[0].segname, section_names::macho::TextSeg);
  TextSections[0].addr = kTransactionFunctionVA;
  TextSections[0].size = kTransactionTextSize;
  TextSections[0].offset = kTransactionTextOff;
  TextSections[0].align = 2;
  TextSections[0].flags = static_cast<uint32_t>(S_REGULAR) |
                          static_cast<uint32_t>(S_ATTR_PURE_INSTRUCTIONS);

  setMachOName(TextSections[1].sectname, section_names::macho::EhFrame);
  setMachOName(TextSections[1].segname, section_names::macho::TextSeg);
  TextSections[1].addr = kImageBase + kTransactionEHFrameOff;
  TextSections[1].size = kTransactionEHFrameSize;
  TextSections[1].offset = kTransactionEHFrameOff;
  TextSections[1].align = 2;
  TextSections[1].flags = S_REGULAR;

  setMachOName(TextSections[2].sectname, section_names::macho::Unwind);
  setMachOName(TextSections[2].segname, section_names::macho::TextSeg);
  TextSections[2].addr = kImageBase + kTransactionUnwindOff;
  TextSections[2].size = CompactCapacity;
  TextSections[2].offset = kTransactionUnwindOff;
  TextSections[2].align = 2;
  TextSections[2].flags = S_REGULAR;

  Command += TextCommandSize;
  auto *DataSegment = reinterpret_cast<segment_command_64 *>(Command);
  DataSegment->cmd = LC_SEGMENT_64;
  DataSegment->cmdsize = DataCommandSize;
  setMachOName(DataSegment->segname, "__DATA_CONST");
  DataSegment->vmaddr = kImageBase + kTransactionDataOff;
  DataSegment->vmsize = kTransactionDataSegmentSize;
  DataSegment->fileoff = kTransactionDataOff;
  DataSegment->filesize = kTransactionDataSegmentSize;
  DataSegment->maxprot = VM_PROT_READ | VM_PROT_WRITE;
  DataSegment->initprot = VM_PROT_READ | VM_PROT_WRITE;
  DataSegment->nsects = DataSectionCount;

  auto *DataSection = reinterpret_cast<section_64 *>(DataSegment + 1);
  setMachOName(DataSection->sectname, "__got");
  setMachOName(DataSection->segname, "__DATA_CONST");
  DataSection->addr = kTransactionPersonalitySlotVA;
  DataSection->size = 0x10;
  DataSection->offset = kTransactionGOTOff;
  DataSection->align = 3;
  DataSection->flags = S_NON_LAZY_SYMBOL_POINTERS;
  DataSection->reserved1 = 0;

  Command += DataCommandSize;
  auto *LinkeditSegment = reinterpret_cast<segment_command_64 *>(Command);
  LinkeditSegment->cmd = LC_SEGMENT_64;
  LinkeditSegment->cmdsize = LinkeditCommandSize;
  setMachOName(LinkeditSegment->segname, section_names::macho::LinkeditSeg);
  LinkeditSegment->vmaddr = kImageBase + kTransactionLinkeditOff;
  LinkeditSegment->vmsize = 0x4000;
  LinkeditSegment->fileoff = kTransactionLinkeditOff;
  LinkeditSegment->filesize = kTransactionLinkeditSize;
  LinkeditSegment->maxprot = VM_PROT_READ;
  LinkeditSegment->initprot = VM_PROT_READ;

  Command += LinkeditCommandSize;
  auto *Symtab = reinterpret_cast<symtab_command *>(Command);
  Symtab->cmd = LC_SYMTAB;
  Symtab->cmdsize = SymtabCommandSize;
  Symtab->symoff = kTransactionSymtabOff;
  Symtab->nsyms = 3;
  Symtab->stroff = kTransactionStringTableOff;
  constexpr char SymbolStringTable[] =
      "\0_test_personality\0may_throw\0_Unwind_Resume";
  constexpr uint32_t MayThrowStringOffset = 19;
  constexpr uint32_t UnwindResumeStringOffset = 29;
  static_assert(SymbolStringTable[MayThrowStringOffset] == 'm');
  static_assert(SymbolStringTable[UnwindResumeStringOffset] == '_');
  Symtab->strsize = sizeof(SymbolStringTable);

  Command += SymtabCommandSize;
  auto *Dysymtab = reinterpret_cast<dysymtab_command *>(Command);
  Dysymtab->cmd = LC_DYSYMTAB;
  Dysymtab->cmdsize = DysymtabCommandSize;
  Dysymtab->indirectsymoff = kTransactionIndirectTableOff;
  Dysymtab->nindirectsyms = 2;

  for (uint32_t Off = kTransactionTextOff;
       Off < kTransactionTextOff + kTransactionTextSize; Off += 4)
    llvm::support::endian::write32le(Fixture.Binary.data() + Off, 0xd503201f);
  std::fill(Fixture.Binary.begin() + kTransactionUnwindOff,
            Fixture.Binary.begin() + kTransactionUnwindOff + CompactCapacity,
            0xa5);
  std::copy(Fixture.OriginalCompact.OriginalBytes.begin(),
            Fixture.OriginalCompact.OriginalBytes.end(),
            Fixture.Binary.begin() + kTransactionUnwindOff);
  llvm::support::endian::write64le(Fixture.Binary.data() + kTransactionGOTOff,
                                   kTransactionPersonalityVA);
  std::fill(Fixture.Binary.begin() + kTransactionLinkeditOff,
            Fixture.Binary.end(), 0x6c);
  auto *PersonalitySymbol = reinterpret_cast<nlist_64 *>(Fixture.Binary.data() +
                                                         kTransactionSymtabOff);
  PersonalitySymbol->n_strx = 1;
  PersonalitySymbol->n_type =
      static_cast<uint8_t>(static_cast<uint8_t>(N_UNDF) | N_EXT);
  PersonalitySymbol->n_sect = NO_SECT;
  PersonalitySymbol->n_desc = 0;
  PersonalitySymbol->n_value = 0;
  auto *MayThrowSymbol = PersonalitySymbol + 1;
  MayThrowSymbol->n_strx = MayThrowStringOffset;
  MayThrowSymbol->n_type =
      static_cast<uint8_t>(static_cast<uint8_t>(N_SECT) | N_EXT);
  MayThrowSymbol->n_sect = 1;
  MayThrowSymbol->n_desc = 0;
  MayThrowSymbol->n_value = kTransactionMayThrowVA;
  auto *UnwindResumeSymbol = PersonalitySymbol + 2;
  UnwindResumeSymbol->n_strx = UnwindResumeStringOffset;
  UnwindResumeSymbol->n_type =
      static_cast<uint8_t>(static_cast<uint8_t>(N_SECT) | N_EXT);
  UnwindResumeSymbol->n_sect = 1;
  UnwindResumeSymbol->n_desc = 0;
  UnwindResumeSymbol->n_value = kTransactionUnwindResumeVA;
  std::memcpy(Fixture.Binary.data() + kTransactionStringTableOff,
              SymbolStringTable, sizeof(SymbolStringTable));
  llvm::support::endian::write32le(
      Fixture.Binary.data() + kTransactionIndirectTableOff, 0);
  llvm::support::endian::write32le(
      Fixture.Binary.data() + kTransactionIndirectTableOff + sizeof(uint32_t),
      INDIRECT_SYMBOL_LOCAL);

  Fixture.Image.Format = BinaryFormat::MachO;
  Fixture.Image.Arch = Arch::AArch64;
  Fixture.Image.Bits = Bitness::Bits64;
  Fixture.Image.Base = kImageBase;
  Fixture.Image.Raw = Fixture.Binary;

  Segment TextImageSegment;
  TextImageSegment.Name = section_names::macho::TextSeg;
  TextImageSegment.VA = kImageBase;
  TextImageSegment.Size = kTransactionTextSegmentSize;
  TextImageSegment.FileOff = 0;
  TextImageSegment.FileSz = kTransactionTextSegmentSize;
  TextImageSegment.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  TextImageSegment.Data.assign(Fixture.Binary.begin(),
                               Fixture.Binary.begin() +
                                   kTransactionTextSegmentSize);
  Fixture.Image.Segments.push_back(std::move(TextImageSegment));

  Segment DataImageSegment;
  DataImageSegment.Name = "__DATA_CONST";
  DataImageSegment.VA = kImageBase + kTransactionDataOff;
  DataImageSegment.Size = kTransactionDataSegmentSize;
  DataImageSegment.FileOff = kTransactionDataOff;
  DataImageSegment.FileSz = kTransactionDataSegmentSize;
  DataImageSegment.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  DataImageSegment.Data.assign(Fixture.Binary.begin() + kTransactionDataOff,
                               Fixture.Binary.begin() +
                                   kTransactionLinkeditOff);
  Fixture.Image.Segments.push_back(std::move(DataImageSegment));

  Segment LinkeditImageSegment;
  LinkeditImageSegment.Name = section_names::macho::LinkeditSeg;
  LinkeditImageSegment.VA = kImageBase + kTransactionLinkeditOff;
  LinkeditImageSegment.Size = 0x4000;
  LinkeditImageSegment.FileOff = kTransactionLinkeditOff;
  LinkeditImageSegment.FileSz = kTransactionLinkeditSize;
  LinkeditImageSegment.Flags = SegmentFlags::Readable;
  LinkeditImageSegment.Data.assign(
      Fixture.Binary.begin() + kTransactionLinkeditOff, Fixture.Binary.end());
  Fixture.Image.Segments.push_back(std::move(LinkeditImageSegment));

  auto AddSection = [&](llvm::StringRef Name, llvm::StringRef SegmentName,
                        uint64_t VA, uint64_t Size, uint64_t FileOff,
                        SegmentFlags Flags) {
    Section ImageSection;
    ImageSection.Name = Name.str();
    ImageSection.SegmentName = SegmentName.str();
    ImageSection.VA = VA;
    ImageSection.Size = Size;
    ImageSection.FileOff = FileOff;
    ImageSection.FileSz = Size;
    ImageSection.Flags = Flags;
    ImageSection.Data.assign(Fixture.Binary.begin() + FileOff,
                             Fixture.Binary.begin() + FileOff + Size);
    Fixture.Image.Sections.push_back(std::move(ImageSection));
  };
  AddSection(section_names::macho::Text, section_names::macho::TextSeg,
             kTransactionFunctionVA, kTransactionTextSize, kTransactionTextOff,
             SegmentFlags::Readable | SegmentFlags::Executable);
  AddSection(section_names::macho::EhFrame, section_names::macho::TextSeg,
             kImageBase + kTransactionEHFrameOff, kTransactionEHFrameSize,
             kTransactionEHFrameOff, SegmentFlags::Readable);
  AddSection(section_names::macho::Unwind, section_names::macho::TextSeg,
             kImageBase + kTransactionUnwindOff, CompactCapacity,
             kTransactionUnwindOff, SegmentFlags::Readable);
  AddSection("__got", "__DATA_CONST", kTransactionPersonalitySlotVA, 0x10,
             kTransactionGOTOff,
             SegmentFlags::Readable | SegmentFlags::Writable);

  Fixture.Image.Symbols.push_back(
      Symbol::makeFunc(kTransactionFunctionVA, kTransactionTextSize));
  Fixture.Image.KnownCodeRanges.push_back(
      {kTransactionFunctionVA, kTransactionFunctionVA + kTransactionTextSize});
  Fixture.Image.Exports.push_back(
      {"test_personality", 0, kTransactionPersonalityVA});
  Fixture.Image.Exports.push_back({"may_throw", 0, kTransactionMayThrowVA});
  Fixture.Image.Exports.push_back(
      {"_Unwind_Resume", 0, kTransactionUnwindResumeVA});
  Fixture.Image.ImportPtrSlots[kTransactionPersonalitySlotVA] =
      "_test_personality";
  return Fixture;
}

llvm::MachO::dysymtab_command *
findMutableDysymtabCommand(std::vector<uint8_t> &Binary) {
  llvm::MachO::dysymtab_command *Result = nullptr;
  forEachMachOLoadCommand(
      Binary.data(), Binary.size(),
      [&](const uint8_t *Command, uint32_t ID, uint32_t CommandSize, bool) {
        if (ID == llvm::MachO::LC_DYSYMTAB &&
            CommandSize >= sizeof(llvm::MachO::dysymtab_command))
          Result = reinterpret_cast<llvm::MachO::dysymtab_command *>(
              const_cast<uint8_t *>(Command));
      });
  return Result;
}

TEST(MachOImportPtrSlots, ReconstructsExactRawSlotSymbolIdentity) {
  MachOPatchTransactionFixture Fixture =
      makeMachOPatchTransactionFixture(/*CompactCapacity=*/0x2000);
  ASSERT_FALSE(Fixture.Binary.empty());

  auto Slots = macho_loader::parseImportPtrSlots(Fixture.Binary);
  ASSERT_TRUE(static_cast<bool>(Slots)) << llvm::toString(Slots.takeError());
  ASSERT_EQ(Slots->size(), 1u);
  EXPECT_EQ(Slots->at(kTransactionPersonalitySlotVA), "_test_personality");
}

TEST(MachOImportPtrSlots, AcceptsSymtabWithoutPointerSectionsOrDysymtab) {
  using namespace llvm::MachO;

  std::vector<uint8_t> Binary(sizeof(mach_header_64) + sizeof(symtab_command),
                              0);
  auto *Header = reinterpret_cast<mach_header_64 *>(Binary.data());
  Header->magic = MH_MAGIC_64;
  Header->cputype = CPU_TYPE_ARM64;
  Header->cpusubtype = CPU_SUBTYPE_ARM64_ALL;
  Header->filetype = MH_OBJECT;
  Header->ncmds = 1;
  Header->sizeofcmds = sizeof(symtab_command);

  auto *Symtab = reinterpret_cast<symtab_command *>(Binary.data() +
                                                    sizeof(mach_header_64));
  Symtab->cmd = LC_SYMTAB;
  Symtab->cmdsize = sizeof(symtab_command);
  Symtab->symoff = Binary.size();
  Symtab->stroff = Binary.size();

  auto Slots = macho_loader::parseImportPtrSlots(Binary);
  ASSERT_TRUE(static_cast<bool>(Slots)) << llvm::toString(Slots.takeError());
  EXPECT_TRUE(Slots->empty());
}

TEST(MachOImportPtrSlots, RejectsTruncatedIndirectSymbolTable) {
  MachOPatchTransactionFixture Fixture =
      makeMachOPatchTransactionFixture(/*CompactCapacity=*/0x2000);
  ASSERT_FALSE(Fixture.Binary.empty());
  auto *Dysymtab = findMutableDysymtabCommand(Fixture.Binary);
  ASSERT_NE(Dysymtab, nullptr);
  Dysymtab->indirectsymoff = Fixture.Binary.size() - sizeof(uint16_t);

  auto Slots = macho_loader::parseImportPtrSlots(Fixture.Binary);
  EXPECT_FALSE(static_cast<bool>(Slots));
  if (!Slots)
    llvm::consumeError(Slots.takeError());
}

TEST(MachOImportPtrSlots, RejectsPointerSectionOutsideIndirectSymbolCount) {
  MachOPatchTransactionFixture Fixture =
      makeMachOPatchTransactionFixture(/*CompactCapacity=*/0x2000);
  ASSERT_FALSE(Fixture.Binary.empty());
  auto *Dysymtab = findMutableDysymtabCommand(Fixture.Binary);
  ASSERT_NE(Dysymtab, nullptr);
  Dysymtab->nindirectsyms = 1;

  auto Slots = macho_loader::parseImportPtrSlots(Fixture.Binary);
  EXPECT_FALSE(static_cast<bool>(Slots));
  if (!Slots)
    llvm::consumeError(Slots.takeError());
}

void removeFinalCompactUnwindSection(MachOPatchTransactionFixture &Fixture) {
  auto *TextSegment = reinterpret_cast<llvm::MachO::segment_command_64 *>(
      Fixture.Binary.data() + sizeof(llvm::MachO::mach_header_64));
  auto *TextSections =
      reinterpret_cast<llvm::MachO::section_64 *>(TextSegment + 1);
  setMachOName(TextSections[2].sectname, "__metadata");
  Fixture.Image.Raw = Fixture.Binary;
  ASSERT_GE(Fixture.Image.Segments.size(), 1u);
  Fixture.Image.Segments[0].Data.assign(Fixture.Binary.begin(),
                                        Fixture.Binary.begin() +
                                            kTransactionTextSegmentSize);
  ASSERT_GE(Fixture.Image.Sections.size(), 3u);
  Fixture.Image.Sections[2].Name = "__metadata";
}

TEST(MachOPatchSegmentInstall,
     RejectsDuplicateLinkeditSegmentsWithoutMutation) {
  MachOPatchTransactionFixture Fixture =
      makeMachOPatchTransactionFixture(/*CompactCapacity=*/0x2000);
  ASSERT_FALSE(Fixture.Binary.empty());
  auto *DataSegment = reinterpret_cast<llvm::MachO::segment_command_64 *>(
      Fixture.Binary.data() + sizeof(llvm::MachO::mach_header_64) +
      reinterpret_cast<const llvm::MachO::segment_command_64 *>(
          Fixture.Binary.data() + sizeof(llvm::MachO::mach_header_64))
          ->cmdsize);
  setMachOName(DataSegment->segname, section_names::macho::LinkeditSeg);
  const std::vector<uint8_t> Before = Fixture.Binary;

  MachOPatcher Patcher;
  EXPECT_EQ(Patcher.plannedExecSegmentVA(Fixture.Binary, Arch::AArch64), 0u);
  EXPECT_EQ(Patcher.appendExecSegment(Fixture.Binary, {0xd6, 0x5f, 0x03, 0xc0},
                                      kDefaultNdTextSegment, Arch::AArch64),
            0u);
  EXPECT_EQ(Fixture.Binary, Before);
}

TEST(MachOPatchSegmentInstall,
     RejectsNonterminalLinkeditSegmentWithoutMutation) {
  MachOPatchTransactionFixture Fixture =
      makeMachOPatchTransactionFixture(/*CompactCapacity=*/0x2000);
  ASSERT_FALSE(Fixture.Binary.empty());
  auto *TextSegment = reinterpret_cast<llvm::MachO::segment_command_64 *>(
      Fixture.Binary.data() + sizeof(llvm::MachO::mach_header_64));
  auto *DataSegment = reinterpret_cast<llvm::MachO::segment_command_64 *>(
      reinterpret_cast<uint8_t *>(TextSegment) + TextSegment->cmdsize);
  auto *LinkeditSegment = reinterpret_cast<llvm::MachO::segment_command_64 *>(
      reinterpret_cast<uint8_t *>(DataSegment) + DataSegment->cmdsize);
  setMachOName(DataSegment->segname, section_names::macho::LinkeditSeg);
  setMachOName(LinkeditSegment->segname, "__AFTER");
  const std::vector<uint8_t> Before = Fixture.Binary;

  MachOPatcher Patcher;
  EXPECT_EQ(Patcher.plannedExecSegmentVA(Fixture.Binary, Arch::AArch64), 0u);
  EXPECT_EQ(Patcher.appendExecSegment(Fixture.Binary, {0xd6, 0x5f, 0x03, 0xc0},
                                      kDefaultNdTextSegment, Arch::AArch64),
            0u);
  EXPECT_EQ(Fixture.Binary, Before);
}

TEST(MachOPatchSegmentInstall,
     RejectsHeaderTargetArchitectureMismatchWithoutMutation) {
  MachOPatchTransactionFixture Fixture =
      makeMachOPatchTransactionFixture(/*CompactCapacity=*/0x2000);
  ASSERT_FALSE(Fixture.Binary.empty());
  const std::vector<uint8_t> Before = Fixture.Binary;

  MachOPatcher Patcher;
  EXPECT_EQ(Patcher.plannedExecSegmentVA(Fixture.Binary, Arch::X64), 0u);
  EXPECT_EQ(Patcher.appendExecSegment(Fixture.Binary, {0xc3},
                                      kDefaultNdTextSegment, Arch::X64),
            0u);
  EXPECT_EQ(Fixture.Binary, Before);
}

TEST(MachOPatchSegmentInstall,
     RejectsSectionParentSegmentNameMismatchWithoutMutation) {
  MachOPatchTransactionFixture Fixture =
      makeMachOPatchTransactionFixture(/*CompactCapacity=*/0x2000);
  ASSERT_FALSE(Fixture.Binary.empty());
  auto *TextSegment = reinterpret_cast<llvm::MachO::segment_command_64 *>(
      Fixture.Binary.data() + sizeof(llvm::MachO::mach_header_64));
  ASSERT_GT(TextSegment->nsects, 0u);
  auto *TextSections =
      reinterpret_cast<llvm::MachO::section_64 *>(TextSegment + 1);
  setMachOName(TextSections[0].segname, "__FORGED");
  const std::vector<uint8_t> Before = Fixture.Binary;

  MachOPatcher Patcher;
  EXPECT_EQ(Patcher.plannedExecSegmentVA(Fixture.Binary, Arch::AArch64), 0u);
  EXPECT_EQ(Patcher.appendExecSegment(Fixture.Binary, {0xd6, 0x5f, 0x03, 0xc0},
                                      kDefaultNdTextSegment, Arch::AArch64),
            0u);
  EXPECT_EQ(Fixture.Binary, Before);
}

TEST(MachOPatchSegmentInstall, HeaderSpaceFailureRollsBackTheWholeCandidate) {
  MachOPatchTransactionFixture Fixture =
      makeMachOPatchTransactionFixture(/*CompactCapacity=*/0x2000);
  ASSERT_FALSE(Fixture.Binary.empty());
  auto *Header =
      reinterpret_cast<llvm::MachO::mach_header_64 *>(Fixture.Binary.data());
  auto *TextSegment = reinterpret_cast<llvm::MachO::segment_command_64 *>(
      Fixture.Binary.data() + sizeof(*Header));
  auto *TextSection =
      reinterpret_cast<llvm::MachO::section_64 *>(TextSegment + 1);
  const uint32_t LoadCommandsEnd = sizeof(*Header) + Header->sizeofcmds;
  TextSection->offset = LoadCommandsEnd;
  TextSection->addr = kImageBase + LoadCommandsEnd;
  const std::vector<uint8_t> Before = Fixture.Binary;

  MachOPatcher Patcher;
  ASSERT_EQ(Patcher.plannedExecSegmentVA(Fixture.Binary, Arch::AArch64),
            kImageBase + kTransactionLinkeditOff);
  EXPECT_EQ(Patcher.appendExecSegment(Fixture.Binary, {0xd6, 0x5f, 0x03, 0xc0},
                                      kDefaultNdTextSegment, Arch::AArch64),
            0u);
  EXPECT_EQ(Fixture.Binary, Before);
}

TEST(MachOPatchSegmentInstall,
     RejectsTruncatedOrConflictingSegmentNamesWithoutMutation) {
  const std::array<llvm::StringRef, 2> InvalidNames = {
      "__TEXT", "__SEGMENT_NAME_IS_TOO_LONG"};
  for (llvm::StringRef Name : InvalidNames) {
    SCOPED_TRACE(Name.str());
    MachOPatchTransactionFixture Fixture =
        makeMachOPatchTransactionFixture(/*CompactCapacity=*/0x2000);
    ASSERT_FALSE(Fixture.Binary.empty());
    const std::vector<uint8_t> Before = Fixture.Binary;

    MachOPatcher Patcher;
    EXPECT_EQ(Patcher.appendExecSegment(Fixture.Binary,
                                        {0xd6, 0x5f, 0x03, 0xc0}, Name,
                                        Arch::AArch64),
              0u);
    EXPECT_EQ(Fixture.Binary, Before);
  }
}

TEST(MachOPatchSegmentInstall,
     RejectsLinkeditOffsetOrAddressOverflowWithoutMutation) {
  for (unsigned Mutation = 0; Mutation != 2; ++Mutation) {
    SCOPED_TRACE(Mutation);
    MachOPatchTransactionFixture Fixture =
        makeMachOPatchTransactionFixture(/*CompactCapacity=*/0x2000);
    ASSERT_FALSE(Fixture.Binary.empty());
    if (Mutation == 0) {
      llvm::MachO::symtab_command *Symtab = nullptr;
      forEachMachOLoadCommand(
          Fixture.Binary.data(), Fixture.Binary.size(),
          [&](const uint8_t *Command, uint32_t ID, uint32_t, bool) {
            if (ID == llvm::MachO::LC_SYMTAB)
              Symtab = reinterpret_cast<llvm::MachO::symtab_command *>(
                  const_cast<uint8_t *>(Command));
          });
      ASSERT_NE(Symtab, nullptr);
      Symtab->symoff = std::numeric_limits<uint32_t>::max() - 0x100;
    } else {
      auto *TextSegment = reinterpret_cast<llvm::MachO::segment_command_64 *>(
          Fixture.Binary.data() + sizeof(llvm::MachO::mach_header_64));
      auto *DataSegment = reinterpret_cast<llvm::MachO::segment_command_64 *>(
          reinterpret_cast<uint8_t *>(TextSegment) + TextSegment->cmdsize);
      auto *LinkeditSegment =
          reinterpret_cast<llvm::MachO::segment_command_64 *>(
              reinterpret_cast<uint8_t *>(DataSegment) + DataSegment->cmdsize);
      LinkeditSegment->vmaddr =
          std::numeric_limits<uint64_t>::max() - kTransactionLinkeditSize;
      LinkeditSegment->vmsize = kTransactionLinkeditSize;
    }
    const std::vector<uint8_t> Before = Fixture.Binary;

    MachOPatcher Patcher;
    EXPECT_EQ(Patcher.appendExecSegment(Fixture.Binary,
                                        {0xd6, 0x5f, 0x03, 0xc0},
                                        kDefaultNdTextSegment, Arch::AArch64),
              0u);
    EXPECT_EQ(Fixture.Binary, Before);
  }
}

TEST(MachOPatchSegmentInstall,
     RejectsUnalignedOrIncongruentLinkeditLayoutWithoutMutation) {
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    SCOPED_TRACE(Mutation);
    MachOPatchTransactionFixture Fixture =
        makeMachOPatchTransactionFixture(/*CompactCapacity=*/0x2000);
    ASSERT_FALSE(Fixture.Binary.empty());
    auto *TextSegment = reinterpret_cast<llvm::MachO::segment_command_64 *>(
        Fixture.Binary.data() + sizeof(llvm::MachO::mach_header_64));
    auto *DataSegment = reinterpret_cast<llvm::MachO::segment_command_64 *>(
        reinterpret_cast<uint8_t *>(TextSegment) + TextSegment->cmdsize);
    auto *LinkeditSegment = reinterpret_cast<llvm::MachO::segment_command_64 *>(
        reinterpret_cast<uint8_t *>(DataSegment) + DataSegment->cmdsize);
    if (Mutation == 0) {
      LinkeditSegment->vmaddr += 1;
    } else if (Mutation == 1) {
      LinkeditSegment->fileoff -= 1;
      LinkeditSegment->filesize += 1;
    } else {
      LinkeditSegment->vmaddr += 1;
      LinkeditSegment->fileoff -= 2;
      LinkeditSegment->filesize += 2;
    }
    const std::vector<uint8_t> Before = Fixture.Binary;

    MachOPatcher Patcher;
    EXPECT_EQ(Patcher.plannedExecSegmentVA(Fixture.Binary, Arch::AArch64), 0u);
    EXPECT_EQ(Patcher.appendExecSegment(Fixture.Binary,
                                        {0xd6, 0x5f, 0x03, 0xc0},
                                        kDefaultNdTextSegment, Arch::AArch64),
              0u);
    EXPECT_EQ(Fixture.Binary, Before);
  }
}

void expectStrictInjectedSegmentLayout(llvm::ArrayRef<uint8_t> Binary,
                                       llvm::StringRef InjectedName,
                                       Arch TargetArch) {
  unsigned InjectedCount = 0;
  unsigned LinkeditCount = 0;
  MachOSegFields Injected;
  MachOSegFields Linkedit;
  forEachMachOLoadCommand(
      Binary.data(), Binary.size(),
      [&](const uint8_t *Command, uint32_t ID, uint32_t, bool Is64) {
        if (ID != getMachOSegmentCmdID(Is64))
          return;
        const MachOSegFields Segment = readMachOSegment(Command, Is64);
        const std::string Name = readMachOName(Segment.SegName);
        if (Name == InjectedName) {
          ++InjectedCount;
          Injected = Segment;
        }
        if (Name == section_names::macho::LinkeditSeg) {
          ++LinkeditCount;
          Linkedit = Segment;
        }
      });
  ASSERT_EQ(InjectedCount, 1u);
  ASSERT_EQ(LinkeditCount, 1u);
  EXPECT_EQ(Injected.MaxProt,
            llvm::MachO::VM_PROT_READ | llvm::MachO::VM_PROT_EXECUTE);
  EXPECT_EQ(Injected.InitProt,
            llvm::MachO::VM_PROT_READ | llvm::MachO::VM_PROT_EXECUTE);
  EXPECT_EQ(Injected.NSects, 0u);
  EXPECT_NE(Injected.VMSize, 0u);
  EXPECT_EQ(Injected.FileSize, Injected.VMSize);
  const uint64_t PageSize = machoPageSize(TargetArch);
  ASSERT_NE(PageSize, 0u);
  EXPECT_EQ(Injected.VMAddr % PageSize, 0u);
  EXPECT_EQ(Injected.FileOff % PageSize, 0u);
  EXPECT_EQ(Linkedit.VMAddr % PageSize, 0u);
  EXPECT_EQ(Linkedit.FileOff % PageSize, 0u);
  ASSERT_LE(Injected.VMAddr,
            std::numeric_limits<uint64_t>::max() - Injected.VMSize);
  ASSERT_LE(Injected.FileOff,
            std::numeric_limits<uint64_t>::max() - Injected.FileSize);
  ASSERT_LE(Linkedit.FileOff,
            std::numeric_limits<uint64_t>::max() - Linkedit.FileSize);
  EXPECT_EQ(Injected.VMAddr + Injected.VMSize, Linkedit.VMAddr);
  EXPECT_EQ(Injected.FileOff + Injected.FileSize, Linkedit.FileOff);
  EXPECT_EQ(Linkedit.FileOff + Linkedit.FileSize, Binary.size());

  MachOPatcher Patcher;
  EXPECT_EQ(Patcher.plannedExecSegmentVA(
                std::vector<uint8_t>(Binary.begin(), Binary.end()), TargetArch),
            Linkedit.VMAddr);
}

std::unique_ptr<llvm::Module> makeMachOPatchTransactionModule(
    llvm::LLVMContext &Context,
    llvm::GlobalValue::LinkageTypes Linkage =
        llvm::GlobalValue::ExternalLinkage,
    llvm::StringRef FunctionName = "lifted_source_function") {
  auto Module =
      std::make_unique<llvm::Module>("macho-patch-transaction", Context);
  auto *VoidFunctionType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto *PersonalityType = llvm::FunctionType::get(
      llvm::Type::getInt32Ty(Context), /*isVarArg=*/true);
  auto *Personality = llvm::Function::Create(PersonalityType,
                                             llvm::GlobalValue::ExternalLinkage,
                                             "test_personality", Module.get());
  auto *MayThrow = llvm::Function::Create(VoidFunctionType,
                                          llvm::GlobalValue::ExternalLinkage,
                                          "may_throw", Module.get());
  auto *Function = llvm::Function::Create(VoidFunctionType, Linkage,
                                          FunctionName, Module.get());
  rewrite_source::setOriginalVA(*Function, kTransactionFunctionVA);
  Function->setPersonalityFn(Personality);
  Function->setUWTableKind(llvm::UWTableKind::Default);
  Function->addFnAttr("frame-pointer", "all");

  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Function);
  llvm::BasicBlock *Return =
      llvm::BasicBlock::Create(Context, "return", Function);
  llvm::BasicBlock *Landing =
      llvm::BasicBlock::Create(Context, "landing", Function);
  llvm::IRBuilder<> EntryBuilder(Entry);
  EntryBuilder.CreateInvoke(MayThrow, Return, Landing);
  llvm::IRBuilder<> ReturnBuilder(Return);
  ReturnBuilder.CreateRetVoid();

  llvm::IRBuilder<> LandingBuilder(Landing);
  auto *LandingPadType = llvm::StructType::get(
      llvm::PointerType::get(Context, 0), llvm::Type::getInt32Ty(Context));
  llvm::LandingPadInst *LandingPad =
      LandingBuilder.CreateLandingPad(LandingPadType, 0);
  LandingPad->setCleanup(true);
  LandingBuilder.CreateResume(LandingPad);
  return Module;
}

std::unique_ptr<llvm::Module>
makeMachOPatchNoUnwindModule(llvm::LLVMContext &Context) {
  auto Module =
      std::make_unique<llvm::Module>("macho-patch-no-unwind", Context);
  auto *FunctionType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto *Function =
      llvm::Function::Create(FunctionType, llvm::GlobalValue::ExternalLinkage,
                             "lifted_source_function", Module.get());
  rewrite_source::setOriginalVA(*Function, kTransactionFunctionVA);
  Function->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  Builder.CreateRetVoid();
  return Module;
}

bool writeFixtureFile(llvm::StringRef Path, llvm::ArrayRef<uint8_t> Bytes) {
  std::error_code Error;
  llvm::raw_fd_ostream Stream(Path, Error, llvm::sys::fs::OF_None);
  if (Error)
    return false;
  Stream.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
  Stream.close();
  return !Stream.has_error();
}

std::vector<uint8_t> readFixtureFile(llvm::StringRef Path) {
  auto BufferOrErr = llvm::MemoryBuffer::getFile(Path);
  if (!BufferOrErr) {
    ADD_FAILURE() << "cannot read fixture file: "
                  << BufferOrErr.getError().message();
    return {};
  }
  llvm::StringRef Bytes = (*BufferOrErr)->getBuffer();
  const auto *Begin = reinterpret_cast<const uint8_t *>(Bytes.data());
  return std::vector<uint8_t>(Begin, Begin + Bytes.size());
}

TEST(MachOPatchLayout, RejectsSegmentCommandOutsideItsDeclaredExtent) {
  using namespace llvm::MachO;

  std::vector<uint8_t> Binary(
      sizeof(mach_header_64) + sizeof(segment_command_64), 0);
  auto *Header = reinterpret_cast<mach_header_64 *>(Binary.data());
  Header->magic = MH_MAGIC_64;
  Header->cputype = CPU_TYPE_ARM64;
  Header->cpusubtype = CPU_SUBTYPE_ARM64_ALL;
  Header->filetype = MH_EXECUTE;
  Header->ncmds = 1;
  Header->sizeofcmds = sizeof(load_command);

  auto *Segment = reinterpret_cast<segment_command_64 *>(
      Binary.data() + sizeof(mach_header_64));
  Segment->cmd = LC_SEGMENT_64;
  Segment->cmdsize = sizeof(load_command);
  setMachOName(Segment->segname, section_names::macho::LinkeditSeg);
  Segment->vmaddr = kImageBase + 0x4000;
  Segment->vmsize = 0x1000;
  Segment->fileoff = sizeof(mach_header_64) + sizeof(load_command);
  Segment->filesize = 1;

  const std::vector<uint8_t> Before = Binary;
  MachOPatcher Patcher;
  EXPECT_EQ(Patcher.plannedExecSegmentVA(Binary, Arch::AArch64), 0u);
  EXPECT_EQ(Binary, Before);
}

TEST(MachOPatchLayout,
     RejectsUnboundedSegmentSectionCountWithoutReplacingOutput) {
  using namespace llvm::MachO;

  std::vector<uint8_t> Binary(
      sizeof(mach_header_64) + sizeof(segment_command_64), 0);
  auto *Header = reinterpret_cast<mach_header_64 *>(Binary.data());
  Header->magic = MH_MAGIC_64;
  Header->cputype = CPU_TYPE_ARM64;
  Header->cpusubtype = CPU_SUBTYPE_ARM64_ALL;
  Header->filetype = MH_EXECUTE;
  Header->ncmds = 1;
  Header->sizeofcmds = sizeof(segment_command_64);

  auto *Segment = reinterpret_cast<segment_command_64 *>(
      Binary.data() + sizeof(mach_header_64));
  Segment->cmd = LC_SEGMENT_64;
  Segment->cmdsize = sizeof(segment_command_64);
  setMachOName(Segment->segname, section_names::macho::TextSeg);
  Segment->vmaddr = kImageBase;
  Segment->vmsize = Binary.size();
  Segment->filesize = Binary.size();
  Segment->nsects = std::numeric_limits<uint32_t>::max();

  llvm::SmallString<128> InputPath;
  llvm::SmallString<128> OutputPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
      "neverd-macho-patch-unbounded-sections-input", "macho", InputPath));
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
      "neverd-macho-patch-unbounded-sections-output", "macho", OutputPath));
  llvm::FileRemover RemoveInput(InputPath);
  llvm::FileRemover RemoveOutput(OutputPath);

  const std::vector<uint8_t> OutputSentinel = {0x42, 0x4f, 0x55, 0x4e, 0x44};
  ASSERT_TRUE(writeFixtureFile(InputPath, Binary));
  ASSERT_TRUE(writeFixtureFile(OutputPath, OutputSentinel));

  llvm::LLVMContext Context;
  llvm::Module Module("malformed-macho", Context);
  MachOPatcher Patcher;
  PatchResult Result = Patcher.patch(
      InputPath.str().str(), OutputPath.str().str(), Module, Arch::AArch64);

  EXPECT_FALSE(Result.Success);
  EXPECT_EQ(readFixtureFile(InputPath), Binary);
  EXPECT_EQ(readFixtureFile(OutputPath), OutputSentinel);
}

MachOCompactUnwindMergeResult makeMergedResult(Arch TargetArch,
                                               size_t RecordCount) {
  MachOCompactUnwindMergeResult Result;
  Result.TargetArch = TargetArch;
  uint32_t Encoding = macho_unwind::kX86_64ModeRBPFrame;
  if (TargetArch == Arch::X86)
    Encoding = macho_unwind::kX86ModeEBPFrame;
  else if (TargetArch == Arch::ARM)
    Encoding = macho_unwind::kARMModeFrame;
  else if (TargetArch == Arch::AArch64)
    Encoding = macho_unwind::kARM64ModeFrame;
  for (size_t I = 0; I < RecordCount; ++I) {
    MachOCompactUnwindMergedRecord Record;
    Record.FunctionRVA = 0x100 + static_cast<uint32_t>(I) * 0x10;
    Record.FunctionEndRVA = Record.FunctionRVA + 0x10;
    Record.Encoding = Encoding;
    Record.Origin = MachOCompactUnwindRecordOrigin::Generated;
    Record.InputRecordIndex = I;
    Result.Records.push_back(std::move(Record));
  }
  if (!Result.Records.empty())
    Result.TerminalFunctionRVA = Result.Records.back().FunctionEndRVA;
  return Result;
}

struct GeneratedFixture {
  BinaryImage Source;
  CompiledImage Compiled;
  uint8_t PointerWidth = 8;
  llvm::endianness ByteOrder = llvm::endianness::little;

  GeneratedFixture(Arch TargetArch, Bitness Bits,
                   llvm::endianness ByteOrder = llvm::endianness::little)
      : PointerWidth(Bits == Bitness::Bits32 ? 4 : 8), ByteOrder(ByteOrder) {
    Source.Format = BinaryFormat::MachO;
    Source.Arch = TargetArch;
    Source.Bits = Bits;
    Source.Base = kImageBase;

    Section SlotSection;
    SlotSection.Name = "__got";
    SlotSection.SegmentName = "__DATA_CONST";
    SlotSection.VA = kPersonalitySlotVA;
    SlotSection.Size = 0x100;
    SlotSection.Flags = SegmentFlags::Readable;
    Source.Sections.push_back(std::move(SlotSection));
    Source.ImportPtrSlots[kPersonalitySlotVA] = "_personality";

    Compiled.Success = true;
    Compiled.BaseVA = kCodeVA;
    Compiled.TargetArch = TargetArch;
    Compiled.Format = BinaryFormat::MachO;
    Compiled.PointerWidth = PointerWidth;
    Compiled.ByteOrder = ByteOrder;
    Compiled.Bytes.resize(0x1100);

    CompiledSection Code;
    Code.Name = "__text";
    Code.VA = kCodeVA;
    Code.Size = 0x400;
    Code.Offset = 0;
    Code.Kind = llvm::mc_rewrite::RewriteSectionKind::Code;
    Code.IsAllocated = true;
    Compiled.Sections.push_back(std::move(Code));

    CompiledSection LSDA;
    LSDA.Name = "__gcc_except_tab";
    LSDA.VA = kLSDAVA;
    LSDA.Size = 0x100;
    LSDA.Offset = kLSDAVA - kCodeVA;
    LSDA.Kind = llvm::mc_rewrite::RewriteSectionKind::ReadOnlyData;
    LSDA.IsAllocated = true;
    Compiled.Sections.push_back(std::move(LSDA));

    CompiledSection Compact;
    Compact.Name = section_names::macho::CompactUnwind;
    Compact.Alignment = PointerWidth;
    Compact.Kind = llvm::mc_rewrite::RewriteSectionKind::Metadata;
    Compact.IsAllocated = false;
    Compact.IsInImage = false;
    Compiled.Sections.push_back(std::move(Compact));
  }

  CompiledSection &compact() { return Compiled.Sections.back(); }

  uint64_t recordSize() const { return 3 * PointerWidth + 8; }
  uint64_t personalityOffset() const { return PointerWidth + 8; }
  uint64_t lsdaOffset() const { return 2 * PointerWidth + 8; }

  void addFixup(uint64_t Offset, std::string Symbol,
                uint64_t FunctionRangeId = 0) {
    CompiledFixupReference Reference;
    Reference.Offset = Offset;
    Reference.FunctionRangeId = FunctionRangeId;
    Reference.Kind = PointerWidth == 8 ? llvm::FK_Data_8 : llvm::FK_Data_4;
    Reference.Symbol = std::move(Symbol);
    Reference.IsResolved = true;
    Reference.BitWidth = PointerWidth * 8;
    compact().FixupReferences.push_back(std::move(Reference));
  }

  void addRecord(uint64_t FunctionVA, uint32_t Length, uint32_t Encoding,
                 bool HasPersonality = false, bool HasLSDA = false,
                 uint64_t LSDAValue = kLSDAVA) {
    const uint64_t Index = compact().ExternalBytes.size() / recordSize();
    const uint64_t BaseOffset = compact().ExternalBytes.size();
    const uint64_t FunctionRangeId = Index + 1;
    const std::string OwnerSymbol = "_f" + std::to_string(Index);
    const std::string FunctionSymbol = "L_begin" + std::to_string(Index);
    const std::string LSDASymbol = "L_lsda" + std::to_string(Index);
    if (HasLSDA)
      Encoding |= macho_unwind::kHasLSDA;

    writePointer(compact().ExternalBytes, FunctionVA, PointerWidth, ByteOrder);
    writeU32(compact().ExternalBytes, Length, ByteOrder);
    writeU32(compact().ExternalBytes, Encoding, ByteOrder);
    // Deliberately not the pointer-slot VA: identity, not this numeric value,
    // must select the final personality slot.
    writePointer(compact().ExternalBytes,
                 HasPersonality ? kImageBase + 0x9000 : 0, PointerWidth,
                 ByteOrder);
    writePointer(compact().ExternalBytes, HasLSDA ? LSDAValue : 0, PointerWidth,
                 ByteOrder);
    compact().Size = compact().ExternalBytes.size();

    addFixup(BaseOffset, FunctionSymbol, FunctionRangeId);
    Compiled.SymbolAddrs[OwnerSymbol] = FunctionVA;
    Compiled.FunctionOwnerAddrs[OwnerSymbol] = FunctionVA;
    Compiled.FunctionRanges.push_back(
        {FunctionRangeId, OwnerSymbol, FunctionVA, FunctionSymbol, FunctionVA,
         "L_end" + std::to_string(Index), FunctionVA + Length});
    if (HasPersonality)
      addFixup(BaseOffset + personalityOffset(), "_personality");
    if (HasLSDA) {
      addFixup(BaseOffset + lsdaOffset(), LSDASymbol);
      Compiled.SymbolAddrs[LSDASymbol] = LSDAValue;
    }
  }
};

llvm::Expected<MachOCompactUnwindRecords>
parseGeneratedFixture(const CompiledImage &Compiled, const BinaryImage &Source,
                      llvm::endianness ByteOrder) {
  return ::neverd::parseGeneratedMachOCompactUnwind(Compiled, Source,
                                                    Source.Base, ByteOrder);
}

void expectFailure(llvm::Expected<MachOCompactUnwindRecords> Result,
                   Failure Expected,
                   std::optional<uint64_t> ExpectedRecord = std::nullopt) {
  ASSERT_FALSE(static_cast<bool>(Result));
  bool Seen = false;
  llvm::handleAllErrors(
      Result.takeError(),
      [&](const MachOCompactUnwindParseError &Error) {
        Seen = true;
        EXPECT_EQ(Error.reason(), Expected);
        if (ExpectedRecord)
          EXPECT_EQ(Error.recordIndex(), *ExpectedRecord);
      },
      [&](const llvm::ErrorInfoBase &Error) {
        std::string Message;
        llvm::raw_string_ostream Stream(Message);
        Error.log(Stream);
        ADD_FAILURE() << "unexpected error type: " << Stream.str();
      });
  EXPECT_TRUE(Seen);
}

void expectBindFailure(llvm::Expected<MachOCompactUnwindRecords> Result,
                       BindFailure Expected,
                       std::optional<uint64_t> ExpectedRecord = std::nullopt) {
  ASSERT_FALSE(static_cast<bool>(Result));
  bool Seen = false;
  llvm::handleAllErrors(
      Result.takeError(),
      [&](const MachOCompactUnwindDwarfBindError &Error) {
        Seen = true;
        EXPECT_EQ(Error.reason(), Expected);
        if (ExpectedRecord)
          EXPECT_EQ(Error.recordIndex(), *ExpectedRecord);
      },
      [&](const llvm::ErrorInfoBase &Error) {
        std::string Message;
        llvm::raw_string_ostream Stream(Message);
        Error.log(Stream);
        ADD_FAILURE() << "unexpected error type: " << Stream.str();
      });
  EXPECT_TRUE(Seen);
}

void expectRangeMapFailure(
    llvm::Expected<std::vector<MachOCompactUnwindRangeMapping>> Result,
    MapFailure Expected,
    std::optional<uint64_t> ExpectedRecord = std::nullopt) {
  ASSERT_FALSE(static_cast<bool>(Result));
  bool Seen = false;
  llvm::handleAllErrors(
      Result.takeError(),
      [&](const MachOCompactUnwindRangeMapError &Error) {
        Seen = true;
        EXPECT_EQ(Error.reason(), Expected);
        if (ExpectedRecord)
          EXPECT_EQ(Error.recordIndex(), *ExpectedRecord);
      },
      [&](const llvm::ErrorInfoBase &Error) {
        std::string Message;
        llvm::raw_string_ostream Stream(Message);
        Error.log(Stream);
        ADD_FAILURE() << "unexpected error type: " << Stream.str();
      });
  EXPECT_TRUE(Seen);
}

void expectMergeFailure(llvm::Expected<MachOCompactUnwindMergeResult> Result,
                        MergeFailure Expected,
                        MachOCompactUnwindMergeInputKind ExpectedKind =
                            MachOCompactUnwindMergeInputKind::None,
                        std::optional<uint64_t> ExpectedIndex = std::nullopt) {
  ASSERT_FALSE(static_cast<bool>(Result));
  bool Seen = false;
  llvm::handleAllErrors(
      Result.takeError(),
      [&](const MachOCompactUnwindMergeError &Error) {
        Seen = true;
        EXPECT_EQ(Error.reason(), Expected);
        EXPECT_EQ(Error.inputKind(), ExpectedKind);
        if (ExpectedIndex)
          EXPECT_EQ(Error.inputIndex(), *ExpectedIndex);
      },
      [&](const llvm::ErrorInfoBase &Error) {
        std::string Message;
        llvm::raw_string_ostream Stream(Message);
        Error.log(Stream);
        ADD_FAILURE() << "unexpected error type: " << Stream.str();
      });
  EXPECT_TRUE(Seen);
}

void expectEncodeFailure(llvm::Expected<std::vector<uint8_t>> Result,
                         EncodeFailure Expected,
                         MachOCompactUnwindEncodeInputKind ExpectedKind =
                             MachOCompactUnwindEncodeInputKind::None,
                         std::optional<uint64_t> ExpectedIndex = std::nullopt) {
  ASSERT_FALSE(static_cast<bool>(Result));
  bool Seen = false;
  llvm::handleAllErrors(
      Result.takeError(),
      [&](const MachOCompactUnwindEncodeError &Error) {
        Seen = true;
        EXPECT_EQ(Error.reason(), Expected);
        EXPECT_EQ(Error.inputKind(), ExpectedKind);
        if (ExpectedIndex)
          EXPECT_EQ(Error.inputIndex(), *ExpectedIndex);
      },
      [&](const llvm::ErrorInfoBase &Error) {
        std::string Message;
        llvm::raw_string_ostream Stream(Message);
        Error.log(Stream);
        ADD_FAILURE() << "unexpected error type: " << Stream.str();
      });
  EXPECT_TRUE(Seen);
}

template <typename T>
void expectInstallFailure(
    llvm::Expected<T> Result, InstallFailure Expected,
    std::optional<uint64_t> RequiredBytes = std::nullopt,
    std::optional<uint64_t> AvailableBytes = std::nullopt) {
  ASSERT_FALSE(static_cast<bool>(Result));
  bool Seen = false;
  llvm::handleAllErrors(
      Result.takeError(),
      [&](const MachOCompactUnwindInstallError &Error) {
        Seen = true;
        EXPECT_EQ(Error.reason(), Expected);
        if (RequiredBytes)
          EXPECT_EQ(Error.requiredBytes(), *RequiredBytes);
        if (AvailableBytes)
          EXPECT_EQ(Error.availableBytes(), *AvailableBytes);
      },
      [&](const llvm::ErrorInfoBase &Error) {
        std::string Message;
        llvm::raw_string_ostream Stream(Message);
        Error.log(Stream);
        ADD_FAILURE() << "unexpected error type: " << Stream.str();
      });
  EXPECT_TRUE(Seen);
}

TEST(MachOGeneratedCompactUnwind, Parses64BitRecordsBySymbolIdentity) {
  GeneratedFixture Fixture(Arch::AArch64, Bitness::Bits64);
  Fixture.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrameless,
                    /*HasPersonality=*/true, /*HasLSDA=*/true);

  auto Parsed = parseGeneratedFixture(Fixture.Compiled, Fixture.Source,
                                      llvm::endianness::little);
  ASSERT_TRUE(static_cast<bool>(Parsed));
  EXPECT_EQ(Parsed->TargetArch, Arch::AArch64);
  EXPECT_EQ(Parsed->PointerWidth, 8u);
  ASSERT_EQ(Parsed->Records.size(), 1u);
  const MachOCompactUnwindRecord &Record = Parsed->Records.front();
  EXPECT_EQ(Record.FunctionVA, kCodeVA);
  EXPECT_EQ(Record.FunctionEndVA, kCodeVA + 0x20);
  EXPECT_EQ(Record.FunctionRangeId, 1u);
  EXPECT_EQ(Record.OwnerSymbol, "_f0");
  EXPECT_EQ(Record.OwnerVA, kCodeVA);
  EXPECT_EQ(Record.FunctionSymbol, "L_begin0");
  ASSERT_TRUE(Fixture.Compiled.SymbolAddrs.contains(Record.OwnerSymbol));
  EXPECT_EQ(Fixture.Compiled.SymbolAddrs.at(Record.OwnerSymbol),
            Record.OwnerVA);
  EXPECT_FALSE(Fixture.Compiled.SymbolAddrs.contains(Record.FunctionSymbol));
  EXPECT_EQ(Record.PersonalitySymbol, "_personality");
  ASSERT_TRUE(Record.PersonalitySlotRVA.has_value());
  EXPECT_EQ(*Record.PersonalitySlotRVA, 0x3000u);
  EXPECT_EQ(Record.LSDASymbol, "L_lsda0");
  ASSERT_TRUE(Record.LSDAVA.has_value());
  EXPECT_EQ(*Record.LSDAVA, kLSDAVA);
}

TEST(MachOGeneratedCompactUnwind, RejectsCompiledFormatMismatch) {
  GeneratedFixture Fixture(Arch::AArch64, Bitness::Bits64);
  Fixture.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame);
  Fixture.Compiled.Format = BinaryFormat::ELF;

  expectFailure(parseGeneratedFixture(Fixture.Compiled, Fixture.Source,
                                      llvm::endianness::little),
                Failure::InvalidCompiledImage);
}

TEST(MachOGeneratedCompactUnwind, RejectsCompiledArchitectureMismatch) {
  GeneratedFixture Fixture(Arch::AArch64, Bitness::Bits64);
  Fixture.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame);
  Fixture.Compiled.TargetArch = Arch::X64;

  expectFailure(parseGeneratedFixture(Fixture.Compiled, Fixture.Source,
                                      llvm::endianness::little),
                Failure::InvalidCompiledImage);
}

TEST(MachOGeneratedCompactUnwind, RejectsCompiledPointerWidthMismatch) {
  GeneratedFixture Fixture(Arch::AArch64, Bitness::Bits64);
  Fixture.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame);
  Fixture.Compiled.PointerWidth = 4;

  expectFailure(parseGeneratedFixture(Fixture.Compiled, Fixture.Source,
                                      llvm::endianness::little),
                Failure::InvalidCompiledImage);
}

TEST(MachOGeneratedCompactUnwind, RejectsCompiledByteOrderMismatch) {
  GeneratedFixture Fixture(Arch::AArch64, Bitness::Bits64);
  Fixture.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame);
  Fixture.Compiled.ByteOrder = llvm::endianness::big;

  expectFailure(parseGeneratedFixture(Fixture.Compiled, Fixture.Source,
                                      llvm::endianness::little),
                Failure::InvalidCompiledImage);
}

TEST(MachOGeneratedCompactUnwind,
     RejectsMissingDuplicateDanglingAndMismatchedRangeProvenance) {
  GeneratedFixture Missing(Arch::AArch64, Bitness::Bits64);
  Missing.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame);
  Missing.compact().FixupReferences.front().FunctionRangeId = 0;
  expectFailure(parseGeneratedFixture(Missing.Compiled, Missing.Source,
                                      llvm::endianness::little),
                Failure::MissingFunctionRangeId, 0);

  GeneratedFixture Dangling(Arch::AArch64, Bitness::Bits64);
  Dangling.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame);
  Dangling.compact().FixupReferences.front().FunctionRangeId = 99;
  expectFailure(parseGeneratedFixture(Dangling.Compiled, Dangling.Source,
                                      llvm::endianness::little),
                Failure::DanglingFunctionRangeId, 0);

  GeneratedFixture Duplicate(Arch::AArch64, Bitness::Bits64);
  Duplicate.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame);
  Duplicate.addRecord(kCodeVA + 0x40, 0x20, macho_unwind::kARM64ModeFrameless);
  Duplicate.compact().FixupReferences[1].FunctionRangeId = 1;
  expectFailure(parseGeneratedFixture(Duplicate.Compiled, Duplicate.Source,
                                      llvm::endianness::little),
                Failure::DuplicateFunctionRangeId, 1);

  GeneratedFixture WrongSymbol(Arch::AArch64, Bitness::Bits64);
  WrongSymbol.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame);
  WrongSymbol.compact().FixupReferences.front().Symbol = "L_other_begin";
  expectFailure(parseGeneratedFixture(WrongSymbol.Compiled, WrongSymbol.Source,
                                      llvm::endianness::little),
                Failure::FunctionRangeSymbolMismatch, 0);

  GeneratedFixture CrossBoundary(Arch::AArch64, Bitness::Bits64);
  CrossBoundary.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame);
  CrossBoundary.addRecord(kCodeVA + 0x40, 0x20,
                          macho_unwind::kARM64ModeFrameless);
  writeU32At(CrossBoundary.compact().ExternalBytes, CrossBoundary.PointerWidth,
             0x60);
  expectFailure(parseGeneratedFixture(CrossBoundary.Compiled,
                                      CrossBoundary.Source,
                                      llvm::endianness::little),
                Failure::FunctionRangeBoundaryMismatch, 0);

  GeneratedFixture InvalidFlag(Arch::AArch64, Bitness::Bits64);
  InvalidFlag.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame);
  InvalidFlag.Compiled.FunctionRangesValid = false;
  expectFailure(parseGeneratedFixture(InvalidFlag.Compiled, InvalidFlag.Source,
                                      llvm::endianness::little),
                Failure::InvalidCompiledImage);
}

TEST(MachOGeneratedCompactUnwind,
     RealCompileParsesWithoutPublishingPrivateBeginSymbols) {
  llvm::LLVMContext Context;
  llvm::Module Module("compact-provenance-integration", Context);
  auto *FunctionType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto *Function =
      llvm::Function::Create(FunctionType, llvm::GlobalValue::ExternalLinkage,
                             "compiled_owner", Module);
  Function->setUWTableKind(llvm::UWTableKind::Default);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  Builder.CreateRetVoid();

  CompiledImage Compiled = compileImageForPatch(
      Module, Arch::AArch64, BinaryFormat::MachO, kCodeVA,
      [](llvm::StringRef, uint32_t) -> std::optional<uint64_t> {
        return std::nullopt;
      },
      kImageBase);
  ASSERT_TRUE(Compiled.Success);
  ASSERT_TRUE(Compiled.FunctionRangesValid);
  ASSERT_FALSE(Compiled.FunctionRanges.empty());
  for (const CompiledFunctionRange &Range : Compiled.FunctionRanges) {
    ASSERT_TRUE(Compiled.FunctionOwnerAddrs.contains(Range.OwnerSymbol));
    EXPECT_EQ(Compiled.FunctionOwnerAddrs.at(Range.OwnerSymbol), Range.OwnerVA);
    EXPECT_FALSE(Compiled.SymbolAddrs.contains(Range.BeginSymbol));
  }

  BinaryImage Source;
  Source.Format = BinaryFormat::MachO;
  Source.Arch = Arch::AArch64;
  Source.Bits = Bitness::Bits64;
  Source.Base = kImageBase;
  auto Parsed = parseGeneratedMachOCompactUnwind(Compiled, Source, kImageBase,
                                                 llvm::endianness::little);
  ASSERT_TRUE(static_cast<bool>(Parsed)) << llvm::toString(Parsed.takeError());
  ASSERT_FALSE(Parsed->Records.empty());
  EXPECT_EQ(Parsed->Records.size(), Compiled.FunctionRanges.size());
  for (const MachOCompactUnwindRecord &Record : Parsed->Records) {
    EXPECT_NE(Record.FunctionRangeId, 0u);
    EXPECT_FALSE(Record.OwnerSymbol.empty());
    EXPECT_FALSE(Compiled.SymbolAddrs.contains(Record.FunctionSymbol));
  }
}

TEST(MachOGeneratedCompactUnwind, NormalizesDisjointRowsByFunctionRange) {
  GeneratedFixture Fixture(Arch::AArch64, Bitness::Bits64);
  Fixture.addRecord(kCodeVA + 0x80, 0x20, macho_unwind::kARM64ModeFrameless);
  Fixture.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame);

  auto Parsed = parseGeneratedFixture(Fixture.Compiled, Fixture.Source,
                                      llvm::endianness::little);
  ASSERT_TRUE(static_cast<bool>(Parsed));
  ASSERT_EQ(Parsed->Records.size(), 2u);
  EXPECT_EQ(Parsed->Records[0].FunctionVA, kCodeVA);
  EXPECT_EQ(Parsed->Records[0].SourceRecordIndex, 1u);
  EXPECT_EQ(Parsed->Records[1].FunctionVA, kCodeVA + 0x80);
  EXPECT_EQ(Parsed->Records[1].SourceRecordIndex, 0u);
}

TEST(MachOGeneratedCompactUnwind, Parses32BitRecordLayout) {
  GeneratedFixture Fixture(Arch::X86, Bitness::Bits32);
  Fixture.Source.Base = 0;
  Fixture.Source.Sections.front().VA = 0x3000;
  Fixture.Source.ImportPtrSlots.clear();
  Fixture.addRecord(0x1000, 0x10, macho_unwind::kX86ModeEBPFrame);
  Fixture.Compiled.Sections[0].VA = 0x1000;
  Fixture.Compiled.BaseVA = 0x1000;
  Fixture.Compiled.SymbolAddrs["_f0"] = 0x1000;

  auto Parsed = parseGeneratedFixture(Fixture.Compiled, Fixture.Source,
                                      llvm::endianness::little);
  ASSERT_TRUE(static_cast<bool>(Parsed));
  EXPECT_EQ(Parsed->PointerWidth, 4u);
  ASSERT_EQ(Parsed->Records.size(), 1u);
  EXPECT_EQ(Parsed->Records.front().FunctionEndVA, 0x1010u);
}

TEST(MachOGeneratedCompactUnwind, ParsesARM32RecordLayout) {
  GeneratedFixture Fixture(Arch::ARM, Bitness::Bits32);
  Fixture.Source.Base = 0;
  Fixture.Source.Sections.front().VA = 0x3000;
  Fixture.Source.ImportPtrSlots.clear();
  Fixture.addRecord(0x1000, 0x20, macho_unwind::kARMModeFrame);
  Fixture.Compiled.Sections[0].VA = 0x1000;
  Fixture.Compiled.BaseVA = 0x1000;
  Fixture.Compiled.SymbolAddrs["_f0"] = 0x1000;

  auto Parsed = parseGeneratedFixture(Fixture.Compiled, Fixture.Source,
                                      llvm::endianness::little);
  ASSERT_TRUE(static_cast<bool>(Parsed)) << llvm::toString(Parsed.takeError());
  ASSERT_EQ(Parsed->Records.size(), 1u);
  EXPECT_EQ(Parsed->TargetArch, Arch::ARM);
  EXPECT_EQ(Parsed->PointerWidth, 4u);
  EXPECT_EQ(Parsed->Records.front().Encoding & macho_unwind::kModeMask,
            macho_unwind::kARMModeFrame);
}

TEST(MachOGeneratedCompactUnwind, HonorsExplicitBigEndianFields) {
  GeneratedFixture Fixture(Arch::AArch64, Bitness::Bits64,
                           llvm::endianness::big);
  Fixture.addRecord(kCodeVA + 0x40, 0x24, macho_unwind::kARM64ModeFrame);

  auto Parsed = parseGeneratedFixture(Fixture.Compiled, Fixture.Source,
                                      llvm::endianness::big);
  ASSERT_TRUE(static_cast<bool>(Parsed));
  ASSERT_EQ(Parsed->Records.size(), 1u);
  EXPECT_EQ(Parsed->Records.front().FunctionVA, kCodeVA + 0x40);
  EXPECT_EQ(Parsed->Records.front().RangeLength, 0x24u);
  EXPECT_EQ(Parsed->Records.front().Encoding, macho_unwind::kARM64ModeFrame);
}

TEST(MachOGeneratedCompactUnwind,
     UsesTrustedMachHeaderVAInsteadOfCachedSegmentOrder) {
  GeneratedFixture Fixture(Arch::AArch64, Bitness::Bits64);
  Segment HeaderSegment;
  HeaderSegment.Name = "__TEXT";
  HeaderSegment.VA = kImageBase + 0x800;
  HeaderSegment.Size = 0x1000;
  HeaderSegment.FileOff = 0;
  HeaderSegment.FileSz = 0x1000;
  Fixture.Source.Segments.push_back(std::move(HeaderSegment));
  Fixture.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame,
                    /*HasPersonality=*/true);

  auto Parsed = parseGeneratedFixture(Fixture.Compiled, Fixture.Source,
                                      llvm::endianness::little);
  ASSERT_TRUE(static_cast<bool>(Parsed));
  ASSERT_EQ(Parsed->Records.size(), 1u);
  ASSERT_TRUE(Parsed->Records.front().PersonalitySlotRVA.has_value());
  EXPECT_EQ(*Parsed->Records.front().PersonalitySlotRVA, 0x3000u);
}

TEST(MachOGeneratedCompactUnwind, RejectsShortAndTrailingRecords) {
  GeneratedFixture Short(Arch::AArch64, Bitness::Bits64);
  Short.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame);
  Short.compact().ExternalBytes.resize(Short.recordSize() - 1);
  Short.compact().Size = Short.compact().ExternalBytes.size();
  expectFailure(parseGeneratedFixture(Short.Compiled, Short.Source,
                                      llvm::endianness::little),
                Failure::SectionTooShort);

  GeneratedFixture Trailing(Arch::AArch64, Bitness::Bits64);
  Trailing.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame);
  Trailing.compact().ExternalBytes.push_back(0);
  Trailing.compact().Size = Trailing.compact().ExternalBytes.size();
  expectFailure(parseGeneratedFixture(Trailing.Compiled, Trailing.Source,
                                      llvm::endianness::little),
                Failure::TrailingBytes);
}

TEST(MachOGeneratedCompactUnwind, RejectsNoncanonicalSectionAlignment) {
  GeneratedFixture Fixture(Arch::AArch64, Bitness::Bits64);
  Fixture.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame);
  Fixture.compact().Alignment = 16;
  expectFailure(parseGeneratedFixture(Fixture.Compiled, Fixture.Source,
                                      llvm::endianness::little),
                Failure::InvalidSectionAlignment);
}

TEST(MachOGeneratedCompactUnwind, RejectsOverflowAndWrongArchitectureMode) {
  GeneratedFixture Overflow(Arch::AArch64, Bitness::Bits64);
  const uint64_t Start = std::numeric_limits<uint64_t>::max() - 1;
  Overflow.addRecord(Start, 4, macho_unwind::kARM64ModeFrame);
  Overflow.Compiled.FunctionRanges.front().BeginVA = kCodeVA;
  Overflow.Compiled.FunctionRanges.front().EndVA = kCodeVA + 0x20;
  expectFailure(parseGeneratedFixture(Overflow.Compiled, Overflow.Source,
                                      llvm::endianness::little),
                Failure::RangeOverflow, 0);

  GeneratedFixture WrongMode(Arch::AArch64, Bitness::Bits64);
  WrongMode.addRecord(kCodeVA, 0x20, macho_unwind::kX86_64ModeRBPFrame);
  expectFailure(parseGeneratedFixture(WrongMode.Compiled, WrongMode.Source,
                                      llvm::endianness::little),
                Failure::UnsupportedEncoding, 0);
}

TEST(MachOGeneratedCompactUnwind, RejectsMissingAndAmbiguousFixups) {
  GeneratedFixture Missing(Arch::AArch64, Bitness::Bits64);
  Missing.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame);
  Missing.compact().FixupReferences.clear();
  expectFailure(parseGeneratedFixture(Missing.Compiled, Missing.Source,
                                      llvm::endianness::little),
                Failure::MissingFixup, 0);

  GeneratedFixture Ambiguous(Arch::AArch64, Bitness::Bits64);
  Ambiguous.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame);
  Ambiguous.compact().FixupReferences.push_back(
      Ambiguous.compact().FixupReferences.front());
  expectFailure(parseGeneratedFixture(Ambiguous.Compiled, Ambiguous.Source,
                                      llvm::endianness::little),
                Failure::AmbiguousFixup, 0);
}

TEST(MachOGeneratedCompactUnwind, RejectsMissingAndAmbiguousPersonalitySlots) {
  GeneratedFixture Missing(Arch::AArch64, Bitness::Bits64);
  Missing.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame,
                    /*HasPersonality=*/true);
  Missing.Source.ImportPtrSlots.clear();
  expectFailure(parseGeneratedFixture(Missing.Compiled, Missing.Source,
                                      llvm::endianness::little),
                Failure::MissingPersonalitySlot, 0);

  GeneratedFixture Ambiguous(Arch::AArch64, Bitness::Bits64);
  Ambiguous.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame,
                      /*HasPersonality=*/true);
  Ambiguous.Source.ImportPtrSlots[kPersonalitySlotVA + 8] = "_personality";
  expectFailure(parseGeneratedFixture(Ambiguous.Compiled, Ambiguous.Source,
                                      llvm::endianness::little),
                Failure::AmbiguousPersonalitySlot, 0);
}

TEST(MachOGeneratedCompactUnwind, RejectsLSDAOutsideGeneratedSections) {
  GeneratedFixture Fixture(Arch::AArch64, Bitness::Bits64);
  Fixture.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame,
                    /*HasPersonality=*/false, /*HasLSDA=*/true,
                    kImageBase + 0x8000);
  expectFailure(parseGeneratedFixture(Fixture.Compiled, Fixture.Source,
                                      llvm::endianness::little),
                Failure::LSDAOutsideGeneratedImage, 0);
}

TEST(MachOGeneratedCompactUnwind, RejectsOverlappingNormalizedRanges) {
  GeneratedFixture Fixture(Arch::AArch64, Bitness::Bits64);
  Fixture.addRecord(kCodeVA, 0x30, macho_unwind::kARM64ModeFrame);
  Fixture.addRecord(kCodeVA + 0x20, 0x20, macho_unwind::kARM64ModeFrameless);
  expectFailure(parseGeneratedFixture(Fixture.Compiled, Fixture.Source,
                                      llvm::endianness::little),
                Failure::InvalidCompiledImage);
}

TEST(MachOGeneratedCompactUnwind, RejectsUnsupportedRecordFormat) {
  GeneratedFixture Fixture(Arch::AArch64, Bitness::Bits64);
  Fixture.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame);
  Fixture.Source.Bits = Bitness::Bits256;
  expectFailure(parseGeneratedFixture(Fixture.Compiled, Fixture.Source,
                                      llvm::endianness::little),
                Failure::UnsupportedPointerWidth);

  GeneratedFixture ByteOrder(Arch::AArch64, Bitness::Bits64);
  ByteOrder.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame);
  expectFailure(parseGeneratedFixture(ByteOrder.Compiled, ByteOrder.Source,
                                      static_cast<llvm::endianness>(0xff)),
                Failure::UnsupportedEndianness);
}

TEST(MachOGeneratedCompactUnwind, RejectsMalformedFixupAndSymbolValue) {
  GeneratedFixture BadShape(Arch::AArch64, Bitness::Bits64);
  BadShape.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame,
                     /*HasPersonality=*/true);
  BadShape.compact().FixupReferences.back().BitWidth = 32;
  expectFailure(parseGeneratedFixture(BadShape.Compiled, BadShape.Source,
                                      llvm::endianness::little),
                Failure::InvalidFixup, 0);

  GeneratedFixture Addend(Arch::AArch64, Bitness::Bits64);
  Addend.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame);
  Addend.compact().FixupReferences.front().Addend = 4;
  expectFailure(parseGeneratedFixture(Addend.Compiled, Addend.Source,
                                      llvm::endianness::little),
                Failure::InvalidFixup, 0);

  GeneratedFixture WrongKind(Arch::AArch64, Bitness::Bits64);
  WrongKind.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame);
  WrongKind.compact().FixupReferences.front().Kind = llvm::FK_SecRel_4;
  expectFailure(parseGeneratedFixture(WrongKind.Compiled, WrongKind.Source,
                                      llvm::endianness::little),
                Failure::InvalidFixup, 0);

  GeneratedFixture Unresolved(Arch::AArch64, Bitness::Bits64);
  Unresolved.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame);
  Unresolved.compact().FixupReferences.front().IsResolved = false;
  expectFailure(parseGeneratedFixture(Unresolved.Compiled, Unresolved.Source,
                                      llvm::endianness::little),
                Failure::InvalidFixup, 0);

  GeneratedFixture BadOwner(Arch::AArch64, Bitness::Bits64);
  BadOwner.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame);
  BadOwner.Compiled.FunctionOwnerAddrs["_f0"] = kCodeVA + 4;
  expectFailure(parseGeneratedFixture(BadOwner.Compiled, BadOwner.Source,
                                      llvm::endianness::little),
                Failure::InvalidCompiledImage);
}

TEST(MachOGeneratedCompactUnwind, RejectsPointerAndFixupPresenceMismatch) {
  GeneratedFixture Personality(Arch::AArch64, Bitness::Bits64);
  Personality.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame,
                        /*HasPersonality=*/true);
  overwritePointer(Personality.compact().ExternalBytes,
                   Personality.personalityOffset(), 0, Personality.PointerWidth,
                   Personality.ByteOrder);
  expectFailure(parseGeneratedFixture(Personality.Compiled, Personality.Source,
                                      llvm::endianness::little),
                Failure::EncodingFieldMismatch, 0);

  GeneratedFixture PersonalityWithoutFixup(Arch::AArch64, Bitness::Bits64);
  PersonalityWithoutFixup.addRecord(kCodeVA, 0x20,
                                    macho_unwind::kARM64ModeFrame,
                                    /*HasPersonality=*/true);
  PersonalityWithoutFixup.compact().FixupReferences.pop_back();
  expectFailure(parseGeneratedFixture(PersonalityWithoutFixup.Compiled,
                                      PersonalityWithoutFixup.Source,
                                      llvm::endianness::little),
                Failure::MissingFixup, 0);

  GeneratedFixture LSDA(Arch::AArch64, Bitness::Bits64);
  LSDA.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame,
                 /*HasPersonality=*/false, /*HasLSDA=*/true);
  overwritePointer(LSDA.compact().ExternalBytes, LSDA.lsdaOffset(), 0,
                   LSDA.PointerWidth, LSDA.ByteOrder);
  expectFailure(parseGeneratedFixture(LSDA.Compiled, LSDA.Source,
                                      llvm::endianness::little),
                Failure::EncodingFieldMismatch, 0);

  GeneratedFixture LSDAWithoutFixup(Arch::AArch64, Bitness::Bits64);
  LSDAWithoutFixup.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame,
                             /*HasPersonality=*/false, /*HasLSDA=*/true);
  LSDAWithoutFixup.compact().FixupReferences.pop_back();
  expectFailure(parseGeneratedFixture(LSDAWithoutFixup.Compiled,
                                      LSDAWithoutFixup.Source,
                                      llvm::endianness::little),
                Failure::MissingFixup, 0);
}

TEST(MachOGeneratedCompactUnwind, RequiresPublicOwnerAndBackedCode) {
  GeneratedFixture MissingOwner(Arch::AArch64, Bitness::Bits64);
  MissingOwner.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame);
  MissingOwner.Compiled.FunctionOwnerAddrs.clear();
  expectFailure(parseGeneratedFixture(MissingOwner.Compiled,
                                      MissingOwner.Source,
                                      llvm::endianness::little),
                Failure::InvalidCompiledImage);

  GeneratedFixture Unbacked(Arch::AArch64, Bitness::Bits64);
  Unbacked.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrame);
  Unbacked.Compiled.Bytes.resize(0x10);
  expectFailure(parseGeneratedFixture(Unbacked.Compiled, Unbacked.Source,
                                      llvm::endianness::little),
                Failure::InvalidSectionStorage, 0);
}

//===----------------------------------------------------------------------===//
// Generated DWARF fallback binding
//===----------------------------------------------------------------------===//

TEST(MachOCompactUnwindDwarfBind,
     BindsOnlyFromInstalledReceiptAndLeavesGeneratedInputUnchanged) {
  static_assert(!std::is_default_constructible_v<MachOEHFrameInstallReceipt>);
  static_assert(!std::is_aggregate_v<MachOEHFrameInstallReceipt>);
  static_assert(!std::is_copy_assignable_v<MachOEHFrameInstallReceipt>);
  static_assert(!std::is_move_assignable_v<MachOEHFrameInstallReceipt>);

  const auto Generated = makeGeneratedRecords(
      {makeGeneratedRecord(kCodeVA, kCodeVA + 0x20,
                           macho_unwind::kARM64ModeDwarf, std::nullopt,
                           std::nullopt, 0),
       makeGeneratedRecord(kCodeVA + 0x40, kCodeVA + 0x60,
                           macho_unwind::kARM64ModeFrame, std::nullopt,
                           std::nullopt, 1)});
  auto ReceiptOrErr =
      makeEHFrameReceipt(kCodeVA, kCodeVA + 0x20, "_generated_0", 1);
  ASSERT_TRUE(static_cast<bool>(ReceiptOrErr))
      << llvm::toString(ReceiptOrErr.takeError());
  const MachOEHFrameInstallReceipt &Receipt = *ReceiptOrErr;
  ASSERT_EQ(Receipt.disposition(), MachOEHFrameInstallDisposition::Installed);
  ASSERT_TRUE(Receipt.region().has_value());
  ASSERT_EQ(Receipt.installedFDEs().size(), 1u);
  ASSERT_EQ(Receipt.authenticatedFunctionRanges().size(), 1u);
  EXPECT_EQ(Receipt.authenticatedFunctionRanges().front().Id, 1u);
  EXPECT_TRUE(
      Receipt.authenticatedFunctionOwnerAddrs().contains("_generated_0"));
  EXPECT_EQ(Receipt.authenticatedFunctionOwnerAddrs().at("_generated_0"),
            kCodeVA);
  EXPECT_FALSE(Receipt.installedSymbolAddrs().contains("_generated_0"));
  EXPECT_FALSE(Receipt.installedSymbolAddrs().contains("L_begin_0"));
  const uint64_t FDEOffset =
      Receipt.installedFDEs().front().RecordVA - Receipt.region()->SectionVA;
  ASSERT_GT(FDEOffset, 0u);
  ASSERT_LE(FDEOffset, macho_unwind::kDwarfSectionOffsetMask);
  const uint32_t OriginalDwarfEncoding = Generated.Records[0].Encoding;

  auto Bound = bindMachOCompactUnwindDwarfFDEs(Generated, Receipt);
  ASSERT_TRUE(static_cast<bool>(Bound)) << llvm::toString(Bound.takeError());
  ASSERT_EQ(Bound->Records.size(), 2u);
  EXPECT_EQ(Bound->Records[0].Encoding,
            macho_unwind::kARM64ModeDwarf | static_cast<uint32_t>(FDEOffset));
  EXPECT_EQ(Bound->Records[1].Encoding, Generated.Records[1].Encoding);
  EXPECT_EQ(Generated.Records[0].Encoding, OriginalDwarfEncoding);
}

TEST(MachOCompactUnwindDwarfBind, BindsARM32DwarfFallbackByExactRange) {
  constexpr uint64_t FunctionVA = kMachO32Base + 0x600;
  const auto Generated =
      makeGeneratedRecords({makeGeneratedRecord(FunctionVA, FunctionVA + 0x20,
                                                macho_unwind::kARMModeDwarf)},
                           Arch::ARM);
  auto Receipt = makeEHFrameReceiptForArch(
      Arch::ARM, FunctionVA, FunctionVA + 0x20, "_generated_0", 1);
  ASSERT_TRUE(static_cast<bool>(Receipt))
      << llvm::toString(Receipt.takeError());
  ASSERT_TRUE(Receipt->region().has_value());
  EXPECT_FALSE(Receipt->region()->Is64);
  EXPECT_EQ(Receipt->targetArch(), Arch::ARM);
  EXPECT_EQ(Receipt->pointerWidth(), 4);
  EXPECT_EQ(Receipt->byteOrder(), llvm::endianness::little);
  const uint64_t FDEOffset =
      Receipt->installedFDEs().front().RecordVA - Receipt->region()->SectionVA;

  auto Bound = bindMachOCompactUnwindDwarfFDEs(Generated, *Receipt);
  ASSERT_TRUE(static_cast<bool>(Bound)) << llvm::toString(Bound.takeError());
  ASSERT_EQ(Bound->Records.size(), 1u);
  EXPECT_EQ(Bound->Records.front().Encoding,
            macho_unwind::kARMModeDwarf | static_cast<uint32_t>(FDEOffset));
}

TEST(MachOCompactUnwindDwarfBind,
     RejectsAArch64ReceiptForARM32GeneratedRecords) {
  const auto Generated =
      makeGeneratedRecords({makeGeneratedRecord(kCodeVA, kCodeVA + 0x20,
                                                macho_unwind::kARMModeDwarf)},
                           Arch::ARM);
  auto Receipt = makeEHFrameReceipt(kCodeVA, kCodeVA + 0x20, "_generated_0", 1);
  ASSERT_TRUE(static_cast<bool>(Receipt))
      << llvm::toString(Receipt.takeError());

  auto Bound = bindMachOCompactUnwindDwarfFDEs(Generated, *Receipt);
  expectBindFailure(std::move(Bound), BindFailure::ReceiptTargetMismatch);
}

TEST(MachOCompactUnwindDwarfBind,
     RejectsARM32ReceiptForAArch64GeneratedRecords) {
  constexpr uint64_t FunctionVA = kMachO32Base + 0x600;
  const auto Generated = makeGeneratedRecords({makeGeneratedRecord(
      FunctionVA, FunctionVA + 0x20, macho_unwind::kARM64ModeDwarf)});
  auto Receipt = makeEHFrameReceiptForArch(
      Arch::ARM, FunctionVA, FunctionVA + 0x20, "_generated_0", 1);
  ASSERT_TRUE(static_cast<bool>(Receipt))
      << llvm::toString(Receipt.takeError());

  expectBindFailure(bindMachOCompactUnwindDwarfFDEs(Generated, *Receipt),
                    BindFailure::ReceiptTargetMismatch);
}

TEST(MachOCompactUnwindDwarfBind,
     RejectsSameWidthReceiptFromDifferentArchitecture) {
  const auto Generated = makeGeneratedRecords(
      {makeGeneratedRecord(kCodeVA, kCodeVA + 0x20,
                           macho_unwind::kX86_64ModeDwarf)},
      Arch::X64);
  auto Receipt = makeEHFrameReceipt(kCodeVA, kCodeVA + 0x20, "_generated_0", 1);
  ASSERT_TRUE(static_cast<bool>(Receipt))
      << llvm::toString(Receipt.takeError());
  ASSERT_EQ(Generated.PointerWidth, Receipt->pointerWidth());

  expectBindFailure(bindMachOCompactUnwindDwarfFDEs(Generated, *Receipt),
                    BindFailure::ReceiptTargetMismatch);
}

TEST(MachOCompactUnwindDwarfBind,
     RejectsInstalledReceiptWithDifferentPointerWidth) {
  auto Generated = makeGeneratedRecords({makeGeneratedRecord(
      kCodeVA, kCodeVA + 0x20, macho_unwind::kARM64ModeDwarf)});
  Generated.PointerWidth = 4;
  auto Receipt = makeEHFrameReceipt(kCodeVA, kCodeVA + 0x20, "_generated_0", 1);
  ASSERT_TRUE(static_cast<bool>(Receipt))
      << llvm::toString(Receipt.takeError());

  expectBindFailure(bindMachOCompactUnwindDwarfFDEs(Generated, *Receipt),
                    BindFailure::ReceiptTargetMismatch);
}

TEST(MachOCompactUnwindDwarfBind,
     RejectsInstalledReceiptWithDifferentByteOrder) {
  auto Generated = makeGeneratedRecords({makeGeneratedRecord(
      kCodeVA, kCodeVA + 0x20, macho_unwind::kARM64ModeDwarf)});
  Generated.ByteOrder = llvm::endianness::big;
  auto Receipt = makeEHFrameReceipt(kCodeVA, kCodeVA + 0x20, "_generated_0", 1);
  ASSERT_TRUE(static_cast<bool>(Receipt))
      << llvm::toString(Receipt.takeError());

  expectBindFailure(bindMachOCompactUnwindDwarfFDEs(Generated, *Receipt),
                    BindFailure::ReceiptTargetMismatch);
}

TEST(MachOCompactUnwindDwarfBind, RejectsMissingInstallReceipt) {
  const auto Generated = makeGeneratedRecords({makeGeneratedRecord(
      kCodeVA, kCodeVA + 0x20, macho_unwind::kARM64ModeDwarf, std::nullopt,
      std::nullopt, 11)});
  auto Receipt = makeNoOpEHFrameReceipt();
  ASSERT_TRUE(static_cast<bool>(Receipt))
      << llvm::toString(Receipt.takeError());
  expectBindFailure(bindMachOCompactUnwindDwarfFDEs(Generated, *Receipt),
                    BindFailure::MissingInstallReceipt);
}

TEST(MachOCompactUnwindDwarfBind,
     InstallerRejectsMissingAndMismatchedFDERangesBeforeBind) {
  auto Missing =
      makeEHFrameReceipt(kCodeVA + 0x40, kCodeVA + 0x60, "_generated_11", 12,
                         kCodeVA, kCodeVA, kCodeVA + 0x20);
  EXPECT_FALSE(static_cast<bool>(Missing));
  if (!Missing)
    llvm::consumeError(Missing.takeError());

  auto Mismatch = makeEHFrameReceipt(kCodeVA, kCodeVA + 0x24, "_generated_11",
                                     12, std::nullopt, kCodeVA, kCodeVA + 0x20);
  EXPECT_FALSE(static_cast<bool>(Mismatch));
  if (!Mismatch)
    llvm::consumeError(Mismatch.takeError());
}

TEST(MachOCompactUnwindDwarfBind,
     RejectsSymbolMismatchAndPrepopulatedDwarfPayload) {
  const auto Generated = makeGeneratedRecords({makeGeneratedRecord(
      kCodeVA, kCodeVA + 0x20, macho_unwind::kARM64ModeDwarf | 1u, std::nullopt,
      std::nullopt, 14)});
  auto WrongSymbol =
      makeEHFrameReceipt(kCodeVA, kCodeVA + 0x20, "_different_symbol", 15);
  ASSERT_TRUE(static_cast<bool>(WrongSymbol))
      << llvm::toString(WrongSymbol.takeError());

  auto Unpopulated = Generated;
  Unpopulated.Records.front().Encoding = macho_unwind::kARM64ModeDwarf;
  expectBindFailure(bindMachOCompactUnwindDwarfFDEs(Unpopulated, *WrongSymbol),
                    BindFailure::FunctionRangeIdentityMismatch, 14);

  auto Receipt =
      makeEHFrameReceipt(kCodeVA, kCodeVA + 0x20, "_generated_14", 15);
  ASSERT_TRUE(static_cast<bool>(Receipt))
      << llvm::toString(Receipt.takeError());

  expectBindFailure(bindMachOCompactUnwindDwarfFDEs(Generated, *Receipt),
                    BindFailure::PrepopulatedDwarfOffset, 14);
}

TEST(MachOCompactUnwindDwarfBind, RejectsDanglingAndRepeatedFunctionRangeIds) {
  MachOCompactUnwindRecord Record = makeGeneratedRecord(
      kCodeVA, kCodeVA + 0x20, macho_unwind::kARM64ModeDwarf);
  auto Receipt = makeEHFrameReceipt(kCodeVA, kCodeVA + 0x20, "_generated_0", 1);
  ASSERT_TRUE(static_cast<bool>(Receipt))
      << llvm::toString(Receipt.takeError());

  MachOCompactUnwindRecord Dangling = Record;
  Dangling.FunctionRangeId = 99;
  expectBindFailure(bindMachOCompactUnwindDwarfFDEs(
                        makeGeneratedRecords({Dangling}), *Receipt),
                    BindFailure::MissingFunctionRange, 0);

  MachOCompactUnwindRecord Duplicate = Record;
  Duplicate.SourceRecordIndex = 1;
  expectBindFailure(bindMachOCompactUnwindDwarfFDEs(
                        makeGeneratedRecords({Record, Duplicate}), *Receipt),
                    BindFailure::FunctionRangeIdentityMismatch, 1);
}

//===----------------------------------------------------------------------===//
// Installed trampoline to exact source-range mapping
//===----------------------------------------------------------------------===//

TEST(MachOCompactUnwindRangeMap,
     BuildsExactRangesFromSymbolIdentifiedInstalledTrampolines) {
  const auto Original = makeRawOriginal(
      {{0x100, macho_unwind::kARM64ModeFrame, std::nullopt},
       {0x180, macho_unwind::kARM64ModeFrameless, std::nullopt}},
      0x200);
  const std::vector<uint8_t> Binary =
      makeInstallableUnwindInfoMachO(Original, Original.OriginalBytes.size());
  const auto Generated = makeGeneratedRecords({makeGeneratedRecord(
      kImageBase + 0x500, kImageBase + 0x540, macho_unwind::kARM64ModeFrame,
      std::nullopt, std::nullopt, 7)});
  const PatchedFunctionEntry Installed{"_generated_7", kImageBase + 0x100,
                                       kImageBase + 0x500};

  auto Mappings = buildMachOCompactUnwindRangeMappings(
      Binary, Arch::AArch64, Generated, {Installed}, llvm::endianness::little);
  ASSERT_TRUE(static_cast<bool>(Mappings))
      << llvm::toString(Mappings.takeError());
  ASSERT_EQ(Mappings->size(), 1u);
  EXPECT_EQ(Mappings->front().SourceVA, kImageBase + 0x100);
  EXPECT_EQ(Mappings->front().SourceEndVA, kImageBase + 0x180);
  EXPECT_EQ(Mappings->front().DestinationVA, kImageBase + 0x500);
  EXPECT_EQ(Mappings->front().DestinationEndVA, kImageBase + 0x540);
  EXPECT_EQ(Mappings->front().Mode, MachOCompactUnwindRangeMode::NewSegment);
  EXPECT_EQ(Mappings->front().FunctionRangeId, 8u);
  EXPECT_EQ(Mappings->front().OwnerSymbol, "_generated_7");
  EXPECT_EQ(Mappings->front().OwnerVA, kImageBase + 0x500);
}

TEST(MachOCompactUnwindRangeMap,
     RejectsMissingSymbolMismatchedAndDuplicateSourceEvidence) {
  const auto Original = makeRawOriginal(
      {{0x100, macho_unwind::kARM64ModeFrame, std::nullopt}}, 0x180);
  const std::vector<uint8_t> Binary =
      makeInstallableUnwindInfoMachO(Original, Original.OriginalBytes.size());
  const auto One = makeGeneratedRecords({makeGeneratedRecord(
      kImageBase + 0x500, kImageBase + 0x520, macho_unwind::kARM64ModeFrame,
      std::nullopt, std::nullopt, 8)});

  expectRangeMapFailure(
      buildMachOCompactUnwindRangeMappings(Binary, Arch::AArch64, One, {},
                                           llvm::endianness::little),
      MapFailure::MissingTrampoline, 8);

  const PatchedFunctionEntry WrongSymbol{"_other", kImageBase + 0x100,
                                         kImageBase + 0x500};
  expectRangeMapFailure(
      buildMachOCompactUnwindRangeMappings(
          Binary, Arch::AArch64, One, {WrongSymbol}, llvm::endianness::little),
      MapFailure::TrampolineSymbolMismatch, 8);

  const PatchedFunctionEntry MissingSource{"_generated_8", kImageBase + 0x120,
                                           kImageBase + 0x500};
  expectRangeMapFailure(buildMachOCompactUnwindRangeMappings(
                            Binary, Arch::AArch64, One, {MissingSource},
                            llvm::endianness::little),
                        MapFailure::MissingSourceRange, 8);

  const auto Two = makeGeneratedRecords(
      {makeGeneratedRecord(kImageBase + 0x500, kImageBase + 0x520,
                           macho_unwind::kARM64ModeFrame, std::nullopt,
                           std::nullopt, 8),
       makeGeneratedRecord(kImageBase + 0x540, kImageBase + 0x560,
                           macho_unwind::kARM64ModeFrame, std::nullopt,
                           std::nullopt, 9)});
  const std::array<PatchedFunctionEntry, 2> DuplicateSource = {
      PatchedFunctionEntry{"_generated_8", kImageBase + 0x100,
                           kImageBase + 0x500},
      PatchedFunctionEntry{"_generated_9", kImageBase + 0x100,
                           kImageBase + 0x540}};
  expectRangeMapFailure(buildMachOCompactUnwindRangeMappings(
                            Binary, Arch::AArch64, Two, DuplicateSource,
                            llvm::endianness::little),
                        MapFailure::CrossOwnerSourceReuse, 9);
}

TEST(MachOCompactUnwindRangeMap,
     SameOwnerAdjacentAndDisjointFragmentsReuseOneSourceRecipe) {
  const auto Original = makeRawOriginal(
      {{0x100, macho_unwind::kARM64ModeFrame, std::nullopt}}, 0x180);
  const std::vector<uint8_t> Binary =
      makeInstallableUnwindInfoMachO(Original, Original.OriginalBytes.size());
  const PatchedFunctionEntry Installed{"_generated_0", kImageBase + 0x100,
                                       kImageBase + 0x500};

  for (const uint64_t SecondBegin : {kImageBase + 0x520, kImageBase + 0x540}) {
    SCOPED_TRACE(SecondBegin);
    MachOCompactUnwindRecord First = makeGeneratedRecord(
        kImageBase + 0x500, kImageBase + 0x520, macho_unwind::kARM64ModeFrame,
        std::nullopt, std::nullopt, 0);
    MachOCompactUnwindRecord Second = makeGeneratedRecord(
        SecondBegin, SecondBegin + 0x20, macho_unwind::kARM64ModeFrameless,
        std::nullopt, std::nullopt, 1);
    Second.OwnerSymbol = First.OwnerSymbol;
    Second.OwnerVA = First.OwnerVA;
    const MachOCompactUnwindRecords Generated =
        makeGeneratedRecords({First, Second});

    auto Mappings = buildMachOCompactUnwindRangeMappings(
        Binary, Arch::AArch64, Generated, {Installed},
        llvm::endianness::little);
    ASSERT_TRUE(static_cast<bool>(Mappings))
        << llvm::toString(Mappings.takeError());
    ASSERT_EQ(Mappings->size(), 2u);
    EXPECT_EQ((*Mappings)[0].SourceVA, kImageBase + 0x100);
    EXPECT_EQ((*Mappings)[1].SourceVA, kImageBase + 0x100);
    EXPECT_EQ((*Mappings)[0].OwnerSymbol, First.OwnerSymbol);
    EXPECT_EQ((*Mappings)[1].OwnerSymbol, First.OwnerSymbol);
    EXPECT_NE((*Mappings)[0].FunctionRangeId, (*Mappings)[1].FunctionRangeId);

    auto Merged = mergeMachOCompactUnwind(Arch::AArch64, kImageBase, Original,
                                          Generated, *Mappings);
    ASSERT_TRUE(static_cast<bool>(Merged))
        << llvm::toString(Merged.takeError());
    const size_t ExpectedGapCount = SecondBegin == kImageBase + 0x520 ? 1u : 2u;
    EXPECT_EQ(
        std::count_if(Merged->Records.begin(), Merged->Records.end(),
                      [](const MachOCompactUnwindMergedRecord &Record) {
                        return Record.Origin ==
                               MachOCompactUnwindRecordOrigin::GapBoundary;
                      }),
        ExpectedGapCount);
    if (SecondBegin != kImageBase + 0x520) {
      const auto Gap = llvm::find_if(
          Merged->Records, [](const MachOCompactUnwindMergedRecord &Record) {
            return Record.FunctionRVA == 0x520;
          });
      ASSERT_NE(Gap, Merged->Records.end());
      EXPECT_EQ(Gap->FunctionEndRVA, 0x540u);
      EXPECT_EQ(Gap->Origin, MachOCompactUnwindRecordOrigin::GapBoundary);
    }
  }
}

TEST(MachOCompactUnwindRangeMap, RejectsMissingAndDuplicateFragmentIds) {
  const auto Original = makeRawOriginal(
      {{0x100, macho_unwind::kARM64ModeFrame, std::nullopt}}, 0x180);
  const std::vector<uint8_t> Binary =
      makeInstallableUnwindInfoMachO(Original, Original.OriginalBytes.size());
  const PatchedFunctionEntry Installed{"_generated_0", kImageBase + 0x100,
                                       kImageBase + 0x500};

  MachOCompactUnwindRecord Missing = makeGeneratedRecord(
      kImageBase + 0x500, kImageBase + 0x520, macho_unwind::kARM64ModeFrame);
  Missing.FunctionRangeId = 0;
  expectRangeMapFailure(buildMachOCompactUnwindRangeMappings(
                            Binary, Arch::AArch64,
                            makeGeneratedRecords({Missing}), {Installed},
                            llvm::endianness::little),
                        MapFailure::InvalidFunctionRangeIdentity, 0);

  MachOCompactUnwindRecord First = makeGeneratedRecord(
      kImageBase + 0x500, kImageBase + 0x520, macho_unwind::kARM64ModeFrame);
  MachOCompactUnwindRecord Duplicate = makeGeneratedRecord(
      kImageBase + 0x540, kImageBase + 0x560, macho_unwind::kARM64ModeFrame,
      std::nullopt, std::nullopt, 1);
  Duplicate.FunctionRangeId = First.FunctionRangeId;
  Duplicate.OwnerSymbol = First.OwnerSymbol;
  Duplicate.OwnerVA = First.OwnerVA;
  expectRangeMapFailure(buildMachOCompactUnwindRangeMappings(
                            Binary, Arch::AArch64,
                            makeGeneratedRecords({First, Duplicate}),
                            {Installed}, llvm::endianness::little),
                        MapFailure::InvalidFunctionRangeIdentity, 1);
}

//===----------------------------------------------------------------------===//
// Strict original-table normalization and old-to-new range merging
//===----------------------------------------------------------------------===//

TEST(MachOCompactUnwindMerge, PreservesOriginalRangePersonalityAndLSDA) {
  const uint32_t Encoding = macho_unwind::kARM64ModeFrame |
                            macho_unwind::kHasLSDA |
                            (2u << macho_unwind::kPersonalityShift);
  const auto Original =
      makeRawOriginal({{0x100, Encoding, 0x900}}, 0x140, {0x3000, 0x4000});
  const auto Generated = makeGeneratedRecords({});

  auto Merged = mergeMachOCompactUnwind(Arch::AArch64, kImageBase, Original,
                                        Generated, {});
  ASSERT_TRUE(static_cast<bool>(Merged)) << llvm::toString(Merged.takeError());
  EXPECT_EQ(Merged->TargetArch, Arch::AArch64);
  EXPECT_EQ(Merged->TerminalFunctionRVA, 0x140u);
  EXPECT_EQ(Merged->PersonalitySlotRVAs, (std::vector<uint32_t>{0x4000}));
  ASSERT_EQ(Merged->Records.size(), 1u);
  const MachOCompactUnwindMergedRecord &Record = Merged->Records.front();
  EXPECT_EQ(Record.FunctionRVA, 0x100u);
  EXPECT_EQ(Record.FunctionEndRVA, 0x140u);
  EXPECT_EQ(Record.Encoding & macho_unwind::kPersonalityMask,
            1u << macho_unwind::kPersonalityShift);
  EXPECT_EQ(Record.Encoding & ~macho_unwind::kPersonalityMask,
            Encoding & ~macho_unwind::kPersonalityMask);
  EXPECT_EQ(Record.PersonalitySlotRVA, 0x4000u);
  EXPECT_EQ(Record.LSDARVA, 0x900u);
  EXPECT_EQ(Record.Origin, MachOCompactUnwindRecordOrigin::Original);
}

TEST(MachOCompactUnwindMerge,
     PreservesOriginalDwarfWithoutOffsetHintPersonalityAndLSDA) {
  const uint32_t Encoding = macho_unwind::kARM64ModeDwarf |
                            macho_unwind::kHasLSDA |
                            (1u << macho_unwind::kPersonalityShift);
  const auto Original =
      makeRawOriginal({{0x100, Encoding, 0x900}}, 0x140, {0x4000});

  auto Merged = mergeMachOCompactUnwind(Arch::AArch64, kImageBase, Original,
                                        makeGeneratedRecords({}), {});
  ASSERT_TRUE(static_cast<bool>(Merged)) << llvm::toString(Merged.takeError());
  ASSERT_EQ(Merged->Records.size(), 1u);
  const MachOCompactUnwindMergedRecord &Record = Merged->Records.front();
  EXPECT_EQ(Record.FunctionRVA, 0x100u);
  EXPECT_EQ(Record.FunctionEndRVA, 0x140u);
  EXPECT_EQ(Record.Encoding & macho_unwind::kModeMask,
            macho_unwind::kARM64ModeDwarf);
  EXPECT_EQ(Record.Encoding & macho_unwind::kDwarfSectionOffsetMask, 0u);
  EXPECT_EQ(Record.Encoding & macho_unwind::kPersonalityMask,
            1u << macho_unwind::kPersonalityShift);
  EXPECT_EQ(Record.PersonalitySlotRVA, 0x4000u);
  EXPECT_EQ(Record.LSDARVA, 0x900u);
  EXPECT_EQ(Record.Origin, MachOCompactUnwindRecordOrigin::Original);
}

TEST(MachOCompactUnwindMerge,
     NewSegmentPreservesTrampolineSourceAndAddsGeneratedRange) {
  const auto Original = makeRawOriginal(
      {{0x400, macho_unwind::kARM64ModeFrame, std::nullopt}}, 0x440);
  GeneratedFixture Fixture(Arch::AArch64, Bitness::Bits64);
  Fixture.addRecord(kCodeVA, 0x20, macho_unwind::kARM64ModeFrameless,
                    /*HasPersonality=*/true, /*HasLSDA=*/true);
  auto Generated = parseGeneratedFixture(Fixture.Compiled, Fixture.Source,
                                         llvm::endianness::little);
  ASSERT_TRUE(static_cast<bool>(Generated))
      << llvm::toString(Generated.takeError());
  const MachOCompactUnwindRangeMapping Mapping =
      makeMapping(0x400, 0x440, 0x1000, 0x1020,
                  MachOCompactUnwindRangeMode::NewSegment, 1, "_f0", kCodeVA);

  auto Merged = mergeMachOCompactUnwind(Arch::AArch64, kImageBase, Original,
                                        *Generated, {Mapping});
  ASSERT_TRUE(static_cast<bool>(Merged)) << llvm::toString(Merged.takeError());
  ASSERT_EQ(Merged->Records.size(), 3u);
  EXPECT_EQ(Merged->Records[0].FunctionRVA, 0x400u);
  EXPECT_EQ(Merged->Records[0].FunctionEndRVA, 0x440u);
  EXPECT_EQ(Merged->Records[0].Origin,
            MachOCompactUnwindRecordOrigin::Original);
  EXPECT_EQ(Merged->Records[1].FunctionRVA, 0x440u);
  EXPECT_EQ(Merged->Records[1].FunctionEndRVA, 0x1000u);
  EXPECT_EQ(Merged->Records[1].Encoding, 0u);
  EXPECT_EQ(Merged->Records[1].Origin,
            MachOCompactUnwindRecordOrigin::GapBoundary);
  EXPECT_EQ(Merged->Records[2].FunctionRVA, 0x1000u);
  EXPECT_EQ(Merged->Records[2].FunctionEndRVA, 0x1020u);
  EXPECT_EQ(Merged->Records[2].Origin,
            MachOCompactUnwindRecordOrigin::Generated);
  EXPECT_EQ(Merged->Records[2].PersonalitySlotRVA, 0x3000u);
  EXPECT_EQ(Merged->Records[2].LSDARVA, 0x2000u);
}

TEST(MachOCompactUnwindMerge, AdjacentDestinationNeedsNoZeroBoundary) {
  const auto Original = makeRawOriginal(
      {{0x100, macho_unwind::kARM64ModeFrame, std::nullopt}}, 0x140);
  const auto Generated = makeGeneratedRecords(
      {makeGeneratedRecord(kImageBase + 0x140, kImageBase + 0x160,
                           macho_unwind::kARM64ModeFrameless)});
  const MachOCompactUnwindRangeMapping Mapping = makeMapping(
      0x100, 0x140, 0x140, 0x160, MachOCompactUnwindRangeMode::NewSegment);

  auto Merged = mergeMachOCompactUnwind(Arch::AArch64, kImageBase, Original,
                                        Generated, {Mapping});
  ASSERT_TRUE(static_cast<bool>(Merged)) << llvm::toString(Merged.takeError());
  ASSERT_EQ(Merged->Records.size(), 2u);
  EXPECT_EQ(Merged->Records[0].Origin,
            MachOCompactUnwindRecordOrigin::Original);
  EXPECT_EQ(Merged->Records[1].Origin,
            MachOCompactUnwindRecordOrigin::Generated);
}

TEST(MachOCompactUnwindMerge,
     SameVAInPlaceReplacesExactSourceAndStopsShorterRecipe) {
  const auto Original = makeRawOriginal(
      {{0x100, macho_unwind::kARM64ModeFrame, std::nullopt},
       {0x180, macho_unwind::kARM64ModeFrameless, std::nullopt}},
      0x200);
  const auto UnboundGenerated = makeGeneratedRecords({makeGeneratedRecord(
      kImageBase + 0x100, kImageBase + 0x160, macho_unwind::kARM64ModeDwarf)});
  auto Receipt = makeEHFrameReceipt(kImageBase + 0x100, kImageBase + 0x160,
                                    "_generated_0", 1);
  ASSERT_TRUE(static_cast<bool>(Receipt))
      << llvm::toString(Receipt.takeError());
  const uint32_t FDEOffset = static_cast<uint32_t>(
      Receipt->installedFDEs().front().RecordVA - Receipt->region()->SectionVA);
  auto Generated = bindMachOCompactUnwindDwarfFDEs(UnboundGenerated, *Receipt);
  ASSERT_TRUE(static_cast<bool>(Generated))
      << llvm::toString(Generated.takeError());
  const MachOCompactUnwindRangeMapping Mapping = makeMapping(
      0x100, 0x180, 0x100, 0x160, MachOCompactUnwindRangeMode::SameVAInPlace);

  auto Merged = mergeMachOCompactUnwind(Arch::AArch64, kImageBase, Original,
                                        *Generated, {Mapping});
  ASSERT_TRUE(static_cast<bool>(Merged)) << llvm::toString(Merged.takeError());
  ASSERT_EQ(Merged->Records.size(), 3u);
  EXPECT_EQ(Merged->Records[0].FunctionRVA, 0x100u);
  EXPECT_EQ(Merged->Records[0].FunctionEndRVA, 0x160u);
  EXPECT_EQ(Merged->Records[0].Origin,
            MachOCompactUnwindRecordOrigin::Generated);
  EXPECT_EQ(Merged->Records[0].Encoding & macho_unwind::kDwarfSectionOffsetMask,
            FDEOffset);
  EXPECT_EQ(Merged->Records[1].FunctionRVA, 0x160u);
  EXPECT_EQ(Merged->Records[1].FunctionEndRVA, 0x180u);
  EXPECT_EQ(Merged->Records[1].Encoding, 0u);
  EXPECT_EQ(Merged->Records[1].Origin,
            MachOCompactUnwindRecordOrigin::GapBoundary);
  EXPECT_EQ(Merged->Records[2].FunctionRVA, 0x180u);
  EXPECT_EQ(Merged->Records[2].Origin,
            MachOCompactUnwindRecordOrigin::Original);
}

TEST(MachOCompactUnwindMerge, RejectsUnboundGeneratedDwarfFDE) {
  const auto Original = makeRawOriginal(
      {{0x100, macho_unwind::kARM64ModeFrame, std::nullopt}}, 0x180);
  const auto Generated = makeGeneratedRecords({makeGeneratedRecord(
      kImageBase + 0x100, kImageBase + 0x160, macho_unwind::kARM64ModeDwarf)});
  const MachOCompactUnwindRangeMapping Mapping = makeMapping(
      0x100, 0x180, 0x100, 0x160, MachOCompactUnwindRangeMode::SameVAInPlace);

  expectMergeFailure(mergeMachOCompactUnwind(Arch::AArch64, kImageBase,
                                             Original, Generated, {Mapping}),
                     MergeFailure::MissingDwarfFDEOffset,
                     MachOCompactUnwindMergeInputKind::GeneratedRecord, 0);
}

TEST(MachOCompactUnwindMerge, SupportsThreeExactPersonalitySlots) {
  const auto Original = makeRawOriginal(
      {{0x100,
        macho_unwind::kARM64ModeFrame | (1u << macho_unwind::kPersonalityShift),
        std::nullopt},
       {0x200,
        macho_unwind::kARM64ModeFrame | (2u << macho_unwind::kPersonalityShift),
        std::nullopt},
       {0x300,
        macho_unwind::kARM64ModeFrame | (3u << macho_unwind::kPersonalityShift),
        std::nullopt}},
      0x400, {0x1000, 0x2000, 0x3000});

  auto Merged = mergeMachOCompactUnwind(Arch::AArch64, kImageBase, Original,
                                        makeGeneratedRecords({}), {});
  ASSERT_TRUE(static_cast<bool>(Merged)) << llvm::toString(Merged.takeError());
  EXPECT_EQ(Merged->PersonalitySlotRVAs,
            (std::vector<uint32_t>{0x1000, 0x2000, 0x3000}));
  ASSERT_EQ(Merged->Records.size(), 3u);
  for (size_t I = 0; I < Merged->Records.size(); ++I) {
    EXPECT_EQ((Merged->Records[I].Encoding & macho_unwind::kPersonalityMask) >>
                  macho_unwind::kPersonalityShift,
              I + 1);
  }
}

TEST(MachOCompactUnwindMerge, RejectsFourthExactPersonalitySlot) {
  const auto Original = makeRawOriginal(
      {{0x100,
        macho_unwind::kARM64ModeFrame | (1u << macho_unwind::kPersonalityShift),
        std::nullopt},
       {0x200,
        macho_unwind::kARM64ModeFrame | (2u << macho_unwind::kPersonalityShift),
        std::nullopt},
       {0x300,
        macho_unwind::kARM64ModeFrame | (3u << macho_unwind::kPersonalityShift),
        std::nullopt}},
      0x400, {0x1000, 0x2000, 0x3000});
  const auto Generated = makeGeneratedRecords(
      {makeGeneratedRecord(kImageBase + 0x500, kImageBase + 0x520,
                           macho_unwind::kARM64ModeFrame, 0x4000)});
  const MachOCompactUnwindRangeMapping Mapping = makeMapping(
      0x100, 0x200, 0x500, 0x520, MachOCompactUnwindRangeMode::NewSegment);

  expectMergeFailure(mergeMachOCompactUnwind(Arch::AArch64, kImageBase,
                                             Original, Generated, {Mapping}),
                     MergeFailure::TooManyPersonalities,
                     MachOCompactUnwindMergeInputKind::MergedRecord);
}

TEST(MachOCompactUnwindMerge, RejectsPartialAndAmbiguousSourceMaps) {
  const auto Original = makeRawOriginal(
      {{0x100, macho_unwind::kARM64ModeFrame, std::nullopt}}, 0x180);
  const auto Generated = makeGeneratedRecords(
      {makeGeneratedRecord(kImageBase + 0x500, kImageBase + 0x520,
                           macho_unwind::kARM64ModeFrame, std::nullopt,
                           std::nullopt, 0),
       makeGeneratedRecord(kImageBase + 0x600, kImageBase + 0x620,
                           macho_unwind::kARM64ModeFrame, std::nullopt,
                           std::nullopt, 1)});

  const MachOCompactUnwindRangeMapping Partial = makeMapping(
      0x110, 0x180, 0x500, 0x520, MachOCompactUnwindRangeMode::NewSegment);
  expectMergeFailure(mergeMachOCompactUnwind(Arch::AArch64, kImageBase,
                                             Original, Generated, {Partial}),
                     MergeFailure::SourceRangeNotExact,
                     MachOCompactUnwindMergeInputKind::RangeMapping, 0);

  const MachOCompactUnwindRangeMapping First = makeMapping(
      0x100, 0x180, 0x500, 0x520, MachOCompactUnwindRangeMode::NewSegment);
  const MachOCompactUnwindRangeMapping Second = makeMapping(
      0x100, 0x180, 0x600, 0x620, MachOCompactUnwindRangeMode::NewSegment, 2,
      "_generated_1", kImageBase + 0x600);
  const std::array<MachOCompactUnwindRangeMapping, 2> Mappings = {First,
                                                                  Second};
  expectMergeFailure(mergeMachOCompactUnwind(Arch::AArch64, kImageBase,
                                             Original, Generated, Mappings),
                     MergeFailure::CrossOwnerSourceReuse,
                     MachOCompactUnwindMergeInputKind::RangeMapping, 1);
}

TEST(MachOCompactUnwindMerge,
     RejectsMissingDuplicateDanglingAndMismatchedFragmentIdentities) {
  const auto Original = makeRawOriginal(
      {{0x100, macho_unwind::kARM64ModeFrame, std::nullopt}}, 0x180);
  const MachOCompactUnwindRecords Generated = makeGeneratedRecords(
      {makeGeneratedRecord(kImageBase + 0x500, kImageBase + 0x520,
                           macho_unwind::kARM64ModeFrame)});
  const MachOCompactUnwindRangeMapping Valid = makeMapping(
      0x100, 0x180, 0x500, 0x520, MachOCompactUnwindRangeMode::NewSegment);

  MachOCompactUnwindRangeMapping Missing = Valid;
  Missing.FunctionRangeId = 0;
  expectMergeFailure(mergeMachOCompactUnwind(Arch::AArch64, kImageBase,
                                             Original, Generated, {Missing}),
                     MergeFailure::MissingFunctionRangeId,
                     MachOCompactUnwindMergeInputKind::RangeMapping, 0);

  expectMergeFailure(mergeMachOCompactUnwind(Arch::AArch64, kImageBase,
                                             Original, Generated,
                                             {Valid, Valid}),
                     MergeFailure::DuplicateFunctionRangeId,
                     MachOCompactUnwindMergeInputKind::RangeMapping, 1);

  MachOCompactUnwindRangeMapping Dangling = Valid;
  Dangling.FunctionRangeId = 99;
  expectMergeFailure(mergeMachOCompactUnwind(Arch::AArch64, kImageBase,
                                             Original, Generated, {Dangling}),
                     MergeFailure::DanglingFunctionRangeId,
                     MachOCompactUnwindMergeInputKind::RangeMapping, 0);

  MachOCompactUnwindRangeMapping WrongOwner = Valid;
  WrongOwner.OwnerSymbol = "_different_owner";
  expectMergeFailure(mergeMachOCompactUnwind(Arch::AArch64, kImageBase,
                                             Original, Generated, {WrongOwner}),
                     MergeFailure::FunctionRangeIdentityMismatch,
                     MachOCompactUnwindMergeInputKind::RangeMapping, 0);

  MachOCompactUnwindRangeMapping WrongBoundary = Valid;
  ++WrongBoundary.DestinationEndVA;
  expectMergeFailure(mergeMachOCompactUnwind(Arch::AArch64, kImageBase,
                                             Original, Generated,
                                             {WrongBoundary}),
                     MergeFailure::DestinationRangeNotExact,
                     MachOCompactUnwindMergeInputKind::RangeMapping, 0);

  MachOCompactUnwindRecords DuplicateGenerated = makeGeneratedRecords(
      {makeGeneratedRecord(kImageBase + 0x500, kImageBase + 0x520,
                           macho_unwind::kARM64ModeFrame),
       makeGeneratedRecord(kImageBase + 0x540, kImageBase + 0x560,
                           macho_unwind::kARM64ModeFrameless, std::nullopt,
                           std::nullopt, 1)});
  DuplicateGenerated.Records[1].FunctionRangeId =
      DuplicateGenerated.Records[0].FunctionRangeId;
  expectMergeFailure(mergeMachOCompactUnwind(Arch::AArch64, kImageBase,
                                             Original, DuplicateGenerated, {}),
                     MergeFailure::DuplicateFunctionRangeId,
                     MachOCompactUnwindMergeInputKind::GeneratedRecord, 1);
}

TEST(MachOCompactUnwindMerge, RejectsDestinationOverlapWithPreservedSource) {
  const auto Original = makeRawOriginal(
      {{0x100, macho_unwind::kARM64ModeFrame, std::nullopt},
       {0x180, macho_unwind::kARM64ModeFrameless, std::nullopt}},
      0x240);
  const auto Generated = makeGeneratedRecords({makeGeneratedRecord(
      kImageBase + 0x190, kImageBase + 0x1b0, macho_unwind::kARM64ModeFrame)});
  const MachOCompactUnwindRangeMapping Mapping = makeMapping(
      0x100, 0x180, 0x190, 0x1b0, MachOCompactUnwindRangeMode::NewSegment);

  expectMergeFailure(mergeMachOCompactUnwind(Arch::AArch64, kImageBase,
                                             Original, Generated, {Mapping}),
                     MergeFailure::OverlappingMergedRanges,
                     MachOCompactUnwindMergeInputKind::MergedRecord);
}

TEST(MachOCompactUnwindMerge, RejectsArchitectureAndLSDAFieldMismatch) {
  const auto WrongEncoding = makeRawOriginal(
      {{0x100, macho_unwind::kX86_64ModeRBPFrame, std::nullopt}}, 0x140);
  expectMergeFailure(mergeMachOCompactUnwind(Arch::AArch64, kImageBase,
                                             WrongEncoding,
                                             makeGeneratedRecords({}), {}),
                     MergeFailure::UnsupportedEncoding,
                     MachOCompactUnwindMergeInputKind::OriginalRecord, 0);

  const auto Original = makeRawOriginal(
      {{0x100, macho_unwind::kARM64ModeFrame, std::nullopt}}, 0x140);
  const auto MissingLSDA = makeGeneratedRecords({makeGeneratedRecord(
      kImageBase + 0x500, kImageBase + 0x520,
      macho_unwind::kARM64ModeFrame | macho_unwind::kHasLSDA)});
  expectMergeFailure(mergeMachOCompactUnwind(Arch::AArch64, kImageBase,
                                             Original, MissingLSDA, {}),
                     MergeFailure::LSDAEncodingMismatch,
                     MachOCompactUnwindMergeInputKind::GeneratedRecord, 0);

  auto UnexpectedRecord = makeGeneratedRecord(
      kImageBase + 0x500, kImageBase + 0x520, macho_unwind::kARM64ModeFrame,
      std::nullopt, kImageBase + 0x900);
  UnexpectedRecord.Encoding &= ~macho_unwind::kHasLSDA;
  const auto UnexpectedLSDA = makeGeneratedRecords({UnexpectedRecord});
  expectMergeFailure(mergeMachOCompactUnwind(Arch::AArch64, kImageBase,
                                             Original, UnexpectedLSDA, {}),
                     MergeFailure::LSDAEncodingMismatch,
                     MachOCompactUnwindMergeInputKind::GeneratedRecord, 0);
}

TEST(MachOCompactUnwindMerge, RejectsGeneratedByteOrderMismatch) {
  const auto Original = makeRawOriginal(
      {{0x100, macho_unwind::kARM64ModeFrame, std::nullopt}}, 0x140);
  auto Generated = makeGeneratedRecords({});
  Generated.ByteOrder = llvm::endianness::big;

  expectMergeFailure(mergeMachOCompactUnwind(Arch::AArch64, kImageBase,
                                             Original, Generated, {}),
                     MergeFailure::ByteOrderMismatch);
}

TEST(MachOCompactUnwindMerge, RejectsDecodedStateChangedAfterStrictParse) {
  auto Original = makeRawOriginal(
      {{0x100, macho_unwind::kARM64ModeFrame, std::nullopt}}, 0x140);
  Original.Pages.front().RegularEntries.front().Encoding =
      macho_unwind::kARM64ModeFrameless;

  expectMergeFailure(mergeMachOCompactUnwind(Arch::AArch64, kImageBase,
                                             Original, makeGeneratedRecords({}),
                                             {}),
                     MergeFailure::InvalidOriginalTable);
}

//===----------------------------------------------------------------------===//
// Deterministic final regular-page encoding
//===----------------------------------------------------------------------===//

TEST(MachOCompactUnwindEncode,
     EmitsStableMultiPageLayoutAndRoundTripsEveryRecipe) {
  auto Input = makeMergedResult(Arch::AArch64, 513);
  Input.PersonalitySlotRVAs = {0x3000, 0x4000};
  Input.Records[0].Encoding |=
      macho_unwind::kHasLSDA | (1u << macho_unwind::kPersonalityShift);
  Input.Records[0].PersonalitySlotRVA = 0x3000;
  Input.Records[0].LSDARVA = 0x9000;
  Input.Records[510].Encoding |= 2u << macho_unwind::kPersonalityShift;
  Input.Records[510].PersonalitySlotRVA = 0x4000;
  Input.Records[511].Encoding |= macho_unwind::kHasLSDA;
  Input.Records[511].LSDARVA = 0xa000;
  const MachOCompactUnwindMergeResult Before = Input;

  auto Encoded =
      encodeMachOCompactUnwindRegular(Input, llvm::endianness::little);
  ASSERT_TRUE(static_cast<bool>(Encoded))
      << llvm::toString(Encoded.takeError());
  EXPECT_EQ(Input, Before);
  auto EncodedAgain =
      encodeMachOCompactUnwindRegular(Input, llvm::endianness::little);
  ASSERT_TRUE(static_cast<bool>(EncodedAgain))
      << llvm::toString(EncodedAgain.takeError());
  EXPECT_EQ(*EncodedAgain, *Encoded);

  // 28-byte header + two personalities + three index rows + two LSDAs,
  // followed by two fixed-size regular pages.
  constexpr uint32_t IndexOffset = 36;
  constexpr uint32_t LSDAOffset = 72;
  constexpr uint32_t PagesOffset = 88;
  EXPECT_EQ(Encoded->size(), PagesOffset + 2 * 4096u);
  auto Raw = macho_unwind::parseCompactUnwindRaw(*Encoded);
  ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
  EXPECT_EQ(Raw->Header.CommonEncodingsSectionOffset, 28u);
  EXPECT_EQ(Raw->Header.CommonEncodingsCount, 0u);
  EXPECT_EQ(Raw->Header.PersonalityArraySectionOffset, 28u);
  EXPECT_EQ(Raw->Header.IndexSectionOffset, IndexOffset);
  EXPECT_EQ(Raw->Header.IndexCount, 3u);
  EXPECT_EQ(Raw->PersonalitySlotOffsets,
            (std::vector<uint32_t>{0x3000, 0x4000}));

  ASSERT_EQ(Raw->Index.size(), 3u);
  EXPECT_EQ(Raw->Index[0].SecondLevelPageSectionOffset, PagesOffset);
  EXPECT_EQ(Raw->Index[0].LSDAIndexArraySectionOffset, LSDAOffset);
  EXPECT_EQ(Raw->Index[1].SecondLevelPageSectionOffset, PagesOffset + 4096u);
  EXPECT_EQ(Raw->Index[1].LSDAIndexArraySectionOffset, LSDAOffset + 8u);
  EXPECT_EQ(Raw->Index[2].FunctionOffset, Input.TerminalFunctionRVA);
  EXPECT_EQ(Raw->Index[2].SecondLevelPageSectionOffset, 0u);
  EXPECT_EQ(Raw->Index[2].LSDAIndexArraySectionOffset, PagesOffset);

  ASSERT_EQ(Raw->Pages.size(), 2u);
  EXPECT_EQ(Raw->Pages[0].Kind, macho_unwind::kSecondLevelRegular);
  EXPECT_EQ(Raw->Pages[0].EntryPageOffset, 8u);
  EXPECT_EQ(Raw->Pages[0].EntryCount, 511u);
  EXPECT_EQ(Raw->Pages[1].Kind, macho_unwind::kSecondLevelRegular);
  EXPECT_EQ(Raw->Pages[1].EntryPageOffset, 8u);
  EXPECT_EQ(Raw->Pages[1].EntryCount, 2u);
  EXPECT_EQ((*Encoded)[PagesOffset + 4096u + 24u], 0u);

  size_t RecordIndex = 0;
  for (const macho_unwind::CompactUnwindRawPage &Page : Raw->Pages) {
    for (const macho_unwind::CompactUnwindRawRegularEntry &Entry :
         Page.RegularEntries) {
      ASSERT_LT(RecordIndex, Input.Records.size());
      EXPECT_EQ(Entry.FunctionOffset, Input.Records[RecordIndex].FunctionRVA);
      EXPECT_EQ(Entry.Encoding, Input.Records[RecordIndex].Encoding);
      const uint32_t RecoveredEnd =
          RecordIndex + 1 == Input.Records.size()
              ? Input.TerminalFunctionRVA
              : Input.Records[RecordIndex + 1].FunctionRVA;
      EXPECT_EQ(RecoveredEnd, Input.Records[RecordIndex].FunctionEndRVA);
      ++RecordIndex;
    }
  }
  EXPECT_EQ(RecordIndex, Input.Records.size());
  ASSERT_EQ(Raw->LSDAEntries.size(), 2u);
  EXPECT_EQ(Raw->LSDAEntries[0].FunctionOffset, Input.Records[0].FunctionRVA);
  EXPECT_EQ(Raw->LSDAEntries[0].LSDAOffset, 0x9000u);
  EXPECT_EQ(Raw->LSDAEntries[1].FunctionOffset, Input.Records[511].FunctionRVA);
  EXPECT_EQ(Raw->LSDAEntries[1].LSDAOffset, 0xa000u);
}

TEST(MachOCompactUnwindEncode, AcceptsMergeOutputWithoutChangingItsInput) {
  const uint32_t FirstEncoding = macho_unwind::kARM64ModeFrame |
                                 macho_unwind::kHasLSDA |
                                 (1u << macho_unwind::kPersonalityShift);
  const auto Original = makeRawOriginal(
      {{0x100, FirstEncoding, 0x900},
       {0x140, macho_unwind::kARM64ModeFrameless, std::nullopt}},
      0x180, {0x3000});
  auto Merged = mergeMachOCompactUnwind(Arch::AArch64, kImageBase, Original,
                                        makeGeneratedRecords({}), {});
  ASSERT_TRUE(static_cast<bool>(Merged)) << llvm::toString(Merged.takeError());
  const MachOCompactUnwindMergeResult Before = *Merged;

  auto Encoded =
      encodeMachOCompactUnwindRegular(*Merged, llvm::endianness::little);
  ASSERT_TRUE(static_cast<bool>(Encoded))
      << llvm::toString(Encoded.takeError());
  EXPECT_EQ(*Merged, Before);
  auto Raw = macho_unwind::parseCompactUnwindRaw(*Encoded);
  ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
  ASSERT_EQ(Raw->Pages.size(), 1u);
  ASSERT_EQ(Raw->Pages.front().RegularEntries.size(), Merged->Records.size());
  EXPECT_EQ(Raw->PersonalitySlotOffsets, Merged->PersonalitySlotRVAs);
  EXPECT_EQ(Raw->Index.back().FunctionOffset, Merged->TerminalFunctionRVA);
}

TEST(MachOCompactUnwindEncode,
     RoundTripsOriginalDwarfWithoutOffsetHintPersonalityAndLSDA) {
  const uint32_t OriginalEncoding = macho_unwind::kARM64ModeDwarf |
                                    macho_unwind::kHasLSDA |
                                    (1u << macho_unwind::kPersonalityShift);
  const auto Original =
      makeRawOriginal({{0x100, OriginalEncoding, 0x900}}, 0x140, {0x4000});
  auto Merged = mergeMachOCompactUnwind(Arch::AArch64, kImageBase, Original,
                                        makeGeneratedRecords({}), {});
  ASSERT_TRUE(static_cast<bool>(Merged)) << llvm::toString(Merged.takeError());

  auto Encoded =
      encodeMachOCompactUnwindRegular(*Merged, llvm::endianness::little);
  ASSERT_TRUE(static_cast<bool>(Encoded))
      << llvm::toString(Encoded.takeError());
  auto Raw = macho_unwind::parseCompactUnwindRaw(*Encoded);
  ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
  ASSERT_EQ(Raw->Pages.size(), 1u);
  ASSERT_EQ(Raw->Pages.front().RegularEntries.size(), 1u);
  const macho_unwind::CompactUnwindRawRegularEntry &Entry =
      Raw->Pages.front().RegularEntries.front();
  EXPECT_EQ(Entry.FunctionOffset, 0x100u);
  EXPECT_EQ(Entry.Encoding & macho_unwind::kModeMask,
            macho_unwind::kARM64ModeDwarf);
  EXPECT_EQ(Entry.Encoding & macho_unwind::kDwarfSectionOffsetMask, 0u);
  EXPECT_EQ(Entry.Encoding & macho_unwind::kPersonalityMask,
            1u << macho_unwind::kPersonalityShift);
  EXPECT_EQ(Raw->PersonalitySlotOffsets, (std::vector<uint32_t>{0x4000}));
  ASSERT_EQ(Raw->LSDAEntries.size(), 1u);
  EXPECT_EQ(Raw->LSDAEntries.front().FunctionOffset, 0x100u);
  EXPECT_EQ(Raw->LSDAEntries.front().LSDAOffset, 0x900u);
}

TEST(MachOCompactUnwindEncode, MergeRetainsExactBoundaryAcrossZeroRecipeGap) {
  const auto Original = makeRawOriginal({{0x100, 0, std::nullopt}}, 0x140);
  const auto Generated = makeGeneratedRecords({makeGeneratedRecord(
      kImageBase + 0x500, kImageBase + 0x520, macho_unwind::kARM64ModeFrame)});
  const MachOCompactUnwindRangeMapping Mapping = makeMapping(
      0x100, 0x140, 0x500, 0x520, MachOCompactUnwindRangeMode::NewSegment);
  auto Merged = mergeMachOCompactUnwind(Arch::AArch64, kImageBase, Original,
                                        Generated, {Mapping});
  ASSERT_TRUE(static_cast<bool>(Merged)) << llvm::toString(Merged.takeError());
  ASSERT_EQ(Merged->Records.size(), 3u);
  EXPECT_EQ(Merged->Records[0].FunctionEndRVA, Merged->Records[1].FunctionRVA);
  EXPECT_EQ(Merged->Records[1].Encoding, 0u);
  EXPECT_EQ(Merged->Records[1].Origin,
            MachOCompactUnwindRecordOrigin::GapBoundary);
  EXPECT_EQ(Merged->Records[1].FunctionEndRVA, Merged->Records[2].FunctionRVA);

  auto Encoded =
      encodeMachOCompactUnwindRegular(*Merged, llvm::endianness::little);
  ASSERT_TRUE(static_cast<bool>(Encoded))
      << llvm::toString(Encoded.takeError());
  auto Raw = macho_unwind::parseCompactUnwindRaw(*Encoded);
  ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
  ASSERT_EQ(Raw->Pages.size(), 1u);
  ASSERT_EQ(Raw->Pages.front().RegularEntries.size(), 3u);
  EXPECT_EQ(Raw->Pages.front().RegularEntries[1].FunctionOffset, 0x140u);
  EXPECT_EQ(Raw->Pages.front().RegularEntries[1].Encoding, 0u);
}

TEST(MachOCompactUnwindEncode, SupportsCurrentLittleEndianMachOTargets) {
  const std::array<Arch, 4> Architectures = {Arch::X86, Arch::X64, Arch::ARM,
                                             Arch::AArch64};
  for (Arch TargetArch : Architectures) {
    SCOPED_TRACE(static_cast<unsigned>(TargetArch));
    const MachOCompactUnwindMergeResult Input = makeMergedResult(TargetArch, 1);
    auto Encoded =
        encodeMachOCompactUnwindRegular(Input, llvm::endianness::little);
    ASSERT_TRUE(static_cast<bool>(Encoded))
        << llvm::toString(Encoded.takeError());
    auto Raw = macho_unwind::parseCompactUnwindRaw(*Encoded);
    ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
    ASSERT_EQ(Raw->Pages.size(), 1u);
    ASSERT_EQ(Raw->Pages.front().RegularEntries.size(), 1u);
    EXPECT_EQ(Raw->Pages.front().RegularEntries.front().Encoding,
              Input.Records.front().Encoding);
  }
}

TEST(MachOCompactUnwindEncode, RejectsNoncanonicalInputWithTypedFailures) {
  const auto Valid = makeMergedResult(Arch::AArch64, 2);

  auto WrongArch = Valid;
  WrongArch.TargetArch = Arch::Unknown;
  expectEncodeFailure(
      encodeMachOCompactUnwindRegular(WrongArch, llvm::endianness::little),
      EncodeFailure::UnsupportedArchitecture);
  expectEncodeFailure(
      encodeMachOCompactUnwindRegular(Valid, llvm::endianness::big),
      EncodeFailure::UnsupportedEndianness);

  MachOCompactUnwindMergeResult Empty;
  Empty.TargetArch = Arch::AArch64;
  expectEncodeFailure(
      encodeMachOCompactUnwindRegular(Empty, llvm::endianness::little),
      EncodeFailure::EmptyRecords);

  auto TooManyPersonalities = Valid;
  TooManyPersonalities.PersonalitySlotRVAs = {1, 2, 3, 4};
  expectEncodeFailure(encodeMachOCompactUnwindRegular(TooManyPersonalities,
                                                      llvm::endianness::little),
                      EncodeFailure::TooManyPersonalities);

  auto DuplicatePersonality = Valid;
  DuplicatePersonality.PersonalitySlotRVAs = {0x3000, 0x3000};
  expectEncodeFailure(encodeMachOCompactUnwindRegular(DuplicatePersonality,
                                                      llvm::endianness::little),
                      EncodeFailure::DuplicatePersonalitySlot,
                      MachOCompactUnwindEncodeInputKind::PersonalitySlot, 1);

  auto Gap = Valid;
  Gap.Records[0].FunctionEndRVA -= 4;
  expectEncodeFailure(
      encodeMachOCompactUnwindRegular(Gap, llvm::endianness::little),
      EncodeFailure::NonContiguousRecords,
      MachOCompactUnwindEncodeInputKind::Record, 0);

  auto Overlap = Valid;
  Overlap.Records[0].FunctionEndRVA += 4;
  expectEncodeFailure(
      encodeMachOCompactUnwindRegular(Overlap, llvm::endianness::little),
      EncodeFailure::UnsortedOrOverlappingRecords,
      MachOCompactUnwindEncodeInputKind::Record, 1);

  auto WrongTerminal = Valid;
  WrongTerminal.TerminalFunctionRVA += 0x10;
  expectEncodeFailure(
      encodeMachOCompactUnwindRegular(WrongTerminal, llvm::endianness::little),
      EncodeFailure::InvalidTerminalBoundary,
      MachOCompactUnwindEncodeInputKind::Record, 1);

  auto UnsupportedEncoding = Valid;
  UnsupportedEncoding.Records[0].Encoding = macho_unwind::kX86_64ModeRBPFrame;
  expectEncodeFailure(encodeMachOCompactUnwindRegular(UnsupportedEncoding,
                                                      llvm::endianness::little),
                      EncodeFailure::UnsupportedEncoding,
                      MachOCompactUnwindEncodeInputKind::Record, 0);

  auto UnboundGeneratedDwarf = Valid;
  UnboundGeneratedDwarf.Records[0].Encoding = macho_unwind::kARM64ModeDwarf;
  expectEncodeFailure(encodeMachOCompactUnwindRegular(UnboundGeneratedDwarf,
                                                      llvm::endianness::little),
                      EncodeFailure::UnsupportedEncoding,
                      MachOCompactUnwindEncodeInputKind::Record, 0);

  auto PersonalityOutOfRange = Valid;
  PersonalityOutOfRange.Records[0].Encoding |=
      1u << macho_unwind::kPersonalityShift;
  PersonalityOutOfRange.Records[0].PersonalitySlotRVA = 0x3000;
  expectEncodeFailure(encodeMachOCompactUnwindRegular(PersonalityOutOfRange,
                                                      llvm::endianness::little),
                      EncodeFailure::PersonalityIndexOutOfRange,
                      MachOCompactUnwindEncodeInputKind::Record, 0);

  auto WrongPersonality = PersonalityOutOfRange;
  WrongPersonality.PersonalitySlotRVAs = {0x4000};
  expectEncodeFailure(encodeMachOCompactUnwindRegular(WrongPersonality,
                                                      llvm::endianness::little),
                      EncodeFailure::PersonalityEncodingMismatch,
                      MachOCompactUnwindEncodeInputKind::Record, 0);

  auto MissingLSDA = Valid;
  MissingLSDA.Records[0].Encoding |= macho_unwind::kHasLSDA;
  expectEncodeFailure(
      encodeMachOCompactUnwindRegular(MissingLSDA, llvm::endianness::little),
      EncodeFailure::LSDAEncodingMismatch,
      MachOCompactUnwindEncodeInputKind::Record, 0);
}

//===----------------------------------------------------------------------===//
// Transactional final-section installation
//===----------------------------------------------------------------------===//

TEST(MachOCompactUnwindInstall, RewritesAnExactlySizedSectionTransactionally) {
  const CompactUnwindInstallInputs Inputs = makeCompactUnwindInstallInputs();
  ASSERT_FALSE(Inputs.Encoded.empty());
  std::vector<uint8_t> Binary =
      makeInstallableUnwindInfoMachO(Inputs.Original, Inputs.Encoded.size());

  auto Plan = prepareMachOCompactUnwindInstall(
      Binary, Arch::AArch64, Inputs.Generated, Inputs.Mappings,
      llvm::endianness::little);
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());
  ASSERT_EQ(Plan->disposition(),
            MachOCompactUnwindInstallDisposition::RewrittenInPlace);
  ASSERT_TRUE(Plan->expectedRegion().has_value());
  EXPECT_TRUE(std::equal(Plan->encodedBytes().begin(),
                         Plan->encodedBytes().end(), Inputs.Encoded.begin(),
                         Inputs.Encoded.end()));
  const uint64_t HeaderSizeBefore =
      reinterpret_cast<const llvm::MachO::section_64 *>(
          Binary.data() + Plan->expectedRegion()->SectionHeaderOff)
          ->size;

  auto Receipt = applyMachOCompactUnwindInstall(Binary, *Plan);
  ASSERT_TRUE(static_cast<bool>(Receipt))
      << llvm::toString(Receipt.takeError());
  EXPECT_EQ(Receipt->Disposition,
            MachOCompactUnwindInstallDisposition::RewrittenInPlace);
  EXPECT_EQ(Receipt->EncodedSize, Inputs.Encoded.size());
  EXPECT_EQ(Receipt->Capacity, Inputs.Encoded.size());
  EXPECT_EQ(Receipt->ClearedTailSize, 0u);
  EXPECT_EQ(Receipt->OriginalRecordCount, 1u);
  EXPECT_EQ(Receipt->GeneratedRecordCount, 1u);
  EXPECT_EQ(Receipt->FinalRecordCount, Inputs.Merged.Records.size());
  ASSERT_TRUE(Receipt->Region.has_value());
  EXPECT_EQ(reinterpret_cast<const llvm::MachO::section_64 *>(
                Binary.data() + Receipt->Region->SectionHeaderOff)
                ->size,
            HeaderSizeBefore);
  EXPECT_TRUE(std::equal(
      Inputs.Encoded.begin(), Inputs.Encoded.end(),
      Binary.begin() + static_cast<size_t>(Receipt->Region->SectionFileOff)));
}

TEST(MachOCompactUnwindInstall,
     RoundTripsOriginalDwarfWithoutOffsetHintPersonalityAndLSDA) {
  const uint32_t OriginalEncoding = macho_unwind::kARM64ModeDwarf |
                                    macho_unwind::kHasLSDA |
                                    (1u << macho_unwind::kPersonalityShift);
  const auto Original =
      makeRawOriginal({{0x100, OriginalEncoding, 0x900},
                       {0x140, macho_unwind::kARM64ModeFrame, std::nullopt}},
                      0x180, {0x4000});
  const auto Generated = makeGeneratedRecords(
      {makeGeneratedRecord(kImageBase + 0x500, kImageBase + 0x520,
                           macho_unwind::kARM64ModeFrameless)});
  const MachOCompactUnwindRangeMapping Mapping = makeMapping(
      0x140, 0x180, 0x500, 0x520, MachOCompactUnwindRangeMode::NewSegment);

  auto Merged = mergeMachOCompactUnwind(Arch::AArch64, kImageBase, Original,
                                        Generated, {Mapping});
  ASSERT_TRUE(static_cast<bool>(Merged)) << llvm::toString(Merged.takeError());
  auto Encoded =
      encodeMachOCompactUnwindRegular(*Merged, llvm::endianness::little);
  ASSERT_TRUE(static_cast<bool>(Encoded))
      << llvm::toString(Encoded.takeError());
  constexpr size_t ExtraCapacity = 32;
  std::vector<uint8_t> Binary =
      makeInstallableUnwindInfoMachO(Original, Encoded->size() + ExtraCapacity);

  auto Plan = prepareMachOCompactUnwindInstall(
      Binary, Arch::AArch64, Generated, {Mapping}, llvm::endianness::little);
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());
  auto Receipt = applyMachOCompactUnwindInstall(Binary, *Plan);
  ASSERT_TRUE(static_cast<bool>(Receipt))
      << llvm::toString(Receipt.takeError());
  EXPECT_EQ(Receipt->OriginalRecordCount, 2u);
  EXPECT_EQ(Receipt->GeneratedRecordCount, 1u);
  EXPECT_EQ(Receipt->FinalRecordCount, Merged->Records.size());
  ASSERT_TRUE(Receipt->Region.has_value());

  const size_t SectionOffset =
      static_cast<size_t>(Receipt->Region->SectionFileOff);
  const size_t SectionSize = static_cast<size_t>(Receipt->Region->SectionSize);
  auto Installed = macho_unwind::parseCompactUnwindRaw(
      llvm::ArrayRef<uint8_t>(Binary).slice(SectionOffset, SectionSize));
  ASSERT_TRUE(static_cast<bool>(Installed))
      << llvm::toString(Installed.takeError());
  EXPECT_EQ(Installed->PersonalitySlotOffsets, (std::vector<uint32_t>{0x4000}));
  ASSERT_EQ(Installed->Pages.size(), 1u);
  const auto &Entries = Installed->Pages.front().RegularEntries;
  const auto Dwarf =
      std::find_if(Entries.begin(), Entries.end(), [](const auto &Entry) {
        return Entry.FunctionOffset == 0x100;
      });
  ASSERT_NE(Dwarf, Entries.end());
  EXPECT_EQ(Dwarf->Encoding & macho_unwind::kModeMask,
            macho_unwind::kARM64ModeDwarf);
  EXPECT_EQ(Dwarf->Encoding & macho_unwind::kDwarfSectionOffsetMask, 0u);
  EXPECT_EQ(Dwarf->Encoding & macho_unwind::kPersonalityMask,
            1u << macho_unwind::kPersonalityShift);
  ASSERT_EQ(Installed->LSDAEntries.size(), 1u);
  EXPECT_EQ(Installed->LSDAEntries.front().FunctionOffset, 0x100u);
  EXPECT_EQ(Installed->LSDAEntries.front().LSDAOffset, 0x900u);
}

TEST(MachOCompactUnwindInstall,
     RewritesPrefixClearsTailAndKeepsDeclaredCapacity) {
  const CompactUnwindInstallInputs Inputs = makeCompactUnwindInstallInputs();
  ASSERT_FALSE(Inputs.Encoded.empty());
  constexpr size_t ExtraCapacity = 47;
  std::vector<uint8_t> Binary = makeInstallableUnwindInfoMachO(
      Inputs.Original, Inputs.Encoded.size() + ExtraCapacity, 0xd7);

  auto Plan = prepareMachOCompactUnwindInstall(
      Binary, Arch::AArch64, Inputs.Generated, Inputs.Mappings,
      llvm::endianness::little);
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());
  ASSERT_TRUE(Plan->expectedRegion().has_value());
  const uint64_t DeclaredSize = Plan->expectedRegion()->SectionSize;
  const size_t SectionOffset =
      static_cast<size_t>(Plan->expectedRegion()->SectionFileOff);
  ASSERT_EQ(Binary[SectionOffset + Inputs.Encoded.size()], 0xd7);

  auto Receipt = applyMachOCompactUnwindInstall(Binary, *Plan);
  ASSERT_TRUE(static_cast<bool>(Receipt))
      << llvm::toString(Receipt.takeError());
  EXPECT_EQ(Receipt->EncodedSize, Inputs.Encoded.size());
  EXPECT_EQ(Receipt->Capacity, DeclaredSize);
  EXPECT_EQ(Receipt->ClearedTailSize, ExtraCapacity);
  EXPECT_TRUE(std::equal(Inputs.Encoded.begin(), Inputs.Encoded.end(),
                         Binary.begin() + SectionOffset));
  EXPECT_TRUE(std::all_of(
      Binary.begin() + SectionOffset + Inputs.Encoded.size(),
      Binary.begin() + SectionOffset + static_cast<size_t>(DeclaredSize),
      [](uint8_t Byte) { return Byte == 0; }));
  EXPECT_EQ(reinterpret_cast<const llvm::MachO::section_64 *>(
                Binary.data() + Plan->expectedRegion()->SectionHeaderOff)
                ->size,
            DeclaredSize);

  auto Installed = findMachOCompactUnwindRegion(Binary);
  ASSERT_TRUE(static_cast<bool>(Installed))
      << llvm::toString(Installed.takeError());
  ASSERT_TRUE(Installed->has_value());
  EXPECT_EQ((*Installed)->SectionSize, DeclaredSize);
  auto Strict =
      macho_unwind::parseCompactUnwindRaw(llvm::ArrayRef<uint8_t>(Binary).slice(
          SectionOffset, static_cast<size_t>(DeclaredSize)));
  ASSERT_TRUE(static_cast<bool>(Strict)) << llvm::toString(Strict.takeError());
}

TEST(MachOCompactUnwindInstall, RejectsOneByteCapacityShortageWithoutMutation) {
  const CompactUnwindInstallInputs Inputs = makeCompactUnwindInstallInputs();
  ASSERT_GT(Inputs.Encoded.size(), Inputs.Original.OriginalBytes.size());
  const size_t Capacity = Inputs.Encoded.size() - 1;
  std::vector<uint8_t> Binary =
      makeInstallableUnwindInfoMachO(Inputs.Original, Capacity);
  const std::vector<uint8_t> Before = Binary;

  expectInstallFailure(prepareMachOCompactUnwindInstall(
                           Binary, Arch::AArch64, Inputs.Generated,
                           Inputs.Mappings, llvm::endianness::little),
                       InstallFailure::InsufficientCapacity,
                       Inputs.Encoded.size(), Capacity);
  EXPECT_EQ(Binary, Before);
}

TEST(MachOCompactUnwindInstall,
     RejectsStaleRegionAndPreimageWithoutFurtherMutation) {
  const CompactUnwindInstallInputs Inputs = makeCompactUnwindInstallInputs();
  const size_t Capacity = Inputs.Encoded.size() + 32;

  std::vector<uint8_t> StaleRegion =
      makeInstallableUnwindInfoMachO(Inputs.Original, Capacity);
  auto RegionPlan = prepareMachOCompactUnwindInstall(
      StaleRegion, Arch::AArch64, Inputs.Generated, Inputs.Mappings,
      llvm::endianness::little);
  ASSERT_TRUE(static_cast<bool>(RegionPlan))
      << llvm::toString(RegionPlan.takeError());
  ASSERT_TRUE(RegionPlan->expectedRegion().has_value());
  auto *SectionHeader = reinterpret_cast<llvm::MachO::section_64 *>(
      StaleRegion.data() + RegionPlan->expectedRegion()->SectionHeaderOff);
  --SectionHeader->size;
  const std::vector<uint8_t> RegionBeforeApply = StaleRegion;
  expectInstallFailure(applyMachOCompactUnwindInstall(StaleRegion, *RegionPlan),
                       InstallFailure::StaleRegion);
  EXPECT_EQ(StaleRegion, RegionBeforeApply);

  std::vector<uint8_t> StalePreimage =
      makeInstallableUnwindInfoMachO(Inputs.Original, Capacity);
  auto PreimagePlan = prepareMachOCompactUnwindInstall(
      StalePreimage, Arch::AArch64, Inputs.Generated, Inputs.Mappings,
      llvm::endianness::little);
  ASSERT_TRUE(static_cast<bool>(PreimagePlan))
      << llvm::toString(PreimagePlan.takeError());
  ASSERT_TRUE(PreimagePlan->expectedRegion().has_value());
  const size_t LastSectionByte =
      static_cast<size_t>(PreimagePlan->expectedRegion()->SectionFileOff +
                          PreimagePlan->expectedRegion()->SectionSize - 1);
  StalePreimage[LastSectionByte] ^= 0xff;
  const std::vector<uint8_t> PreimageBeforeApply = StalePreimage;
  expectInstallFailure(
      applyMachOCompactUnwindInstall(StalePreimage, *PreimagePlan),
      InstallFailure::StalePreimage);
  EXPECT_EQ(StalePreimage, PreimageBeforeApply);
}

TEST(MachOCompactUnwindInstall, EmptyRequestIsAnExactNoOpWithoutASection) {
  std::vector<uint8_t> Binary =
      makeUnwindInfoMachO(/*Is64=*/true, /*IncludeUnwind=*/false);
  const std::vector<uint8_t> Before = Binary;
  const MachOCompactUnwindRecords Empty = makeGeneratedRecords({});

  auto Plan = prepareMachOCompactUnwindInstall(Binary, Arch::AArch64, Empty, {},
                                               llvm::endianness::little);
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());
  EXPECT_EQ(Plan->disposition(),
            MachOCompactUnwindInstallDisposition::Unchanged);
  auto Receipt = applyMachOCompactUnwindInstall(Binary, *Plan);
  ASSERT_TRUE(static_cast<bool>(Receipt))
      << llvm::toString(Receipt.takeError());
  EXPECT_EQ(Receipt->Disposition,
            MachOCompactUnwindInstallDisposition::Unchanged);
  EXPECT_FALSE(Receipt->Region.has_value());
  EXPECT_EQ(Binary, Before);

  const MachOCompactUnwindRangeMapping Unexpected = makeMapping(
      0x100, 0x140, 0x500, 0x520, MachOCompactUnwindRangeMode::NewSegment);
  expectInstallFailure(
      prepareMachOCompactUnwindInstall(Binary, Arch::AArch64, Empty,
                                       {Unexpected}, llvm::endianness::little),
      InstallFailure::UnexpectedMappingsForNoOp);
  EXPECT_EQ(Binary, Before);
}

TEST(MachOCompactUnwindInstall,
     RejectsHeaderCPUArchitectureMismatchWithoutMutation) {
  const CompactUnwindInstallInputs Inputs = makeCompactUnwindInstallInputs();
  const size_t Capacity = Inputs.Encoded.size() + 32;
  std::vector<uint8_t> Binary =
      makeInstallableUnwindInfoMachO(Inputs.Original, Capacity);
  auto *Header = reinterpret_cast<llvm::MachO::mach_header_64 *>(Binary.data());
  Header->cputype = llvm::MachO::CPU_TYPE_X86_64;
  Header->cpusubtype = llvm::MachO::CPU_SUBTYPE_X86_64_ALL;
  const std::vector<uint8_t> Before = Binary;

  expectInstallFailure(prepareMachOCompactUnwindInstall(
                           Binary, Arch::AArch64, Inputs.Generated,
                           Inputs.Mappings, llvm::endianness::little),
                       InstallFailure::ArchitectureMismatch);
  EXPECT_EQ(Binary, Before);
}

TEST(MachOCompactUnwindInstall,
     RevalidatesHeaderCPUArchitectureBeforeApplyingAPlan) {
  const CompactUnwindInstallInputs Inputs = makeCompactUnwindInstallInputs();
  const size_t Capacity = Inputs.Encoded.size() + 32;
  std::vector<uint8_t> Binary =
      makeInstallableUnwindInfoMachO(Inputs.Original, Capacity);
  auto Plan = prepareMachOCompactUnwindInstall(
      Binary, Arch::AArch64, Inputs.Generated, Inputs.Mappings,
      llvm::endianness::little);
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());

  auto *Header = reinterpret_cast<llvm::MachO::mach_header_64 *>(Binary.data());
  Header->cputype = llvm::MachO::CPU_TYPE_X86_64;
  Header->cpusubtype = llvm::MachO::CPU_SUBTYPE_X86_64_ALL;
  const std::vector<uint8_t> BeforeApply = Binary;
  expectInstallFailure(applyMachOCompactUnwindInstall(Binary, *Plan),
                       InstallFailure::ArchitectureMismatch);
  EXPECT_EQ(Binary, BeforeApply);
}

TEST(MachOPatchTransaction,
     RejectsSameArchitectureContextWithDifferentMachHeaderVAWithoutMutation) {
  MachOPatchTransactionFixture Fixture =
      makeMachOPatchTransactionFixture(/*CompactCapacity=*/0x2000);
  ASSERT_FALSE(Fixture.Binary.empty());
  BinaryImage WrongContext = Fixture.Image;
  ASSERT_FALSE(WrongContext.Segments.empty());
  ASSERT_EQ(WrongContext.Segments.front().FileOff, 0u);
  WrongContext.Segments.front().VA += 0x1000;

  llvm::SmallString<128> InputPath;
  llvm::SmallString<128> OutputPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
      "neverd-macho-patch-wrong-context-input", "macho", InputPath));
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
      "neverd-macho-patch-wrong-context-output", "macho", OutputPath));
  llvm::FileRemover RemoveInput(InputPath);
  llvm::FileRemover RemoveOutput(OutputPath);

  const std::vector<uint8_t> OutputSentinel = {0x43, 0x54, 0x58};
  ASSERT_TRUE(writeFixtureFile(InputPath, Fixture.Binary));
  ASSERT_TRUE(writeFixtureFile(OutputPath, OutputSentinel));

  llvm::LLVMContext Context;
  auto Module = makeMachOPatchTransactionModule(Context);
  MachOPatcher Patcher;
  Patcher.setImageContext(&WrongContext);
  testing::internal::CaptureStderr();
  PatchResult Result = Patcher.patch(
      InputPath.str().str(), OutputPath.str().str(), *Module, Arch::AArch64);
  const std::string Diagnostic = testing::internal::GetCapturedStderr();

  EXPECT_FALSE(Result.Success);
  EXPECT_NE(Diagnostic.find("image context does not match input"),
            std::string::npos)
      << Diagnostic;
  EXPECT_EQ(readFixtureFile(OutputPath), OutputSentinel);
  EXPECT_EQ(readFixtureFile(InputPath), Fixture.Binary);
}

TEST(MachOPatchTransaction,
     RejectsSameArchitectureContextWithStaleBytesPointerSectionOrSlotMap) {
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    SCOPED_TRACE(Mutation);
    MachOPatchTransactionFixture Fixture =
        makeMachOPatchTransactionFixture(/*CompactCapacity=*/0x2000);
    ASSERT_FALSE(Fixture.Binary.empty());
    BinaryImage WrongContext = Fixture.Image;
    if (Mutation == 0) {
      ASSERT_FALSE(WrongContext.Raw.empty());
      WrongContext.Raw.back() ^= 1;
    } else if (Mutation == 1) {
      auto Section = llvm::find_if(WrongContext.Sections, [](const auto &S) {
        return S.Name == "__got";
      });
      ASSERT_NE(Section, WrongContext.Sections.end());
      Section->FileOff += 8;
    } else {
      ASSERT_EQ(WrongContext.ImportPtrSlots.size(), 1u);
      const std::string Symbol = WrongContext.ImportPtrSlots.begin()->second;
      WrongContext.ImportPtrSlots.clear();
      WrongContext.ImportPtrSlots[kTransactionPersonalitySlotVA + 8] = Symbol;
    }

    llvm::SmallString<128> InputPath;
    llvm::SmallString<128> OutputPath;
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
        "neverd-macho-patch-stale-context-input", "macho", InputPath));
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
        "neverd-macho-patch-stale-context-output", "macho", OutputPath));
    llvm::FileRemover RemoveInput(InputPath);
    llvm::FileRemover RemoveOutput(OutputPath);

    const std::vector<uint8_t> OutputSentinel = {0x53, 0x54, 0x41, 0x4c, 0x45};
    ASSERT_TRUE(writeFixtureFile(InputPath, Fixture.Binary));
    ASSERT_TRUE(writeFixtureFile(OutputPath, OutputSentinel));

    llvm::LLVMContext Context;
    auto Module = makeMachOPatchTransactionModule(Context);
    MachOPatcher Patcher;
    Patcher.setImageContext(&WrongContext);
    testing::internal::CaptureStderr();
    PatchResult Result = Patcher.patch(
        InputPath.str().str(), OutputPath.str().str(), *Module, Arch::AArch64);
    const std::string Diagnostic = testing::internal::GetCapturedStderr();

    EXPECT_FALSE(Result.Success);
    EXPECT_NE(Diagnostic.find("image context does not match input"),
              std::string::npos)
        << Diagnostic;
    EXPECT_EQ(readFixtureFile(OutputPath), OutputSentinel);
    EXPECT_EQ(readFixtureFile(InputPath), Fixture.Binary);
  }
}

TEST(MachOPatchTransaction,
     CompactPrepareFailureLeavesExistingOutputAndInputUnchanged) {
  MachOPatchTransactionFixture Fixture = makeMachOPatchTransactionFixture(
      makeRawOriginal(
          {{kTransactionTextOff, macho_unwind::kARM64ModeFrame, std::nullopt}},
          kTransactionTextOff + kTransactionTextSize)
          .OriginalBytes.size());
  ASSERT_FALSE(Fixture.Binary.empty());

  llvm::SmallString<128> InputPath;
  llvm::SmallString<128> OutputPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
      "neverd-macho-patch-transaction-input", "macho", InputPath));
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
      "neverd-macho-patch-transaction-output", "macho", OutputPath));
  llvm::FileRemover RemoveInput(InputPath);
  llvm::FileRemover RemoveOutput(OutputPath);

  const std::vector<uint8_t> OutputSentinel = {0x4e, 0x44, 0x54, 0x58};
  ASSERT_TRUE(writeFixtureFile(InputPath, Fixture.Binary));
  ASSERT_TRUE(writeFixtureFile(OutputPath, OutputSentinel));

  llvm::LLVMContext Context;
  auto Module = makeMachOPatchTransactionModule(Context);
  MachOPatcher Patcher;
  Patcher.setImageContext(&Fixture.Image);
  testing::internal::CaptureStderr();
  PatchResult Result = Patcher.patch(
      InputPath.str().str(), OutputPath.str().str(), *Module, Arch::AArch64);
  const std::string Diagnostic = testing::internal::GetCapturedStderr();

  EXPECT_FALSE(Result.Success);
  EXPECT_NE(Diagnostic.find("insufficient capacity"), std::string::npos)
      << Diagnostic;
  EXPECT_EQ(readFixtureFile(OutputPath), OutputSentinel);
  EXPECT_EQ(readFixtureFile(InputPath), Fixture.Binary);
}

TEST(MachOPatchTransaction,
     GeneratedCompactWithoutFinalSectionUsesAuthenticatedDwarfFallback) {
  MachOPatchTransactionFixture Fixture =
      makeMachOPatchTransactionFixture(/*CompactCapacity=*/0x2000);
  ASSERT_FALSE(Fixture.Binary.empty());
  removeFinalCompactUnwindSection(Fixture);

  llvm::SmallString<128> InputPath;
  llvm::SmallString<128> OutputPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
      "neverd-macho-patch-no-final-compact-input", "macho", InputPath));
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
      "neverd-macho-patch-no-final-compact-output", "macho", OutputPath));
  llvm::FileRemover RemoveInput(InputPath);
  llvm::FileRemover RemoveOutput(OutputPath);

  const std::vector<uint8_t> OutputSentinel = {0x4e, 0x4f, 0x43, 0x55};
  ASSERT_TRUE(writeFixtureFile(InputPath, Fixture.Binary));
  ASSERT_TRUE(writeFixtureFile(OutputPath, OutputSentinel));

  llvm::LLVMContext Context;
  auto Module = makeMachOPatchTransactionModule(Context);
  MachOPatcher Patcher;
  Patcher.setImageContext(&Fixture.Image);
  PatchResult Result = Patcher.patch(
      InputPath.str().str(), OutputPath.str().str(), *Module, Arch::AArch64);

  ASSERT_TRUE(Result.Success);
  EXPECT_EQ(Result.TrampolineCount, 1u);
  EXPECT_GT(Result.CodeSize, 0u);
  EXPECT_EQ(readFixtureFile(InputPath), Fixture.Binary);
  const std::vector<uint8_t> Output = readFixtureFile(OutputPath);
  EXPECT_NE(Output, OutputSentinel);
  expectStrictInjectedSegmentLayout(Output, kDefaultNdTextSegment,
                                    Arch::AArch64);
  const std::optional<MachOEHFrameRegion> AfterEH =
      findMachOEHFrameRegion(Output);
  ASSERT_TRUE(AfterEH.has_value());
  auto CompactRegion = findMachOCompactUnwindRegion(Output);
  ASSERT_TRUE(static_cast<bool>(CompactRegion))
      << llvm::toString(CompactRegion.takeError());
  EXPECT_FALSE(CompactRegion->has_value());
}

TEST(MachOPatchTransaction,
     RejectsMissingOrUnauthenticatedSourceEntryWithoutMutation) {
  for (unsigned Mutation = 0; Mutation != 2; ++Mutation) {
    SCOPED_TRACE(Mutation);
    MachOPatchTransactionFixture Fixture =
        makeMachOPatchTransactionFixture(/*CompactCapacity=*/0x2000);
    ASSERT_FALSE(Fixture.Binary.empty());
    removeFinalCompactUnwindSection(Fixture);

    llvm::SmallString<128> InputPath;
    llvm::SmallString<128> OutputPath;
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
        "neverd-macho-patch-source-identity-input", "macho", InputPath));
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
        "neverd-macho-patch-source-identity-output", "macho", OutputPath));
    llvm::FileRemover RemoveInput(InputPath);
    llvm::FileRemover RemoveOutput(OutputPath);

    const std::vector<uint8_t> OutputSentinel = {0x49, 0x44, 0x45, 0x4e};
    ASSERT_TRUE(writeFixtureFile(InputPath, Fixture.Binary));
    ASSERT_TRUE(writeFixtureFile(OutputPath, OutputSentinel));

    llvm::LLVMContext Context;
    auto Module = makeMachOPatchTransactionModule(Context);
    llvm::Function *Function = Module->getFunction("lifted_source_function");
    ASSERT_NE(Function, nullptr);
    if (Mutation == 0)
      Function->setMetadata(rewrite_source::FunctionAttachment, nullptr);
    else
      rewrite_source::setOriginalVA(*Function, kTransactionFunctionVA + 4);

    MachOPatcher Patcher;
    Patcher.setImageContext(&Fixture.Image);
    testing::internal::CaptureStderr();
    PatchResult Result = Patcher.patch(
        InputPath.str().str(), OutputPath.str().str(), *Module, Arch::AArch64);
    const std::string Diagnostic = testing::internal::GetCapturedStderr();

    EXPECT_FALSE(Result.Success);
    EXPECT_NE(Diagnostic.find("source identity"), std::string::npos)
        << Diagnostic;
    EXPECT_EQ(readFixtureFile(OutputPath), OutputSentinel);
    EXPECT_EQ(readFixtureFile(InputPath), Fixture.Binary);
  }
}

TEST(MachOPatchTransaction,
     RejectsStaleAddressModelWithoutFinalCompactSection) {
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    SCOPED_TRACE(Mutation);
    MachOPatchTransactionFixture Fixture =
        makeMachOPatchTransactionFixture(/*CompactCapacity=*/0x2000);
    ASSERT_FALSE(Fixture.Binary.empty());
    removeFinalCompactUnwindSection(Fixture);

    BinaryImage WrongContext = Fixture.Image;
    if (Mutation == 0) {
      ASSERT_FALSE(WrongContext.Segments.empty());
      ASSERT_EQ(WrongContext.Segments.front().FileOff, 0u);
      WrongContext.Segments.front().VA += 0x1000;
    } else if (Mutation == 1) {
      auto Section = llvm::find_if(WrongContext.Sections, [](const auto &S) {
        return S.Name == "__got";
      });
      ASSERT_NE(Section, WrongContext.Sections.end());
      Section->FileOff += 8;
    } else {
      ASSERT_EQ(WrongContext.ImportPtrSlots.size(), 1u);
      const std::string Symbol = WrongContext.ImportPtrSlots.begin()->second;
      WrongContext.ImportPtrSlots.clear();
      WrongContext.ImportPtrSlots[kTransactionPersonalitySlotVA + 8] = Symbol;
    }

    llvm::SmallString<128> InputPath;
    llvm::SmallString<128> OutputPath;
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
        "neverd-macho-patch-no-compact-stale-input", "macho", InputPath));
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
        "neverd-macho-patch-no-compact-stale-output", "macho", OutputPath));
    llvm::FileRemover RemoveInput(InputPath);
    llvm::FileRemover RemoveOutput(OutputPath);

    const std::vector<uint8_t> OutputSentinel = {0x53, 0x54, 0x41, 0x4c, 0x45};
    ASSERT_TRUE(writeFixtureFile(InputPath, Fixture.Binary));
    ASSERT_TRUE(writeFixtureFile(OutputPath, OutputSentinel));

    llvm::LLVMContext Context;
    auto Module = makeMachOPatchTransactionModule(Context);
    MachOPatcher Patcher;
    Patcher.setImageContext(&WrongContext);
    testing::internal::CaptureStderr();
    PatchResult Result = Patcher.patch(
        InputPath.str().str(), OutputPath.str().str(), *Module, Arch::AArch64);
    const std::string Diagnostic = testing::internal::GetCapturedStderr();

    EXPECT_FALSE(Result.Success);
    EXPECT_NE(Diagnostic.find("image context does not match input"),
              std::string::npos)
        << Diagnostic;
    EXPECT_EQ(readFixtureFile(OutputPath), OutputSentinel);
    EXPECT_EQ(readFixtureFile(InputPath), Fixture.Binary);
  }
}

TEST(MachOPatchTransaction,
     MissingFinalSectionRemainsValidWhenNothingGeneratesCompactUnwind) {
  MachOPatchTransactionFixture Fixture =
      makeMachOPatchTransactionFixture(/*CompactCapacity=*/0x2000);
  ASSERT_FALSE(Fixture.Binary.empty());
  removeFinalCompactUnwindSection(Fixture);

  llvm::SmallString<128> InputPath;
  llvm::SmallString<128> OutputPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
      "neverd-macho-patch-no-compact-input", "macho", InputPath));
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
      "neverd-macho-patch-no-compact-output", "macho", OutputPath));
  llvm::FileRemover RemoveInput(InputPath);
  llvm::FileRemover RemoveOutput(OutputPath);
  ASSERT_TRUE(writeFixtureFile(InputPath, Fixture.Binary));

  llvm::LLVMContext Context;
  auto Module = makeMachOPatchNoUnwindModule(Context);
  MachOPatcher Patcher;
  Patcher.setImageContext(&Fixture.Image);
  PatchResult Result = Patcher.patch(
      InputPath.str().str(), OutputPath.str().str(), *Module, Arch::AArch64);

  ASSERT_TRUE(Result.Success);
  EXPECT_EQ(Result.TrampolineCount, 1u);
  const std::vector<uint8_t> Output = readFixtureFile(OutputPath);
  expectStrictInjectedSegmentLayout(Output, kDefaultNdTextSegment,
                                    Arch::AArch64);
  auto CompactRegion = findMachOCompactUnwindRegion(Output);
  ASSERT_TRUE(static_cast<bool>(CompactRegion))
      << llvm::toString(CompactRegion.takeError());
  EXPECT_FALSE(CompactRegion->has_value());
}

TEST(MachOPatchTransaction,
     SuccessCommitsEHSegmentTrampolineAndCompactUnwindTogether) {
  MachOPatchTransactionFixture Fixture =
      makeMachOPatchTransactionFixture(/*CompactCapacity=*/0x2000);
  ASSERT_FALSE(Fixture.Binary.empty());
  const std::optional<MachOEHFrameRegion> BeforeEH =
      findMachOEHFrameRegion(Fixture.Binary);
  ASSERT_TRUE(BeforeEH.has_value());

  llvm::SmallString<128> InputPath;
  llvm::SmallString<128> OutputPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
      "neverd-macho-patch-atomic-input", "macho", InputPath));
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
      "neverd-macho-patch-atomic-output", "macho", OutputPath));
  llvm::FileRemover RemoveInput(InputPath);
  llvm::FileRemover RemoveOutput(OutputPath);
  ASSERT_TRUE(writeFixtureFile(InputPath, Fixture.Binary));
  ASSERT_TRUE(writeFixtureFile(OutputPath, {0x4e, 0x44}));

  llvm::LLVMContext Context;
  auto Module = makeMachOPatchTransactionModule(
      Context, llvm::GlobalValue::PrivateLinkage);
  MachOPatcher Patcher;
  Patcher.setImageContext(&Fixture.Image);
  PatchResult Result = Patcher.patch(
      InputPath.str().str(), OutputPath.str().str(), *Module, Arch::AArch64);

  ASSERT_TRUE(Result.Success);
  EXPECT_EQ(Result.TrampolineCount, 1u);
  EXPECT_GT(Result.CodeSize, 0u);
  EXPECT_EQ(Result.OutputPath, OutputPath.str().str());
  EXPECT_EQ(readFixtureFile(InputPath), Fixture.Binary);
  const std::vector<uint8_t> Output = readFixtureFile(OutputPath);
  ASSERT_GT(Output.size(), Fixture.Binary.size());
  expectStrictInjectedSegmentLayout(Output, kDefaultNdTextSegment,
                                    Arch::AArch64);

  ASSERT_LE(kTransactionTextOff + sizeof(uint32_t), Output.size());
  EXPECT_FALSE(std::equal(Fixture.Binary.begin() + kTransactionTextOff,
                          Fixture.Binary.begin() + kTransactionTextOff +
                              sizeof(uint32_t),
                          Output.begin() + kTransactionTextOff));

  unsigned InjectedSegmentCount = 0;
  uint64_t InjectedVA = 0;
  uint64_t InjectedFileOff = 0;
  uint64_t ShiftedLinkeditVA = 0;
  uint64_t ShiftedLinkeditFileOff = 0;
  forEachMachOLoadCommand(
      Output.data(), Output.size(),
      [&](const uint8_t *Command, uint32_t ID, uint32_t, bool Is64) {
        if (ID != getMachOSegmentCmdID(Is64))
          return;
        const MachOSegFields Segment = readMachOSegment(Command, Is64);
        const std::string Name = readMachOName(Segment.SegName);
        if (Name == kDefaultNdTextSegment) {
          ++InjectedSegmentCount;
          InjectedVA = Segment.VMAddr;
          InjectedFileOff = Segment.FileOff;
          EXPECT_NE(Segment.InitProt & llvm::MachO::VM_PROT_EXECUTE, 0u);
        } else if (Name == section_names::macho::LinkeditSeg) {
          ShiftedLinkeditVA = Segment.VMAddr;
          ShiftedLinkeditFileOff = Segment.FileOff;
        }
      });
  EXPECT_EQ(InjectedSegmentCount, 1u);
  EXPECT_EQ(InjectedVA, kImageBase + kTransactionLinkeditOff);
  EXPECT_EQ(InjectedFileOff, kTransactionLinkeditOff);
  EXPECT_GT(ShiftedLinkeditVA, kImageBase + kTransactionLinkeditOff);
  EXPECT_GT(ShiftedLinkeditFileOff, kTransactionLinkeditOff);

  const std::optional<MachOEHFrameRegion> AfterEH =
      findMachOEHFrameRegion(Output);
  ASSERT_TRUE(AfterEH.has_value());
  EXPECT_GT(AfterEH->AppendFileOff, BeforeEH->AppendFileOff);
  EXPECT_EQ(AfterEH->SectionFileOff, BeforeEH->SectionFileOff);
  EXPECT_FALSE(std::equal(Fixture.Binary.begin() + kTransactionEHFrameOff,
                          Fixture.Binary.begin() + kTransactionEHFrameOff +
                              kTransactionEHFrameSize,
                          Output.begin() + kTransactionEHFrameOff));

  auto CompactRegion = findMachOCompactUnwindRegion(Output);
  ASSERT_TRUE(static_cast<bool>(CompactRegion))
      << llvm::toString(CompactRegion.takeError());
  ASSERT_TRUE(CompactRegion->has_value());
  auto InstalledCompact =
      macho_unwind::parseCompactUnwindRaw(llvm::ArrayRef<uint8_t>(Output).slice(
          static_cast<size_t>((*CompactRegion)->SectionFileOff),
          static_cast<size_t>((*CompactRegion)->SectionSize)));
  ASSERT_TRUE(static_cast<bool>(InstalledCompact))
      << llvm::toString(InstalledCompact.takeError());

  bool SawOriginal = false;
  bool SawGeneratedDwarf = false;
  for (const macho_unwind::CompactUnwindRawPage &Page :
       InstalledCompact->Pages) {
    for (const macho_unwind::CompactUnwindRawRegularEntry &Entry :
         Page.RegularEntries) {
      SawOriginal |= Entry.FunctionOffset == kTransactionTextOff &&
                     (Entry.Encoding & macho_unwind::kModeMask) ==
                         macho_unwind::kARM64ModeFrame;
      SawGeneratedDwarf |=
          Entry.FunctionOffset == kTransactionLinkeditOff &&
          (Entry.Encoding & macho_unwind::kModeMask) ==
              macho_unwind::kARM64ModeDwarf &&
          (Entry.Encoding & macho_unwind::kDwarfSectionOffsetMask) != 0;
    }
  }
  EXPECT_TRUE(SawOriginal);
  EXPECT_TRUE(SawGeneratedDwarf);
}

TEST(MachOCompactUnwindInstall,
     PlanIsFactoryOnlyImmutableAndSurvivesPostValidation) {
  static_assert(
      !std::is_default_constructible_v<MachOCompactUnwindInstallPlan>);
  static_assert(!std::is_aggregate_v<MachOCompactUnwindInstallPlan>);
  static_assert(!std::is_copy_assignable_v<MachOCompactUnwindInstallPlan>);
  static_assert(!std::is_move_assignable_v<MachOCompactUnwindInstallPlan>);
  static_assert(std::is_same_v<
                decltype(std::declval<const MachOCompactUnwindInstallPlan &>()
                             .expectedSemantics()),
                const MachOCompactUnwindMergeResult &>);

  const CompactUnwindInstallInputs Inputs = makeCompactUnwindInstallInputs();
  const size_t Capacity = Inputs.Encoded.size() + 32;
  std::vector<uint8_t> Binary =
      makeInstallableUnwindInfoMachO(Inputs.Original, Capacity);
  auto PlanOrErr = prepareMachOCompactUnwindInstall(
      Binary, Arch::AArch64, Inputs.Generated, Inputs.Mappings,
      llvm::endianness::little);
  ASSERT_TRUE(static_cast<bool>(PlanOrErr))
      << llvm::toString(PlanOrErr.takeError());
  const MachOCompactUnwindInstallPlan &Plan = *PlanOrErr;
  const std::vector<uint8_t> EncodedBefore(Plan.encodedBytes().begin(),
                                           Plan.encodedBytes().end());
  const MachOCompactUnwindMergeResult SemanticsBefore =
      Plan.expectedSemantics();

  auto Receipt = applyMachOCompactUnwindInstall(Binary, Plan);
  ASSERT_TRUE(static_cast<bool>(Receipt))
      << llvm::toString(Receipt.takeError());
  EXPECT_TRUE(std::equal(Plan.encodedBytes().begin(), Plan.encodedBytes().end(),
                         EncodedBefore.begin(), EncodedBefore.end()));
  EXPECT_EQ(Plan.expectedSemantics(), SemanticsBefore);
}

} // namespace
} // namespace neverd

//===- MachOArm64eDependencyTests.cpp - Whole-image signing ABI -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve exact dyld signing recipes while repacking linked-image metadata.
/// This does not execute dyld or authorize complete arm64e image rebuilding.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/codegen/MachO/MachODependencyState.h"
#include "gtest/gtest.h"

#include "neverd/loader/MachO/MachOArm64e.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
using namespace neverd;
using namespace llvm::MachO;

#define ARM64E_DEPENDENCY_VALUE(Type, Name, Value) constexpr Type Name = Value;
#define ARM64E_DEPENDENCY_TEXT(Name, Text) constexpr char Name[] = Text;
#include "MachOArm64eDependencyFixtures.def"

struct ChainWords {
  unsigned Format;
  uint64_t First, Second;
};
constexpr ChainWords Chains[] = {
#define ARM64E_DEPENDENCY_CHAIN(Format, First, Second) {Format, First, Second},
#include "MachOArm64eDependencyFixtures.def"
};

std::vector<uint8_t> linkedImage(const ChainWords &Chain) {
  std::vector<uint8_t> Bytes(kFileBytes);
  size_t Command = 0;
  const auto Append = [&](const auto &Value) {
    std::memcpy(Bytes.data() + Command, &Value, sizeof(Value));
    Command += sizeof(Value);
  };
  mach_header_64 Header{};
  Header.magic = MH_MAGIC_64;
  Header.cputype = CPU_TYPE_ARM64;
  Header.cpusubtype = macho_loader::arm64e::kCPUSubtype;
  Header.filetype = MH_DYLIB;
  Header.flags = MH_DYLDLINK | MH_TWOLEVEL;
  Header.ncmds = kCommandCount;
  Header.sizeofcmds = kSegmentCount * sizeof(segment_command_64) +
                      kLibraryCommandBytes + sizeof(symtab_command) +
                      sizeof(dysymtab_command) + sizeof(linkedit_data_command);
  Append(Header);
  for (unsigned I = 0; I < kSegmentCount; ++I) {
    segment_command_64 Segment{};
    Segment.cmd = LC_SEGMENT_64;
    Segment.cmdsize = sizeof(Segment);
    const char *Names[] = {kTextSegment, kDataSegment, kLinkeditSegment};
    std::memcpy(Segment.segname, Names[I], std::strlen(Names[I]));
    Segment.vmaddr = kBase + I * kPage;
    Segment.vmsize = Segment.filesize = kPage;
    Segment.fileoff = I * kPage;
    Segment.initprot = Segment.maxprot =
        VM_PROT_READ | (I == 0   ? VM_PROT_EXECUTE
                        : I == 1 ? VM_PROT_WRITE
                                 : 0);
    Append(Segment);
  }
  dylib_command Library{};
  Library.cmd = LC_LOAD_DYLIB;
  Library.cmdsize = kLibraryCommandBytes;
  Library.dylib.name = sizeof(Library);
  const size_t LibraryAt = Command;
  Append(Library);
  std::memcpy(Bytes.data() + Command, kLibrary, sizeof(kLibrary));
  Command = LibraryAt + kLibraryCommandBytes;
  symtab_command Symbols{};
  Symbols.cmd = LC_SYMTAB;
  Symbols.cmdsize = sizeof(Symbols);
  Symbols.symoff = Symbols.stroff = kStringTable;
  Symbols.strsize = 1;
  Append(Symbols);
  dysymtab_command Dynamic{};
  Dynamic.cmd = LC_DYSYMTAB;
  Dynamic.cmdsize = sizeof(Dynamic);
  Append(Dynamic);
  linkedit_data_command Fixups{};
  Fixups.cmd = LC_DYLD_CHAINED_FIXUPS;
  Fixups.cmdsize = sizeof(Fixups);
  Fixups.dataoff = kChains;
  Fixups.datasize = kChainBytes;
  Append(Fixups);
  dyld_chained_fixups_header FixupHeader{};
  FixupHeader.starts_offset = kStarts;
  FixupHeader.imports_offset = kImportTable;
  FixupHeader.symbols_offset = kNames;
  FixupHeader.imports_count = 1;
  FixupHeader.imports_format = DYLD_CHAINED_IMPORT_ADDEND64;
  Command = kChains;
  Append(FixupHeader);
  auto *Payload = Bytes.data() + kChains;
  writeLE<uint32_t>(Payload + kStarts, kSegmentCount);
  writeLE<uint32_t>(Payload + kStarts + 2 * kWordBytes,
                    kSegmentStart - kStarts);
  dyld_chained_starts_in_segment Starts{};
  Starts.size = sizeof(Starts);
  Starts.page_size = kPage;
  Starts.pointer_format = Chain.Format;
  Starts.segment_offset = kPage;
  Starts.page_count = 1;
  Command = kChains + kSegmentStart;
  Append(Starts);
  dyld_chained_import_addend64 Import{};
  Import.lib_ordinal = 1;
  Import.addend = kImportAddend;
  Command = kChains + kImportTable;
  Append(Import);
  std::memcpy(Payload + kNames, kSymbol, sizeof(kSymbol));
  writeLE<uint64_t>(Bytes.data() + kPage, Chain.First);
  writeLE<uint64_t>(Bytes.data() + kSecondSlot, Chain.Second);
  return Bytes;
}

TEST(MachOArm64eDependencies, RetainsSigningABIWhileRepackingChainMetadata) {
  for (const auto &Chain : Chains) {
    SCOPED_TRACE(Chain.Format);
    const auto Original = linkedImage(Chain);
    auto Before = readMachODependencyState(Original);
    ASSERT_TRUE(Before) << llvm::toString(Before.takeError());
    ASSERT_EQ(Before->Slots.size(), 2u);
    auto Repacked = Original;
    auto *Payload = Repacked.data() + kChains;
    std::memmove(Payload + kStarts + kSlotBytes, Payload + kStarts,
                 kRepackBytes);
    writeLE<uint32_t>(Payload +
                          offsetof(dyld_chained_fixups_header, starts_offset),
                      kStarts + kSlotBytes);
    auto After = readMachODependencyState(Repacked);
    ASSERT_TRUE(After) << llvm::toString(After.takeError());
    EXPECT_EQ(Before->Slots, After->Slots);
    EXPECT_EQ(Before->Digest, After->Digest);
    auto Error = preserveMachODependencyState(*Before, *After);
    EXPECT_FALSE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
}

TEST(MachOArm64eDependencies, RejectsChangesToTargetsAndSigningSchemas) {
  for (const auto &Chain : Chains) {
    SCOPED_TRACE(Chain.Format);
    const auto Original = linkedImage(Chain);
    auto Before = readMachODependencyState(Original);
    ASSERT_TRUE(Before) << llvm::toString(Before.takeError());
    const auto Check = [&](unsigned Offset, uint64_t Mask) {
      auto Changed = Original;
      writeLE<uint64_t>(Changed.data() + Offset,
                        readLE<uint64_t>(Changed.data() + Offset) ^ Mask);
      auto After = readMachODependencyState(Changed);
      ASSERT_TRUE(After) << llvm::toString(After.takeError());
      auto Error = preserveMachODependencyState(*Before, *After);
      EXPECT_TRUE(bool(Error));
      llvm::consumeError(std::move(Error));
    };
#define ARM64E_DEPENDENCY_MUTATION(Offset, Mask) Check(Offset, Mask);
#include "MachOArm64eDependencyFixtures.def"
    if (!macho_loader::arm64e::chainFormat(Chain.Format)->Segmented) {
#define ARM64E_DEPENDENCY_IMPORT_MUTATION(Offset, Mask) Check(Offset, Mask);
#include "MachOArm64eDependencyFixtures.def"
    }
    auto ChangedABI = Original;
    writeLE<uint32_t>(ChangedABI.data() + offsetof(mach_header_64, cpusubtype),
                      macho_loader::arm64e::kCPUSubtypeX1);
    auto After = readMachODependencyState(ChangedABI);
    ASSERT_TRUE(After) << llvm::toString(After.takeError());
    auto Error = preserveMachODependencyState(*Before, *After);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
}

TEST(MachOArm64eDependencies, RejectsWrongCPUAndCrossPageStorage) {
  for (const auto &Chain : Chains) {
    const auto Original = linkedImage(Chain);
    auto WrongCPU = Original;
    writeLE<uint32_t>(WrongCPU.data() + offsetof(mach_header_64, cputype),
                      CPU_TYPE_X86_64);
    auto CPUState = readMachODependencyState(WrongCPU);
    EXPECT_FALSE(CPUState);
    llvm::consumeError(CPUState.takeError());
    auto CrossPage = Original;
    writeLE<uint16_t>(CrossPage.data() + kChains + kSegmentStart +
                          offsetof(dyld_chained_starts_in_segment, page_start),
                      kPageEnd);
    auto PageState = readMachODependencyState(CrossPage);
    EXPECT_FALSE(PageState);
    llvm::consumeError(PageState.takeError());
  }
}

TEST(MachOArm64eDependencies, UsesFormatAlignmentAndRetainsSlotAddresses) {
  for (const auto &Chain : Chains) {
    SCOPED_TRACE(Chain.Format);
    const auto Original = linkedImage(Chain);
    auto Before = readMachODependencyState(Original);
    ASSERT_TRUE(Before) << llvm::toString(Before.takeError());
    auto Moved = Original;
    writeLE<uint16_t>(Moved.data() + kChains + kSegmentStart +
                          offsetof(dyld_chained_starts_in_segment, page_start),
                      kWordBytes);
    writeLE<uint64_t>(Moved.data() + kPage + kWordBytes, Chain.First);
    writeLE<uint64_t>(Moved.data() + kSecondSlot + kWordBytes, Chain.Second);
    auto After = readMachODependencyState(Moved);
    if (macho_loader::arm64e::chainFormat(Chain.Format)->Stride != kWordBytes) {
      EXPECT_FALSE(After);
      llvm::consumeError(After.takeError());
      continue;
    }
    ASSERT_TRUE(After) << llvm::toString(After.takeError());
    EXPECT_EQ(After->Slots.size(), Before->Slots.size());
    auto Error = preserveMachODependencyState(*Before, *After);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
}

} // namespace

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

#include "neverd/backend/codegen/MachO/MachOAuthenticatedFixups.h"
#include "neverd/backend/codegen/MachO/MachOPatch.h"
#include "neverd/backend/codegen/SourcePatchDependencies.h"
#include "neverd/loader/Loader.h"
#include "neverd/loader/MachO/MachOArm64e.h"
#include "neverd/loader/MachO/MachOThreadedBind.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
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
  Header.filetype = MH_BUNDLE;
  Header.flags = MH_DYLDLINK | MH_TWOLEVEL;
  Header.ncmds = kCommandCount;
  Header.sizeofcmds = kSegmentCount * sizeof(segment_command_64) +
                      sizeof(section_64) + kLibraryCommandBytes +
                      sizeof(symtab_command) + sizeof(dysymtab_command) +
                      sizeof(linkedit_data_command);
  Append(Header);
  for (unsigned I = 0; I < kSegmentCount; ++I) {
    segment_command_64 Segment{};
    Segment.cmd = LC_SEGMENT_64;
    Segment.cmdsize = sizeof(Segment);
    if (I == 0) {
      Segment.cmdsize += sizeof(section_64);
      Segment.nsects = 1;
    }
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
    if (I == 0) {
      section_64 Text{};
      std::memcpy(Text.segname, kTextSegment, sizeof(kTextSegment));
      std::memcpy(Text.sectname, kTextSection, sizeof(kTextSection));
      Text.addr = kBase + kOrdinaryRebaseTarget;
      Text.offset = kOrdinaryRebaseTarget;
      Text.size = kSlotBytes;
      Text.flags = S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS;
      Append(Text);
    }
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

std::vector<uint8_t> threadedImage() {
  auto Bytes = linkedImage(Chains[0]);
  forEachMachOLoadCommand(Bytes.data(), Bytes.size(),
                          [&](const uint8_t *P, uint32_t ID, uint32_t, bool) {
                            if (ID != LC_DYLD_CHAINED_FIXUPS)
                              return;
                            dyld_info_command Dyld{};
                            Dyld.cmd = LC_DYLD_INFO_ONLY;
                            Dyld.cmdsize = sizeof(Dyld);
                            Dyld.bind_off = kChains;
                            Dyld.bind_size = sizeof(kThreadedProgram) - 1;
                            std::memcpy(Bytes.data() + (P - Bytes.data()),
                                        &Dyld, sizeof(Dyld));
                          });
  auto *Header = reinterpret_cast<mach_header_64 *>(Bytes.data());
  Header->sizeofcmds +=
      sizeof(dyld_info_command) - sizeof(linkedit_data_command);
  std::memcpy(Bytes.data() + kChains, kThreadedProgram,
              sizeof(kThreadedProgram) - 1);
  return Bytes;
}

std::vector<uint8_t> multiStartImage(const ChainWords &Chain) {
  auto Bytes = linkedImage(
      {Chain.Format, Chain.First & ~kNextMask, Chain.Second & ~kNextMask});
  auto *Starts = Bytes.data() + kChains + kSegmentStart;
  constexpr auto StartOffset =
      offsetof(dyld_chained_starts_in_segment, page_start);
  writeLE<uint32_t>(Starts, StartOffset + kMultiStartCount * sizeof(uint16_t));
  writeLE<uint16_t>(Starts + StartOffset,
                    DYLD_CHAINED_PTR_START_MULTI | kOverflowIndex);
  writeLE<uint16_t>(Starts + StartOffset + kOverflowIndex * sizeof(uint16_t),
                    0);
  writeLE<uint16_t>(Starts + StartOffset +
                        kLastOverflowIndex * sizeof(uint16_t),
                    DYLD_CHAINED_PTR_START_LAST | (kSecondSlot - kPage));
  return Bytes;
}

std::vector<uint8_t> largePageImage(const std::vector<uint8_t> &Small) {
  std::vector<uint8_t> Bytes(kSegmentCount * kLargePage);
  for (size_t I = 0; I < kSegmentCount; ++I)
    std::copy_n(Small.begin() + I * kPage, kPage,
                Bytes.begin() + I * kLargePage);
  unsigned SegmentIndex = 0;
  const auto Shift = 2 * (kLargePage - kPage);
  bool Chained = false;
  forEachMachOLoadCommand(
      Bytes.data(), Bytes.size(),
      [&](const uint8_t *Input, uint32_t ID, uint32_t, bool) {
        auto *Command = Bytes.data() + (Input - Bytes.data());
        if (ID == LC_SEGMENT_64) {
          auto *Segment = reinterpret_cast<segment_command_64 *>(Command);
          Segment->vmaddr = kBase + SegmentIndex * kLargePage;
          Segment->fileoff = SegmentIndex++ * kLargePage;
          Segment->vmsize = Segment->filesize = kLargePage;
        } else if (ID == LC_SYMTAB) {
          auto *Symbols = reinterpret_cast<symtab_command *>(Command);
          Symbols->symoff += Shift;
          Symbols->stroff += Shift;
        } else if (ID == LC_DYLD_CHAINED_FIXUPS) {
          reinterpret_cast<linkedit_data_command *>(Command)->dataoff += Shift;
          Chained = true;
        } else if (ID == LC_DYLD_INFO_ONLY) {
          reinterpret_cast<dyld_info_command *>(Command)->bind_off += Shift;
        }
      });
  if (Chained) {
    auto *Starts = reinterpret_cast<dyld_chained_starts_in_segment *>(
        Bytes.data() + kChains + Shift + kSegmentStart);
    Starts->page_size = kLargePage;
    Starts->segment_offset = kLargePage;
  }
  return Bytes;
}

TEST(MachOArm64eDependencies, PreservesMultipleAuthenticatedChainsOnOnePage) {
  for (const auto &Chain : Chains) {
    SCOPED_TRACE(Chain.Format);
    const auto Original = multiStartImage(Chain);
    auto Before = readMachODependencyState(Original);
    ASSERT_TRUE(bool(Before)) << llvm::toString(Before.takeError());
    ASSERT_EQ(Before->Slots.size(), 2u);
    auto Repacked = Original;
    auto *Payload = Repacked.data() + kChains;
    std::memmove(Payload + kStarts + kSlotBytes, Payload + kStarts,
                 kRepackBytes + kSlotBytes);
    writeLE<uint32_t>(Payload +
                          offsetof(dyld_chained_fixups_header, starts_offset),
                      kStarts + kSlotBytes);
    auto After = readMachODependencyState(Repacked);
    ASSERT_TRUE(bool(After)) << llvm::toString(After.takeError());
    EXPECT_EQ(Before->Slots, After->Slots);
    EXPECT_EQ(Before->Digest, After->Digest);
    auto Error = preserveMachODependencyState(*Before, *After);
    EXPECT_FALSE(bool(Error));
    llvm::consumeError(std::move(Error));
    auto Changed = Original;
    writeLE<uint64_t>(Changed.data() + kSecondSlot,
                      readLE<uint64_t>(Original.data() + kSecondSlot) ^
                          uint64_t(1));
    auto ChangedState = readMachODependencyState(Changed);
    if (ChangedState) {
      auto ChangedError = preserveMachODependencyState(*Before, *ChangedState);
      EXPECT_TRUE(bool(ChangedError));
      llvm::consumeError(std::move(ChangedError));
    } else {
      llvm::consumeError(ChangedState.takeError());
    }
  }
}

TEST(MachOArm64eDependencies, RejectsMalformedOrOverlappingOverflowChains) {
  for (const auto &Chain : Chains) {
    SCOPED_TRACE(Chain.Format);
    const auto Original = multiStartImage(Chain);
    constexpr auto StartOffset =
        offsetof(dyld_chained_starts_in_segment, page_start);
    const auto Check = [&](unsigned Index, uint16_t Value) {
      auto Changed = Original;
      writeLE<uint16_t>(Changed.data() + kChains + kSegmentStart + StartOffset +
                            Index * sizeof(uint16_t),
                        Value);
      auto State = readMachODependencyState(Changed);
      EXPECT_FALSE(bool(State));
      if (!State)
        llvm::consumeError(State.takeError());
    };
    Check(0, DYLD_CHAINED_PTR_START_MULTI);
    Check(0, DYLD_CHAINED_PTR_START_MULTI | kOutsideOverflowIndex);
    Check(kLastOverflowIndex, kSecondSlot - kPage);
    Check(kOverflowIndex, kSecondSlot - kPage + kSlotBytes);
    Check(kLastOverflowIndex, DYLD_CHAINED_PTR_START_LAST);
    Check(kLastOverflowIndex, DYLD_CHAINED_PTR_START_LAST | kOverlappingStart);
    Check(kLastOverflowIndex, DYLD_CHAINED_PTR_START_LAST | kPageEnd);
  }
}

TEST(MachOArm64eDependencies, OrdinaryChainsShareMultiStartStorageChecks) {
  for (unsigned Format : {DYLD_CHAINED_PTR_64, DYLD_CHAINED_PTR_64_OFFSET}) {
    const auto Target =
        kOrdinaryRebaseTarget + (Format == DYLD_CHAINED_PTR_64 ? kBase : 0);
    auto Bytes = multiStartImage({Format, Target, kOrdinaryBind});
    writeLE<uint32_t>(Bytes.data() + offsetof(mach_header_64, cpusubtype),
                      CPU_SUBTYPE_ARM64_ALL);
    auto State = readMachODependencyState(Bytes);
    ASSERT_TRUE(bool(State)) << llvm::toString(State.takeError());
    EXPECT_EQ(State->Slots.size(), 2u);
    auto *Starts = Bytes.data() + kChains + kSegmentStart;
    writeLE<uint16_t>(Starts +
                          offsetof(dyld_chained_starts_in_segment, page_start) +
                          kLastOverflowIndex * sizeof(uint16_t),
                      DYLD_CHAINED_PTR_START_LAST);
    auto Overlap = readMachODependencyState(Bytes);
    EXPECT_FALSE(bool(Overlap));
    if (!Overlap)
      llvm::consumeError(Overlap.takeError());
  }
}

TEST(MachOArm64eDependencies, RetainsSigningABIWhileRepackingChainMetadata) {
  for (const auto &Chain : Chains) {
    SCOPED_TRACE(Chain.Format);
    const auto Original = linkedImage(Chain);
    auto Before = readMachODependencyState(Original);
    ASSERT_TRUE(bool(Before)) << llvm::toString(Before.takeError());
    ASSERT_EQ(Before->Slots.size(), 2u);
    auto Repacked = Original;
    auto *Payload = Repacked.data() + kChains;
    std::memmove(Payload + kStarts + kSlotBytes, Payload + kStarts,
                 kRepackBytes);
    writeLE<uint32_t>(Payload +
                          offsetof(dyld_chained_fixups_header, starts_offset),
                      kStarts + kSlotBytes);
    auto After = readMachODependencyState(Repacked);
    ASSERT_TRUE(bool(After)) << llvm::toString(After.takeError());
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
    ASSERT_TRUE(bool(Before)) << llvm::toString(Before.takeError());
    const auto Check = [&](unsigned Offset, uint64_t Mask) {
      auto Changed = Original;
      writeLE<uint64_t>(Changed.data() + Offset,
                        readLE<uint64_t>(Changed.data() + Offset) ^ Mask);
      auto After = readMachODependencyState(Changed);
      ASSERT_TRUE(bool(After)) << llvm::toString(After.takeError());
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
    ASSERT_TRUE(bool(After)) << llvm::toString(After.takeError());
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
    EXPECT_FALSE(bool(CPUState));
    llvm::consumeError(CPUState.takeError());
    auto CrossPage = Original;
    writeLE<uint16_t>(CrossPage.data() + kChains + kSegmentStart +
                          offsetof(dyld_chained_starts_in_segment, page_start),
                      kPageEnd);
    auto PageState = readMachODependencyState(CrossPage);
    EXPECT_FALSE(bool(PageState));
    llvm::consumeError(PageState.takeError());
  }
}

TEST(MachOArm64eDependencies, UsesFormatAlignmentAndRetainsSlotAddresses) {
  for (const auto &Chain : Chains) {
    SCOPED_TRACE(Chain.Format);
    const auto Original = linkedImage(Chain);
    auto Before = readMachODependencyState(Original);
    ASSERT_TRUE(bool(Before)) << llvm::toString(Before.takeError());
    auto Moved = Original;
    writeLE<uint16_t>(Moved.data() + kChains + kSegmentStart +
                          offsetof(dyld_chained_starts_in_segment, page_start),
                      kWordBytes);
    writeLE<uint64_t>(Moved.data() + kPage + kWordBytes, Chain.First);
    writeLE<uint64_t>(Moved.data() + kSecondSlot + kWordBytes, Chain.Second);
    auto After = readMachODependencyState(Moved);
    if (macho_loader::arm64e::chainFormat(Chain.Format)->Stride != kWordBytes) {
      EXPECT_FALSE(bool(After));
      llvm::consumeError(After.takeError());
      continue;
    }
    ASSERT_TRUE(bool(After)) << llvm::toString(After.takeError());
    EXPECT_EQ(After->Slots.size(), Before->Slots.size());
    auto Error = preserveMachODependencyState(*Before, *After);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
}

MachOAuthenticatedFixup installedFixup(bool Bind = false) {
  MachOAuthenticatedFixup Fixup;
  Fixup.Address = kBase + kInstalledSlot;
  Fixup.Recipe.Diversity = kInstalledDiversity;
  if (Bind) {
    Fixup.Recipe.Symbol = kNewSymbol;
    Fixup.Recipe.Module = kLibrary;
    Fixup.Recipe.LibraryOrdinal = 1;
    Fixup.Recipe.Addend = kNewImportAddend;
    Fixup.Recipe.WeakImport = true;
  } else {
    Fixup.Recipe.Target = kBase + kInstalledTarget;
  }
  return Fixup;
}

CompiledImage producerImage(const std::vector<uint8_t> &Original) {
  using Kind = llvm::mc_rewrite::RewriteSectionKind;
  CompiledImage Image;
  Image.Success = true;
  Image.InputKind = RewriteInputKind::ReplacementIR;
  Image.TargetTriple = kProducerTriple;
  Image.Format = BinaryFormat::MachO;
  Image.TargetArch = Arch::AArch64;
  Image.PointerWidth = kSlotBytes;
  Image.BaseVA =
      MachOPatcher().plannedExecSegmentVA(Original, Image.TargetArch);
  Image.Bytes.resize(kLargePage + 2 * kSlotBytes);
  CompiledSection Code, Data;
  Code.Name = kTextSegment;
  Code.VA = Image.BaseVA;
  Code.Size = kSlotBytes;
  Code.Kind = Kind::Code;
  Data.Name = kDataSegment;
  Data.Offset = kLargePage;
  Data.VA = Image.BaseVA + Data.Offset;
  Data.Size = 2 * kSlotBytes;
  Data.Kind = Kind::WritableData;
  CompiledFixupReference Local;
  Local.Symbol = kNewSymbol;
  Local.BitWidth = kSlotBytes * CHAR_BIT;
  Local.IsResolved = true;
  Local.ResolvedValue = Code.VA;
  Local.PointerAuth =
      CompiledPointerAuthentication{0, kInstalledDiversity, true};
  auto Import = Local;
  Import.Symbol = kSymbol;
  Import.Offset = kSlotBytes;
  Import.Addend = kImportAddend;
  Import.ResolvedValue = kInstalledTarget;
  Data.FixupReferences = {Local, Import};
  Image.SymbolAddrs.emplace(Local.Symbol, Local.ResolvedValue);
  Image.Sections = {Code, Data};
  return Image;
}

TEST(MachOArm64eInstallation,
     AutomaticallyInstallsProducerReceiptsInBothFormats) {
  for (bool Threaded : {false, true}) {
    auto Bytes =
        largePageImage(Threaded ? threadedImage() : linkedImage(Chains[0]));
    auto Image = producerImage(Bytes);
    ASSERT_NE(Image.BaseVA, 0u);
    auto Expected = collectMachOAuthenticatedFixups(Bytes, Image);
    ASSERT_TRUE(bool(Expected)) << llvm::toString(Expected.takeError());
    ASSERT_EQ(Expected->size(), 2u);
    EXPECT_EQ(Expected->front().Address, Image.BaseVA + kLargePage);
    EXPECT_EQ(Expected->front().Recipe.Target, Image.BaseVA);
    EXPECT_EQ(Expected->back().Recipe.LibraryOrdinal, 1);
    EXPECT_EQ(Expected->back().Recipe.Addend, kImportAddend);
    auto Error = MachOPatcher().appendReplacementImage(Bytes, Image);
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    Error = verifyMachOAuthenticatedFixups(Bytes, *Expected);
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  }
}

TEST(MachOArm64eInstallation, RejectsIncompleteProducerReceiptsBeforeMutation) {
  const auto Original = largePageImage(threadedImage());
  const auto Check = [&](auto Change) {
    auto Bytes = Original;
    auto Image = producerImage(Bytes);
    Change(Image, Image.Sections.back().FixupReferences.front());
    auto Error = MachOPatcher().appendReplacementImage(Bytes, Image);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
    EXPECT_EQ(Bytes, Original);
  };
  Check([](auto &, auto &F) { F.IsResolved = false; });
  Check([](auto &, auto &F) { F.IsPCRel = true; });
  Check([](auto &, auto &F) { F.Symbol.clear(); });
  Check([](auto &, auto &F) { F.SubtractSymbol = kSymbol; });
  Check([](auto &, auto &F) { ++F.ResolvedValue; });
  Check([](auto &, auto &F) { ++F.Addend; });
  Check([](auto &, auto &F) { --F.BitWidth; });
  Check([](auto &I, auto &) { ++I.Sections.back().VA; });
  Check([](auto &I, auto &) { I.Bytes.back() = 1; });
  Check([](auto &I, auto &) { I.SymbolAddrs.clear(); });
  Check([](auto &I, auto &) {
    I.Sections.back().Kind = llvm::mc_rewrite::RewriteSectionKind::ReadOnlyData;
  });
}

TEST(MachOArm64eDependencies,
     ObjectSigningWordsKeepSignedAddendsAndAllKeyBits) {
  const auto Word =
      macho_loader::arm64e::decodeObjectPointerWord(kObjectSigningWord);
  ASSERT_TRUE(Word);
  EXPECT_EQ(Word->Addend, kObjectSignedAddend);
  EXPECT_EQ(Word->Diversity, kInstalledDiversity);
  EXPECT_EQ(Word->Key, 3u);
  EXPECT_TRUE(Word->AddressDiversity);
  EXPECT_FALSE(
      macho_loader::arm64e::decodeObjectPointerWord(kObjectReservedWord));
  EXPECT_FALSE(macho_loader::arm64e::decodeObjectPointerWord(0));
}

TEST(MachOArm64eInstallation, DeclaredProvidersAndRelroInstallTogether) {
  for (bool Threaded : {false, true})
    for (const uint64_t MutableOffset :
         {uint64_t{0}, 2 * kLargePage, kAlignedMutableOffset}) {
      const bool Mutable = MutableOffset != 0;
      auto Bytes =
          largePageImage(Threaded ? threadedImage() : linkedImage(Chains[0]));
      const auto Before = Bytes;
      auto Image = producerImage(Bytes);
      Image.Sections.back().ReadOnlyAfterFixups = true;
      Image.Sections.back().FixupReferences.back().Symbol =
          kAdditionalNativeSymbol;
      if (Mutable) {
        CompiledSection Data;
        Data.Name = kMutableSegment;
        Data.Kind = llvm::mc_rewrite::RewriteSectionKind::WritableData;
        Data.Offset = MutableOffset;
        Data.VA = Image.BaseVA + Data.Offset;
        Data.Size = kSlotBytes;
        Image.Bytes.resize(Data.Offset + Data.Size);
        Image.Sections.push_back(Data);
      }
      const SourcePatchDependency Dependency{kAdditionalLibrary,
                                             kAdditionalSymbol};
      auto Error =
          MachOPatcher().appendReplacementImage(Bytes, Image, {}, {Dependency});
      ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
      auto Recipes =
          collectMachOAuthenticatedFixups(Bytes, Image, {Dependency});
      ASSERT_TRUE(bool(Recipes)) << llvm::toString(Recipes.takeError());
      ASSERT_EQ(Recipes->size(), 2u);
      EXPECT_EQ(Recipes->back().Recipe.LibraryOrdinal, 2);
      EXPECT_EQ(Recipes->back().Recipe.Module, kAdditionalLibrary);
      EXPECT_EQ(Recipes->back().Recipe.Addend, kImportAddend);
      Error = verifyMachOAuthenticatedFixups(Bytes, *Recipes);
      ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
      unsigned Relro = 0, Writable = 0;
      forEachMachOLoadCommand(
          Bytes.data(), Bytes.size(),
          [&](const uint8_t *Command, uint32_t ID, uint32_t, bool Is64) {
            if (ID != LC_SEGMENT_64)
              return;
            const auto *Segment =
                reinterpret_cast<const segment_command_64 *>(Command);
            const auto Name = readMachOName(Segment->segname);
            if (Name == kRelroSegment) {
              ++Relro;
              EXPECT_EQ(Segment->vmaddr, Image.BaseVA + kLargePage);
              const auto Size =
                  Mutable ? MutableOffset - kLargePage : kLargePage;
              EXPECT_EQ(Segment->vmsize, Size);
              EXPECT_EQ(Segment->filesize, Size);
              EXPECT_EQ(Segment->flags, kReadOnlySegmentFlag);
              EXPECT_EQ(Segment->initprot, VM_PROT_READ | VM_PROT_WRITE);
              EXPECT_EQ(Segment->maxprot, VM_PROT_READ | VM_PROT_WRITE);
            }
            if (Name == kMutableSegment) {
              ++Writable;
              EXPECT_EQ(Segment->vmaddr, Image.BaseVA + MutableOffset);
              EXPECT_EQ(Segment->flags, 0u);
            }
          });
      EXPECT_EQ(Relro, 1u);
      EXPECT_EQ(Writable, unsigned(Mutable));
      auto OldState = readMachODependencyState(Before);
      auto NewState = readMachODependencyState(Bytes);
      ASSERT_TRUE(bool(OldState));
      ASSERT_TRUE(bool(NewState));
      Error = preserveMachODependencyState(*OldState, *NewState);
      ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    }
}

TEST(MachOArm64eInstallation, InvalidProviderAndRelroRequestsRollBackTogether) {
  const auto Original = largePageImage(threadedImage());
  const auto Check = [&](auto Change) {
    auto Bytes = Original;
    auto Image = producerImage(Bytes);
    Image.Sections.back().ReadOnlyAfterFixups = true;
    Image.Sections.back().FixupReferences.back().Symbol =
        kAdditionalNativeSymbol;
    std::vector<SourcePatchDependency> Requests{
        {kAdditionalLibrary, kAdditionalSymbol}};
    Change(Bytes, Image, Requests);
    const auto Before = Bytes;
    auto Error =
        MachOPatcher().appendReplacementImage(Bytes, Image, {}, Requests);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
    EXPECT_EQ(Bytes, Before);
  };
  Check([](auto &, auto &, auto &D) { D.front().Symbol = kUnusedDependency; });
  Check([](auto &, auto &, auto &D) { D.push_back(D.front()); });
  Check([](auto &, auto &, auto &D) { D.front().Library.push_back('\0'); });
  Check([](auto &B, auto &, auto &) {
    const auto H = parseMachOHeader(B.data(), B.size());
    B[H.HeaderSize + H.SizeOfCmds] = kOccupiedHeader;
  });
  Check([](auto &B, auto &, auto &D) {
    D.front().Library = kLibrary;
    forEachMachOLoadCommand(
        B.data(), B.size(), [&](const uint8_t *C, uint32_t ID, uint32_t, bool) {
          if (ID == LC_LOAD_DYLIB)
            writeLE<uint32_t>(B.data() + (C - B.data()), LC_LOAD_WEAK_DYLIB);
        });
  });
  Check([](auto &, auto &I, auto &) {
    auto Mutable = I.Sections.back();
    Mutable.ReadOnlyAfterFixups = false;
    Mutable.FixupReferences.clear();
    Mutable.Offset += Mutable.Size;
    Mutable.VA += Mutable.Size;
    I.Bytes.resize(Mutable.Offset + Mutable.Size);
    I.Sections.push_back(Mutable);
  });
  Check([](auto &, auto &I, auto &) {
    I.Sections.back().Kind = llvm::mc_rewrite::RewriteSectionKind::ReadOnlyData;
  });
}

TEST(MachOArm64eInstallation, RejectsFailedProducerPlacementBeforeMutation) {
  for (bool Signing : {false, true}) {
    auto Bytes = threadedImage();
    const auto Original = Bytes;
    auto Image = producerImage(Bytes);
    ASSERT_EQ(Image.BaseVA, 0u);
    if (!Signing)
      Image.Sections.back().FixupReferences.clear();
    auto Error = MachOPatcher().appendReplacementImage(Bytes, Image);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
    EXPECT_EQ(Bytes, Original);
  }
}

TEST(MachOArm64eInstallation, ProducerRequiresOneExactProviderPerSymbol) {
  for (bool DifferentProvider : {false, true}) {
    auto Bytes = linkedImage(Chains[0]);
    auto *Header =
        reinterpret_cast<dyld_chained_fixups_header *>(Bytes.data() + kChains);
    Header->imports_count = 2;
    auto *Import = Bytes.data() + kChains + kImportTable +
                   sizeof(dyld_chained_import_addend64);
    // Same name, distinct addend does not change provider identity. A special
    // ordinal instead of the original dylib does change it and must refuse.
    writeLE<uint64_t>(Import, DifferentProvider ? 0 : 1);
    writeLE<int64_t>(Import + sizeof(uint64_t), kNewImportAddend);
    Bytes = largePageImage(Bytes);
    const auto Original = Bytes;
    auto Image = producerImage(Bytes);
    auto Error = MachOPatcher().appendReplacementImage(Bytes, Image);
    if (DifferentProvider) {
      EXPECT_TRUE(bool(Error));
      llvm::consumeError(std::move(Error));
      EXPECT_EQ(Bytes, Original);
    } else {
      ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    }
  }
}

TEST(MachOArm64eInstallation, CompilerRetainsOrRefusesNestedSigningConstants) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module =
      llvm::parseAssemblyString(kNestedProducerIR, Diagnostic, Context);
  ASSERT_TRUE(Module);
  auto NativeModule = llvm::CloneModule(*Module);
  auto Native = Codegen().compile(*NativeModule, Arch::AArch64,
                                  BinaryFormat::MachO, kProducerTriple);
  ASSERT_FALSE(Native.ObjectData.empty());
  auto Object = Loader::create(BinaryFormat::MachO)
                    ->loadBuffer(llvm::MemoryBufferRef(
                        llvm::toStringRef(llvm::ArrayRef(Native.ObjectData)),
                        kProducerModule));
  ASSERT_TRUE(bool(Object)) << llvm::toString(Object.takeError());
  ASSERT_EQ(Object->MachOAuthenticatedPointers.size(), 1u);
  EXPECT_EQ(Object->MachOAuthenticatedPointers.begin()->second.Symbol, kSymbol);
  EXPECT_EQ(Object->MachOAuthenticatedPointers.begin()->second.Addend,
            kImportAddend);
  auto Image = compileImageForPatch(
      *Module, Arch::AArch64, BinaryFormat::MachO, kBase,
      PatchSymbolResolver(
          [](llvm::StringRef, uint32_t) -> std::optional<uint64_t> {
            return kInstalledTarget;
          }),
      kBase, kProducerTriple, RewriteInputKind::ReplacementIR);
#if __has_include("llvm/MC/MCPointerAuthInfo.h")
  ASSERT_TRUE(Image.Success);
  size_t Recipes = 0;
  for (const auto &Section : Image.Sections)
    for (const auto &Fixup : Section.FixupReferences)
      Recipes += Fixup.PointerAuth.has_value();
  EXPECT_EQ(Recipes, 1u);
#else
  EXPECT_FALSE(Image.Success);
#endif
}

TEST(MachOArm64eInstallation, MCProducerCapturesRecipesThroughFinalPlacement) {
#if !__has_include("llvm/MC/MCPointerAuthInfo.h")
  GTEST_SKIP() << kMissingProducer;
#else
  for (bool Assembly : {false, true})
    for (bool Threaded : {false, true}) {
      auto Bytes =
          largePageImage(Threaded ? threadedImage() : linkedImage(Chains[0]));
      llvm::LLVMContext Context;
      llvm::SMDiagnostic Diagnostic;
      auto Module = llvm::parseAssemblyString(kProducerIR, Diagnostic, Context);
      ASSERT_TRUE(Module);
      if (Assembly) {
        Module = std::make_unique<llvm::Module>(kProducerModule, Context);
        Module->setModuleInlineAsm(kProducerAssembly);
      }
      const auto Base =
          MachOPatcher().plannedExecSegmentVA(Bytes, Arch::AArch64);
      ASSERT_NE(Base, 0u);
      auto Image = compileImageForPatch(
          *Module, Arch::AArch64, BinaryFormat::MachO, Base,
          PatchSymbolResolver(
              [](llvm::StringRef Name, uint32_t) -> std::optional<uint64_t> {
                return Name == kSymbol ? std::optional(kInstalledTarget)
                                       : std::nullopt;
              }),
          kBase, kProducerTriple, RewriteInputKind::ReplacementIR);
      ASSERT_TRUE(Image.Success);
      auto Recipes = collectMachOAuthenticatedFixups(Bytes, Image);
      ASSERT_TRUE(bool(Recipes)) << llvm::toString(Recipes.takeError());
      ASSERT_EQ(Recipes->size(), 2u);
      auto Error = MachOPatcher().appendReplacementImage(Bytes, Image);
      ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
      Error = verifyMachOAuthenticatedFixups(Bytes, *Recipes);
      ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    }
#endif
}

TEST(MachOArm64eInstallation,
     ThreadedLibraryOrdinalsAreNotChainedImportFields) {
  const auto Bytes = llvm::arrayRefFromStringRef(
      llvm::StringRef(kWideThreadedProgram, sizeof(kWideThreadedProgram) - 1));
  const auto Plan =
      macho_loader::threaded::decode(Bytes, kWideThreadedLibraryOrdinal);
  ASSERT_TRUE(Plan);
  ASSERT_EQ(Plan->Imports.size(), 1u);
  EXPECT_EQ(Plan->Imports.front().Ordinal, kWideThreadedLibraryOrdinal);
  EXPECT_FALSE(
      macho_loader::threaded::decode(Bytes, kWideThreadedLibraryOrdinal - 1));
}

TEST(MachOArm64eInstallation, BoundsThreadedSymbolCopiesBeforeInstallation) {
  for (unsigned Count : {kThreadedSmallCopyCount, kThreadedLargeCopyCount}) {
    SCOPED_TRACE(Count);
    auto Bytes = threadedImage();
    std::string Program(kThreadedProgram, sizeof(kThreadedProgram) - 1);
    Program.replace(kThreadedSymbolFlagsOffset + 1, sizeof(kSymbol) - 1,
                    kThreadedLongSymbolBytes, kThreadedLongSymbolByte);
    const auto Strings = kChains + Program.size();
    Bytes.resize(Strings + 1);
    std::memcpy(Bytes.data() + kChains, Program.data(), Program.size());
    forEachMachOLoadCommand(
        Bytes.data(), Bytes.size(),
        [&](const uint8_t *P, uint32_t ID, uint32_t, bool) {
          auto *Command = Bytes.data() + (P - Bytes.data());
          if (ID == LC_DYLD_INFO_ONLY) {
            reinterpret_cast<dyld_info_command *>(Command)->bind_size =
                Program.size();
          } else if (ID == LC_SYMTAB) {
            auto *Symbols = reinterpret_cast<symtab_command *>(Command);
            Symbols->symoff = Symbols->stroff = Strings;
          } else if (ID == LC_SEGMENT_64) {
            auto *Segment = reinterpret_cast<segment_command_64 *>(Command);
            if (Segment->fileoff == kLinkedit) {
              Segment->filesize = Bytes.size() - kLinkedit;
              Segment->vmsize = llvm::alignTo(Segment->filesize, kPage);
            }
          }
        });
    for (unsigned I = 0; I < Count; ++I)
      writeLE<uint64_t>(Bytes.data() + kPage + I * kSlotBytes,
                        Chains[0].Second |
                            (I + 1 < Count ? Chains[0].First & kNextMask : 0));
    auto State = readMachODependencyState(Bytes);
    if (Count == kThreadedSmallCopyCount) {
      ASSERT_TRUE(bool(State)) << llvm::toString(State.takeError());
      EXPECT_EQ(State->Slots.size(), Count);
    } else {
      ASSERT_FALSE(bool(State));
      EXPECT_NE(llvm::toString(State.takeError()).find(kThreadedCopyDiagnostic),
                std::string::npos);
    }
    const auto Original = Bytes;
    auto Fixup = installedFixup();
    Fixup.Address = kBase + kPage + Count * kSlotBytes;
    auto Error = installMachOAuthenticatedFixups(Bytes, {Fixup});
    if (Count == kThreadedSmallCopyCount) {
      ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    } else {
      ASSERT_TRUE(bool(Error));
      EXPECT_NE(llvm::toString(std::move(Error)).find(kThreadedCopyDiagnostic),
                std::string::npos);
      EXPECT_EQ(Bytes, Original);
    }
  }
}

TEST(MachOArm64eInstallation,
     ExtendsThreadedTablesAndPreservesOriginalRecipes) {
  constexpr int64_t Ordinals[] = {
      BIND_SPECIAL_DYLIB_WEAK_LOOKUP, BIND_SPECIAL_DYLIB_FLAT_LOOKUP,
      BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE, BIND_SPECIAL_DYLIB_SELF, 1};
  for (int64_t Ordinal : Ordinals) {
    auto Bytes = threadedImage();
    // Retain the NON_WEAK_DEFINITION flag without mistaking it for weak import.
    Bytes[kChains + kThreadedSymbolFlagsOffset] |=
        BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION;
    const auto Original = Bytes;
    auto Before = readMachODependencyState(Bytes);
    ASSERT_TRUE(bool(Before)) << llvm::toString(Before.takeError());
    auto Local = installedFixup();
    auto Import = installedFixup(true);
    Import.Address += kSlotBytes;
    Import.Recipe.LibraryOrdinal = Ordinal;
    if (Ordinal <= 0)
      Import.Recipe.Module.clear();
    for (bool Weak : {false, true}) {
      auto Installed = Original;
      Import.Recipe.WeakImport = Weak;
      auto Error = installMachOAuthenticatedFixups(Installed, {Local, Import});
      ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
      MachOChainedFixups Facts;
      auto After = readMachODependencyState(Installed, &Facts);
      ASSERT_TRUE(bool(After)) << llvm::toString(After.takeError());
      EXPECT_TRUE(Facts.Threaded);
      ASSERT_EQ(Facts.Imports.size(), 2u);
      EXPECT_EQ(Facts.Imports.front().Flags,
                BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION);
      Error = preserveMachODependencyState(*Before, *After);
      ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
      Error = verifyMachOAuthenticatedFixups(Installed, {Local, Import});
      ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
      auto Loaded =
          Loader::create(BinaryFormat::MachO)
              ->loadBuffer(llvm::MemoryBufferRef(
                  llvm::toStringRef(llvm::ArrayRef(Installed)), kSymbol));
      ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
      ASSERT_TRUE(Loaded->MachOChainedFixupsError.empty());
      EXPECT_EQ(Loaded->MachOAuthenticatedPointers.size(), After->Slots.size());
      EXPECT_EQ(
          Loaded->MachOAuthenticatedPointers.at(Import.Address).WeakImport,
          Weak);
      auto Reordered = Original;
      Error = installMachOAuthenticatedFixups(Reordered, {Import, Local});
      ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
      EXPECT_EQ(Installed, Reordered);
      const auto Committed = Installed;
      Error = installMachOAuthenticatedFixups(Installed, {Local});
      EXPECT_TRUE(bool(Error));
      llvm::consumeError(std::move(Error));
      EXPECT_EQ(Installed, Committed);
    }
  }
}

TEST(MachOArm64eInstallation, ConvertsThreadedWithoutChangingSlotIdentity) {
  for (bool UnsupportedFlag : {false, true}) {
    auto Bytes = threadedImage();
    if (UnsupportedFlag)
      Bytes[kChains + kThreadedSymbolFlagsOffset] |=
          BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION;
    auto Import = installedFixup(true);
    auto Error = installMachOAuthenticatedFixups(Bytes, {Import});
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    const auto Original = Bytes;
    auto Before = readMachODependencyState(Bytes);
    ASSERT_TRUE(bool(Before)) << llvm::toString(Before.takeError());
    Error = convertMachOThreadedFixupsToChains(Bytes);
    if (UnsupportedFlag) {
      EXPECT_TRUE(bool(Error));
      llvm::consumeError(std::move(Error));
      EXPECT_EQ(Bytes, Original);
      continue;
    }
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    MachOChainedFixups Facts;
    auto After = readMachODependencyState(Bytes, &Facts);
    ASSERT_TRUE(bool(After)) << llvm::toString(After.takeError());
    EXPECT_FALSE(Facts.Threaded);
    EXPECT_EQ(After->Slots.size(), Before->Slots.size());
    Error = preserveMachODependencyState(*Before, *After);
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    Error = verifyMachOAuthenticatedFixups(Bytes, {Import});
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    const auto Converted = Bytes;
    Error = convertMachOThreadedFixupsToChains(Bytes);
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    EXPECT_EQ(Bytes, Converted);
  }
}

TEST(MachOArm64eInstallation,
     DefaultStartupRepartitionsPagesWithoutChangingABI) {
  for (const auto &Chain : Chains) {
    if (Chain.Format != DYLD_CHAINED_PTR_ARM64E &&
        Chain.Format != DYLD_CHAINED_PTR_ARM64E_USERLAND &&
        Chain.Format != DYLD_CHAINED_PTR_ARM64E_USERLAND24)
      continue;
    for (bool MultiStart : {false, true}) {
      SCOPED_TRACE(Chain.Format);
      SCOPED_TRACE(MultiStart);
      auto Bytes = largePageImage(MultiStart ? multiStartImage(Chain)
                                             : linkedImage(Chain));
      auto Last = installedFixup();
      Last.Address = kBase + kLargePage + kPage - kSlotBytes;
      Last.Recipe.Key = kDataBKey;
      Last.Recipe.AddressDiversity = true;
      auto First = installedFixup(true);
      First.Address = Last.Address + kSlotBytes;
      auto Far = First;
      Far.Address = kBase + kLargePage + kFarSlot;
      Far.Recipe.LibraryOrdinal = BIND_SPECIAL_DYLIB_WEAK_LOOKUP;
      Far.Recipe.Module.clear();
      Far.Recipe.Addend = -int64_t(kNewImportAddend);
      auto Plain = Last;
      Plain.Address = Far.Address + kSlotBytes;
      Plain.Recipe = {};
      Plain.Authenticated = false;
      Plain.Recipe.Target = kBase + kOrdinaryRebaseTarget;
      auto Error =
          installMachOAuthenticatedFixups(Bytes, {Last, First, Far, Plain});
      ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
      auto Before = readMachODependencyState(Bytes);
      ASSERT_TRUE(bool(Before)) << llvm::toString(Before.takeError());
      const auto Original = Bytes;
      Error = prepareMachOAuthenticatedStartup(Bytes);
      ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
      MachOChainedFixups Facts;
      auto After = readMachODependencyState(Bytes, &Facts);
      ASSERT_TRUE(bool(After)) << llvm::toString(After.takeError());
      EXPECT_FALSE(Facts.Threaded);
      EXPECT_EQ(Before->Slots, After->Slots);
      Error = preserveMachODependencyState(*Before, *After);
      ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
      Error = verifyMachOAuthenticatedFixups(Bytes, {Last, First, Far, Plain});
      ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
      const auto &Data = Facts.Segments[1];
      EXPECT_EQ(Data.PageSize, kPage);
      ASSERT_EQ(Data.Slots.size(), 6u);
      const auto Format = *macho_loader::arm64e::chainFormat(Chain.Format);
      for (const auto &Slot : Data.Slots) {
        const auto Word =
            *macho_loader::arm64e::decodePointerWord(Slot.Raw, Format);
        if (Slot.Offset == kPage - kSlotBytes || Slot.Offset == kPage)
          EXPECT_EQ(Word.Next, 0u);
      }
      // Repacking must not change instructions, segment placement/protections,
      // or the now-unreferenced original metadata.
      EXPECT_TRUE(std::equal(Original.begin() + kOrdinaryRebaseTarget,
                             Original.begin() + kLargePage,
                             Bytes.begin() + kOrdinaryRebaseTarget));
      const auto Prepared = Bytes;
      Error = prepareMachOAuthenticatedStartup(Bytes);
      ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
      EXPECT_EQ(Bytes, Prepared);
    }
  }
}

TEST(MachOArm64eInstallation, DefaultStartupMigratesThreadedAndIsIdempotent) {
  auto Bytes = largePageImage(threadedImage());
  auto Before = readMachODependencyState(Bytes);
  ASSERT_TRUE(bool(Before)) << llvm::toString(Before.takeError());
  auto Error = prepareMachOAuthenticatedStartup(Bytes);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  MachOChainedFixups Facts;
  auto After = readMachODependencyState(Bytes, &Facts);
  ASSERT_TRUE(bool(After)) << llvm::toString(After.takeError());
  EXPECT_FALSE(Facts.Threaded);
  EXPECT_EQ(Facts.Segments[1].PageSize, kPage);
  EXPECT_EQ(Before->Slots, After->Slots);
  Error = preserveMachODependencyState(*Before, *After);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  const auto Prepared = Bytes;
  Error = prepareMachOAuthenticatedStartup(Bytes);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  EXPECT_EQ(Bytes, Prepared);
}

TEST(MachOArm64eInstallation,
     DefaultStartupKeepsAKeyAndSmallPageImagesUnchanged) {
  for (bool Small : {false, true}) {
    auto Bytes =
        Small ? linkedImage(Chains[0]) : largePageImage(linkedImage(Chains[0]));
    if (!Small) {
      auto *Slot = Bytes.data() + kLargePage + kSlotBytes;
      writeLE<uint64_t>(Slot, readLE<uint64_t>(Slot) & ~kBKeyBit);
    }
    const auto Original = Bytes;
    auto Error = prepareMachOAuthenticatedStartup(Bytes);
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    EXPECT_EQ(Bytes, Original);
  }
  for (unsigned Format : {DYLD_CHAINED_PTR_64, DYLD_CHAINED_PTR_64_OFFSET}) {
    const auto Target =
        kOrdinaryRebaseTarget + (Format == DYLD_CHAINED_PTR_64 ? kBase : 0);
    auto Bytes =
        largePageImage(multiStartImage({Format, Target, kOrdinaryBind}));
    const auto Original = Bytes;
    auto Error = prepareMachOAuthenticatedStartup(Bytes);
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    EXPECT_EQ(Bytes, Original);
  }
}

TEST(MachOArm64eInstallation,
     DefaultStartupRejectsUnrepresentableInputsAtomically) {
  for (const auto &Chain : Chains) {
    if (Chain.Format == DYLD_CHAINED_PTR_ARM64E ||
        Chain.Format == DYLD_CHAINED_PTR_ARM64E_USERLAND ||
        Chain.Format == DYLD_CHAINED_PTR_ARM64E_USERLAND24)
      continue;
    auto Bytes = largePageImage(linkedImage(Chain));
    const auto Original = Bytes;
    auto Error = prepareMachOAuthenticatedStartup(Bytes);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
    EXPECT_EQ(Bytes, Original);
  }
  auto Bytes = threadedImage();
  Bytes[kChains + kThreadedSymbolFlagsOffset] |=
      BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION;
  const auto Original = Bytes;
  auto Error = prepareMachOAuthenticatedStartup(Bytes);
  EXPECT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
  EXPECT_EQ(Bytes, Original);
}

TEST(MachOArm64eInstallation, DefaultStartupDistinguishesEverySigningKey) {
  const struct {
    uint64_t Word;
    unsigned PageSize;
  } Cases[] = {
#define ARM64E_STARTUP_CASE(Word, PageSize) {Word, PageSize},
#include "MachOArm64eDependencyFixtures.def"
  };
  for (const auto &Case : Cases) {
    auto Chain = Chains[0];
    Chain.Second = Case.Word;
    auto Bytes = largePageImage(linkedImage(Chain));
    auto Before = readMachODependencyState(Bytes);
    ASSERT_TRUE(bool(Before)) << llvm::toString(Before.takeError());
    auto Error = prepareMachOAuthenticatedStartup(Bytes);
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    MachOChainedFixups Facts;
    auto After = readMachODependencyState(Bytes, &Facts);
    ASSERT_TRUE(bool(After)) << llvm::toString(After.takeError());
    EXPECT_EQ(Facts.Segments[1].PageSize, Case.PageSize);
    EXPECT_EQ(Before->Slots, After->Slots);
  }
}

TEST(MachOArm64eInstallation,
     RejectsIncompleteThreadedProgramsTransactionally) {
  const auto Check = [&](auto Change) {
    auto Bytes = threadedImage();
    Change(Bytes);
    const auto Original = Bytes;
    auto Error = installMachOAuthenticatedFixups(Bytes, {installedFixup()});
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
    EXPECT_EQ(Bytes, Original);
  };
  Check([](auto &B) {
    B[kChains + sizeof(kThreadedProgram) - 2] = BIND_OPCODE_DO_BIND;
  });
  Check([](auto &B) { B[kChains + 1] = 0; });
  Check([](auto &B) { B[kChains + 1] = 2; });
  Check([](auto &B) {
    writeLE<uint64_t>(B.data() + kSecondSlot, kBindMask | kImportLimit16);
  });
  Check([](auto &B) {
    forEachMachOLoadCommand(B.data(), B.size(),
                            [&](const uint8_t *P, uint32_t ID, uint32_t, bool) {
                              if (ID == LC_DYLD_INFO_ONLY) {
                                auto *D = reinterpret_cast<dyld_info_command *>(
                                    B.data() + (P - B.data()));
                                --D->bind_size;
                              }
                            });
  });
  Check([](auto &B) {
    forEachMachOLoadCommand(
        B.data(), B.size(), [&](const uint8_t *P, uint32_t ID, uint32_t, bool) {
          if (ID == LC_DYLD_INFO_ONLY) {
            auto *D = reinterpret_cast<dyld_info_command *>(B.data() +
                                                            (P - B.data()));
            B[D->bind_off + D->bind_size++] = BIND_OPCODE_DO_BIND;
          }
        });
  });
  Check([](auto &B) {
    forEachMachOLoadCommand(B.data(), B.size(),
                            [&](const uint8_t *P, uint32_t ID, uint32_t, bool) {
                              if (ID == LC_DYLD_INFO_ONLY) {
                                auto *D = reinterpret_cast<dyld_info_command *>(
                                    B.data() + (P - B.data()));
                                D->weak_bind_off = kStringTable;
                                D->weak_bind_size = 1;
                              }
                            });
  });
}

TEST(MachOArm64eInstallation,
     InstallsEveryKeyAndDiversityWithExactImportIdentity) {
  struct SigningCase {
    uint8_t Key;
    bool AddressDiversity;
    uint64_t Word;
  };
  constexpr SigningCase Cases[] = {
#define ARM64E_INSTALL_CASE(Key, AddressDiversity, Word)                       \
  {Key, AddressDiversity, Word},
#include "MachOArm64eDependencyFixtures.def"
  };
  for (const auto &Chain : Chains) {
    SCOPED_TRACE(Chain.Format);
    for (const auto &Case : Cases) {
      SCOPED_TRACE(Case.Word);
      for (bool Bind : {false, true}) {
        auto Bytes = multiStartImage(Chain);
        const auto Original = Bytes;
        auto Fixup = installedFixup(Bind);
        Fixup.Recipe.Key = Case.Key;
        Fixup.Recipe.AddressDiversity = Case.AddressDiversity;
        auto Error = installMachOAuthenticatedFixups(Bytes, {Fixup});
        if (Bind &&
            macho_loader::arm64e::chainFormat(Chain.Format)->Segmented) {
          EXPECT_TRUE(bool(Error));
          llvm::consumeError(std::move(Error));
          EXPECT_EQ(Bytes, Original);
          continue;
        }
        ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
        EXPECT_EQ(readLE<uint64_t>(Bytes.data() + kInstalledSlot),
                  Case.Word | (Bind ? kBindMask | 1 : kInstalledTarget));
        Error = verifyMachOAuthenticatedFixups(Bytes, {Fixup});
        EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
        // The analysis loader is a separate consumer of the installed image.
        auto Image =
            Loader::create(BinaryFormat::MachO)
                ->loadBuffer(llvm::MemoryBufferRef(
                    llvm::toStringRef(llvm::ArrayRef(Bytes)), kNewSymbol));
        ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
        ASSERT_TRUE(Image->MachOAuthenticatedPointers.contains(Fixup.Address));
        const auto &Recipe =
            Image->MachOAuthenticatedPointers.at(Fixup.Address);
        EXPECT_EQ(Recipe.Key, Case.Key);
        EXPECT_EQ(Recipe.Diversity, kInstalledDiversity);
        EXPECT_EQ(Recipe.AddressDiversity, Case.AddressDiversity);
        EXPECT_EQ(Recipe.Symbol, Fixup.Recipe.Symbol);
        EXPECT_EQ(Recipe.Module, Fixup.Recipe.Module);
        EXPECT_EQ(Recipe.LibraryOrdinal, Fixup.Recipe.LibraryOrdinal);
        EXPECT_EQ(Recipe.Addend, Fixup.Recipe.Addend);
        EXPECT_EQ(Recipe.WeakImport, Fixup.Recipe.WeakImport);
        EXPECT_EQ(Recipe.Target, Fixup.Recipe.Target);
        // Replaying against a different discriminator must not validate.
        Fixup.Recipe.Diversity ^= 1;
        Error = verifyMachOAuthenticatedFixups(Bytes, {Fixup});
        EXPECT_TRUE(bool(Error));
        llvm::consumeError(std::move(Error));
      }
    }
  }
}

TEST(MachOArm64eInstallation,
     PreservesOldSlotsAndDeduplicatesOnlyExactImports) {
  for (const auto &Chain : Chains) {
    if (macho_loader::arm64e::chainFormat(Chain.Format)->Segmented)
      continue;
    auto Bytes = linkedImage(Chain);
    const auto Original = Bytes;
    auto Before = readMachODependencyState(Bytes);
    ASSERT_TRUE(bool(Before)) << llvm::toString(Before.takeError());
    auto Same = installedFixup(true);
    Same.Recipe.Symbol = kSymbol;
    Same.Recipe.Addend = kImportAddend;
    Same.Recipe.WeakImport = false;
    auto Different = Same;
    Different.Address += kSlotBytes;
    Different.Recipe.WeakImport = true;
    auto New = installedFixup(true);
    New.Address += 2 * kSlotBytes;
    std::vector<MachOAuthenticatedFixup> Fixups{New, Same, Different};
    auto Error = installMachOAuthenticatedFixups(Bytes, Fixups);
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    MachOChainedFixups Installed;
    auto After = readMachODependencyState(Bytes, &Installed);
    ASSERT_TRUE(bool(After)) << llvm::toString(After.takeError());
    EXPECT_EQ(Installed.Imports.size(), 3u);
    EXPECT_EQ(After->Slots.size(), Before->Slots.size() + Fixups.size());
    Error = preserveMachODependencyState(*Before, *After);
    EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    auto Reordered = Original;
    std::reverse(Fixups.begin(), Fixups.end());
    Error = installMachOAuthenticatedFixups(Reordered, Fixups);
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    EXPECT_EQ(Bytes, Reordered);
    // A second installation cannot overwrite an existing authenticated slot.
    const auto Committed = Bytes;
    Error = installMachOAuthenticatedFixups(Bytes, {Same});
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
    EXPECT_EQ(Bytes, Committed);
  }
}

TEST(MachOArm64eInstallation, RefusesMalformedRequestsWithoutPublishingBytes) {
  for (const auto &Chain : Chains) {
    const auto Original = linkedImage(Chain);
    const auto Check = [&](auto Change) {
      auto Bytes = Original;
      auto Fixup = installedFixup();
      Change(Fixup);
      auto Error = installMachOAuthenticatedFixups(Bytes, {Fixup});
      EXPECT_TRUE(bool(Error));
      llvm::consumeError(std::move(Error));
      EXPECT_EQ(Bytes, Original);
    };
    Check([](auto &F) { F.Address = kBase + kPage; });
    Check([](auto &F) { F.Address = kBase + kPage + kWordBytes; });
    Check([](auto &F) { ++F.Address; });
    Check([](auto &F) { F.Address = kBase + kPageEnd; });
    Check([](auto &F) { F.Address = kBase; });
    Check([](auto &F) { F.Address = kBase + kLinkedit; });
    Check([](auto &F) { F.Address = InvalidVA; });
    Check([](auto &F) { F.Recipe.Key = kWordBytes; });
    Check([](auto &F) { F.Recipe.EncodedValue = 0; });
    Check([](auto &F) { F.Recipe.Target = InvalidVA; });
    Check([](auto &F) { F.Recipe.Target = kBase + kLinkedit; });
    Check([](auto &F) { F.Recipe.Target = kBase - 1; });
    Check([](auto &F) { F.Recipe.Addend = 1; });
    Check([](auto &F) { F.Recipe.LibraryOrdinal = 1; });
    Check([](auto &F) { F.Recipe.Symbol = kNewSymbol; });
    Check([](auto &F) {
      F = installedFixup(true);
      F.Recipe.Module = kNewSymbol;
    });
    Check([](auto &F) {
      F = installedFixup(true);
      F.Recipe.LibraryOrdinal = kImportAddend;
    });
    Check([](auto &F) {
      F = installedFixup(true);
      F.Recipe.LibraryOrdinal = kSegmentCount;
    });
    auto Bytes = Original;
    auto Fixup = installedFixup();
    auto Error = installMachOAuthenticatedFixups(Bytes, {Fixup, Fixup});
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
    EXPECT_EQ(Bytes, Original);
  }
}

TEST(MachOArm64eInstallation, EncodesLargePageGapsAndAllSpecialOrdinals) {
  for (const auto &Chain : Chains) {
    auto Bytes = largePageImage(linkedImage(Chain));
    auto Near = installedFixup();
    Near.Address += kLargePage - kPage;
    auto Far = Near;
    Far.Address += kFarSlot;
    auto Error = installMachOAuthenticatedFixups(Bytes, {Far, Near});
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    MachOChainedFixups Installed;
    auto State = readMachODependencyState(Bytes, &Installed);
    ASSERT_TRUE(bool(State)) << llvm::toString(State.takeError());
    const auto Format = macho_loader::arm64e::chainFormat(Chain.Format);
    const auto &Data = Installed.Segments[1];
    EXPECT_EQ(Data.PageSize, kLargePage);
    ASSERT_EQ(Data.Slots.size(), 4u);
    auto Word =
        macho_loader::arm64e::decodePointerWord(Data.Slots[2].Raw, *Format);
    ASSERT_TRUE(bool(Word));
    // 12 KiB fits an 8-byte chain stride or segmented 12-bit next, but not
    // the 4-byte stride of the 11-bit kernel/firmware next field.
    EXPECT_EQ(Word->Next, Format->Stride == kSlotBytes || Format->Segmented
                              ? kFarSlot / Format->Stride
                              : 0);
    Error = verifyMachOAuthenticatedFixups(Bytes, {Near, Far});
    EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    if (Format->Segmented)
      continue;
    for (int64_t Ordinal = BIND_SPECIAL_DYLIB_WEAK_LOOKUP; Ordinal <= 0;
         ++Ordinal) {
      auto Special = installedFixup(true);
      Special.Address = Far.Address + (1 - Ordinal) * kSlotBytes;
      Special.Recipe.LibraryOrdinal = Ordinal;
      Special.Recipe.Module.clear();
      Error = installMachOAuthenticatedFixups(Bytes, {Special});
      ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
      Error = verifyMachOAuthenticatedFixups(Bytes, {Special});
      EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    }
  }
}

TEST(MachOArm64eInstallation, InstallsFinalNewSegmentAddressesTransactionally) {
  using Kind = llvm::mc_rewrite::RewriteSectionKind;
  for (const auto &Chain : Chains) {
    auto Bytes = largePageImage(linkedImage(Chain));
    const auto Original = Bytes;
    auto Before = readMachODependencyState(Bytes);
    ASSERT_TRUE(bool(Before)) << llvm::toString(Before.takeError());
    MachOPatcher Patcher;
    CompiledImage Image;
    Image.Success = true;
    Image.InputKind = RewriteInputKind::ReplacementObject;
    Image.Format = BinaryFormat::MachO;
    Image.TargetArch = Arch::AArch64;
    Image.BaseVA = Patcher.plannedExecSegmentVA(Bytes, Image.TargetArch);
    ASSERT_EQ(Image.BaseVA, kBase + 2 * kLargePage);
    Image.Bytes.resize(kLargePage + kSlotBytes);
    CompiledSection Code, Data;
    Code.Name = kTextSegment;
    Code.VA = Image.BaseVA;
    Code.Size = kSlotBytes;
    Code.Kind = Kind::Code;
    Data.Name = kDataSegment;
    Data.Offset = kLargePage;
    Data.VA = Image.BaseVA + Data.Offset;
    Data.Size = kSlotBytes;
    Data.Kind = Kind::WritableData;
    Image.Sections = {Code, Data};
    auto Fixup = installedFixup();
    Fixup.Address = Data.VA;
    Fixup.Recipe.Target = Code.VA;
    Fixup.Recipe.AddressDiversity = true;
    auto Bad = Fixup;
    Bad.Address = Code.VA;
    auto Error = Patcher.appendReplacementImage(Bytes, Image, {Bad});
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
    EXPECT_EQ(Bytes, Original);
    Error = Patcher.appendReplacementImage(Bytes, Image, {Fixup});
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    auto After = readMachODependencyState(Bytes);
    ASSERT_TRUE(bool(After)) << llvm::toString(After.takeError());
    Error = preserveMachODependencyState(*Before, *After);
    EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    Error = verifyMachOAuthenticatedFixups(Bytes, {Fixup});
    EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    if (macho_loader::arm64e::chainFormat(Chain.Format)->Segmented) {
      // A segment may appear after LINKEDIT in load-command order while
      // occupying an earlier VM range. Insertion changes its target ordinal
      // even when the chain's owning segment and storage VA stay fixed.
      auto Reordered = Original;
      auto *Header = reinterpret_cast<mach_header_64 *>(Reordered.data());
      segment_command_64 Extra{};
      Extra.cmd = LC_SEGMENT_64;
      Extra.cmdsize = sizeof(Extra);
      std::memcpy(Extra.segname, kExtraSegment, sizeof(kExtraSegment));
      Extra.vmaddr = kBase - kLargePage;
      Extra.vmsize = kLargePage;
      Extra.initprot = Extra.maxprot = VM_PROT_READ;
      std::memcpy(Reordered.data() + sizeof(*Header) + Header->sizeofcmds,
                  &Extra, sizeof(Extra));
      Header->sizeofcmds += sizeof(Extra);
      ++Header->ncmds;
      writeLE<uint64_t>(Reordered.data() + kLargePage + kSlotBytes,
                        kExtraSegmentTarget);
      auto OldState = readMachODependencyState(Reordered);
      ASSERT_TRUE(bool(OldState)) << llvm::toString(OldState.takeError());
      const auto Snapshot = Reordered;
      Error = Patcher.appendReplacementImage(Reordered, Image, {Fixup});
      EXPECT_TRUE(bool(Error));
      llvm::consumeError(std::move(Error));
      EXPECT_EQ(Reordered, Snapshot);
    }
  }
}

TEST(MachOArm64eInstallation,
     RetainsUnauthenticatedWordsInAuthenticatedChains) {
  for (const auto &Chain : Chains) {
    const auto Format = macho_loader::arm64e::chainFormat(Chain.Format);
    const uint64_t Target = kOrdinaryRebaseTarget +
                            (Format->Offset || Format->Segmented ? 0 : kBase);
    auto Bytes =
        linkedImage({Chain.Format, (Chain.First & kNextMask) | Target,
                     Format->Segmented ? kUnauthenticatedSegmentedTarget
                                       : kUnauthenticatedBind});
    auto Before = readMachODependencyState(Bytes);
    ASSERT_TRUE(bool(Before)) << llvm::toString(Before.takeError());
    auto Error = installMachOAuthenticatedFixups(Bytes, {installedFixup()});
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    auto After = readMachODependencyState(Bytes);
    ASSERT_TRUE(bool(After)) << llvm::toString(After.takeError());
    Error = preserveMachODependencyState(*Before, *After);
    EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  }
}

TEST(MachOArm64eInstallation, UpgradesEveryImportTableWithoutChangingBindings) {
  for (const auto &Chain : Chains) {
    if (macho_loader::arm64e::chainFormat(Chain.Format)->Segmented)
      continue;
    for (unsigned Format : {DYLD_CHAINED_IMPORT, DYLD_CHAINED_IMPORT_ADDEND,
                            DYLD_CHAINED_IMPORT_ADDEND64}) {
      auto Bytes = linkedImage(Chain);
      auto *Header = reinterpret_cast<dyld_chained_fixups_header *>(
          Bytes.data() + kChains);
      Header->imports_format = Format;
      auto *Import = Bytes.data() + kChains + kImportTable;
      std::memset(Import, 0, sizeof(dyld_chained_import_addend64));
      writeLE<uint32_t>(Import, 1);
      if (Format == DYLD_CHAINED_IMPORT_ADDEND)
        writeLE<int32_t>(Import + sizeof(uint32_t), kImportAddend);
      if (Format == DYLD_CHAINED_IMPORT_ADDEND64)
        writeLE<int64_t>(Import + sizeof(uint64_t), kImportAddend);
      auto Before = readMachODependencyState(Bytes);
      ASSERT_TRUE(bool(Before)) << llvm::toString(Before.takeError());
      auto Error =
          installMachOAuthenticatedFixups(Bytes, {installedFixup(true)});
      ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
      auto After = readMachODependencyState(Bytes);
      ASSERT_TRUE(bool(After)) << llvm::toString(After.takeError());
      Error = preserveMachODependencyState(*Before, *After);
      EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    }
  }
}

TEST(MachOArm64eInstallation,
     DistinguishesSixteenAndTwentyFourBitImportIndices) {
  for (unsigned Format :
       {DYLD_CHAINED_PTR_ARM64E_USERLAND, DYLD_CHAINED_PTR_ARM64E_USERLAND24}) {
    for (uint32_t Count : {kImportLimit16, kImportLimit16 + 1}) {
      auto Bytes = linkedImage({Format, Chains[0].First, Chains[0].Second});
      const auto Names =
          kImportTable + Count * sizeof(dyld_chained_import_addend64);
      const auto MetadataSize = Names + sizeof(kSymbol);
      const auto StringTable = kChains + MetadataSize;
      Bytes.resize(StringTable + 1);
      for (uint32_t I = 0; I < Count; ++I) {
        auto *Import = Bytes.data() + kChains + kImportTable +
                       I * sizeof(dyld_chained_import_addend64);
        writeLE<uint64_t>(Import, 1);
        writeLE<int64_t>(Import + sizeof(uint64_t), kImportAddend);
      }
      std::memcpy(Bytes.data() + kChains + Names, kSymbol, sizeof(kSymbol));
      auto *Header = reinterpret_cast<dyld_chained_fixups_header *>(
          Bytes.data() + kChains);
      Header->imports_count = Count;
      Header->symbols_offset = Names;
      forEachMachOLoadCommand(
          Bytes.data(), Bytes.size(),
          [&](const uint8_t *Input, uint32_t ID, uint32_t, bool) {
            auto *Command = Bytes.data() + (Input - Bytes.data());
            if (ID == LC_SEGMENT_64) {
              auto *Segment = reinterpret_cast<segment_command_64 *>(Command);
              if (readMachOName(Segment->segname) == kLinkeditSegment) {
                Segment->filesize = Bytes.size() - kLinkedit;
                Segment->vmsize = llvm::alignTo(Segment->filesize, kLargePage);
              }
            } else if (ID == LC_SYMTAB) {
              auto *Symbols = reinterpret_cast<symtab_command *>(Command);
              Symbols->symoff = Symbols->stroff = StringTable;
            } else if (ID == LC_DYLD_CHAINED_FIXUPS) {
              reinterpret_cast<linkedit_data_command *>(Command)->datasize =
                  MetadataSize;
            }
          });
      const auto Original = Bytes;
      auto Error =
          installMachOAuthenticatedFixups(Bytes, {installedFixup(true)});
      if (Count > kImportLimit16 &&
          Format == DYLD_CHAINED_PTR_ARM64E_USERLAND) {
        EXPECT_TRUE(bool(Error));
        llvm::consumeError(std::move(Error));
        EXPECT_EQ(Bytes, Original);
      } else {
        ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
        EXPECT_EQ(readLE<uint64_t>(Bytes.data() + kInstalledSlot) &
                      kImportLimit16,
                  Count & kImportLimit16);
        Error = verifyMachOAuthenticatedFixups(Bytes, {installedFixup(true)});
        EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
      }
    }
  }
}

TEST(MachOArm64eInstallation, RetainsOpaqueTerminalSignatureForPublication) {
  auto Original = linkedImage(Chains[0]);
  auto *Header = reinterpret_cast<mach_header_64 *>(Original.data());
  const auto CommandOffset = sizeof(*Header) + Header->sizeofcmds;
  linkedit_data_command Signature{};
  Signature.cmd = LC_CODE_SIGNATURE;
  Signature.cmdsize = sizeof(Signature);
  Signature.dataoff = Original.size();
  Signature.datasize = kSlotBytes;
  std::memcpy(Original.data() + CommandOffset, &Signature, sizeof(Signature));
  ++Header->ncmds;
  Header->sizeofcmds += sizeof(Signature);
  Original.resize(Original.size() + Signature.datasize);
  // The layout owner keeps this opaque staged signature; publication alone
  // owns cryptographic validity and final code-limit/hash replacement.
  writeLE<uint64_t>(Original.data() + Signature.dataoff, Chains[0].First);
  forEachMachOLoadCommand(
      Original.data(), Original.size(),
      [&](const uint8_t *Input, uint32_t ID, uint32_t, bool) {
        if (ID != LC_SEGMENT_64)
          return;
        auto Segment = readMachOSegment(Input, true);
        if (readMachOName(Segment.SegName) != kLinkeditSegment)
          return;
        Segment.FileSize += Signature.datasize;
        Segment.VMSize = llvm::alignTo(Segment.FileSize, kPage);
        writeMachOSegment(Original.data() + (Input - Original.data()), true,
                          Segment);
      });
  auto Bytes = Original;
  auto Error = installMachOAuthenticatedFixups(Bytes, {installedFixup()});
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  const auto *Moved = reinterpret_cast<const linkedit_data_command *>(
      Bytes.data() + CommandOffset);
  EXPECT_EQ(uint64_t(Moved->dataoff) + Moved->datasize, Bytes.size());
  EXPECT_EQ(readLE<uint64_t>(Bytes.data() + Moved->dataoff), Chains[0].First);
  EXPECT_EQ(readLE<uint64_t>(Bytes.data() + Signature.dataoff),
            Chains[0].First);
  for (uint32_t Offset :
       {uint32_t(0), Signature.dataoff - 1, Signature.dataoff + 1}) {
    auto Bad = Original;
    writeLE<uint32_t>(Bad.data() + CommandOffset +
                          offsetof(linkedit_data_command, dataoff),
                      Offset);
    const auto Input = Bad;
    Error = installMachOAuthenticatedFixups(Bad, {installedFixup()});
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
    EXPECT_EQ(Bad, Input);
  }
}

TEST(MachOArm64eInstallation, InstallsOrdinaryPointersAlongsideSignedChains) {
  for (bool Threaded : {false, true}) {
    for (const auto &Chain : Chains) {
      const auto Format = macho_loader::arm64e::chainFormat(Chain.Format);
      if (Threaded && Chain.Format != DYLD_CHAINED_PTR_ARM64E)
        continue;
      for (bool Bind : {false, true}) {
        for (int64_t Addend : {kImportAddend, int64_t(kNewImportAddend)}) {
          auto Bytes = Threaded ? threadedImage() : multiStartImage(Chain);
          const auto Before = Bytes;
          auto Fixup = installedFixup(Bind);
          Fixup.Authenticated = false;
          Fixup.Recipe.Diversity = 0;
          if (Bind)
            Fixup.Recipe.Addend = Addend;
          auto Error = installMachOAuthenticatedFixups(Bytes, {Fixup});
          if (Bind && Format->Segmented) {
            EXPECT_TRUE(bool(Error));
            llvm::consumeError(std::move(Error));
            EXPECT_EQ(Bytes, Before);
            continue;
          }
          ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
          const uint64_t Target = Format->Offset || Format->Segmented
                                      ? kInstalledTarget
                                      : kBase + kInstalledTarget;
          EXPECT_EQ(readLE<uint64_t>(Bytes.data() + kInstalledSlot),
                    Bind ? kBindMask | 1 : Target);
          Error = verifyMachOAuthenticatedFixups(Bytes, {Fixup});
          ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
          auto Parsed =
              Loader::create(BinaryFormat::MachO)
                  ->loadBuffer(llvm::MemoryBufferRef(
                      llvm::toStringRef(llvm::ArrayRef(Bytes)), kNewSymbol));
          ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
          EXPECT_FALSE(
              Parsed->MachOAuthenticatedPointers.contains(Fixup.Address));
          if (Bind) {
            auto Slot = Parsed->DyldBindSlots.find(Fixup.Address);
            ASSERT_NE(Slot, Parsed->DyldBindSlots.end());
            EXPECT_EQ(Slot->second.Name, kNewSymbol);
            EXPECT_EQ(Slot->second.Addend, Addend);
            EXPECT_EQ(Slot->second.Module, kLibrary);
          } else {
            auto Pointer = Parsed->readVA(Fixup.Address, kSlotBytes);
            ASSERT_NE(Pointer, nullptr);
            EXPECT_EQ(readLE<uint64_t>(Pointer), Fixup.Recipe.Target);
          }
          // Ordinary slots cannot satisfy a signing request or silently carry
          // authentication discriminators left over from a different recipe.
          Fixup.Authenticated = true;
          Error = verifyMachOAuthenticatedFixups(Bytes, {Fixup});
          EXPECT_TRUE(bool(Error));
          llvm::consumeError(std::move(Error));
          Fixup.Authenticated = false;
          Fixup.Recipe.Diversity = kInstalledDiversity;
          auto Corrupt = Before;
          Error = installMachOAuthenticatedFixups(Corrupt, {Fixup});
          EXPECT_TRUE(bool(Error));
          llvm::consumeError(std::move(Error));
          EXPECT_EQ(Corrupt, Before);
        }
      }
    }
  }
}

} // namespace

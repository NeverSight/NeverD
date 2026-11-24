//===- MachOStubRunTests.cpp - Mach-O symbol stub runs --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The stubs a symbol stub section holds where its header's stub size and
/// the indirect symbol table disagree.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/MachO/MachOLoaderUtils.h"

#include "llvm/BinaryFormat/MachO.h"

#include <cstdint>
#include <vector>

using namespace neverd;
using macho_loader::SectionInfo;
using macho_loader::StubRun;
using macho_loader::stubRun;

namespace {

SectionInfo section(uint32_t Type, uint64_t Size, uint32_t FirstSymbol,
                    uint32_t StubSize = 0) {
  SectionInfo Info;
  Info.Flags = Type;
  Info.Size = Size;
  Info.Reserved1 = FirstSymbol;
  Info.StubSize = StubSize;
  return Info;
}

TEST(MachOStubRun, TheSymbolsASectionOwnsCorrectAStubSizeThatClaimsMore) {
  // Go's linker writes 6, x86-64's stub size, for arm64's `__symbol_stub1`
  // too: 58 stubs of three instructions, then a `__got` whose symbols start
  // at 58.  By the header the section would hold 116 stubs and take the
  // `__got`'s symbols, and the stub `runtime.mmap_trampoline` calls named
  // mach_timebase_info.
  const std::vector<SectionInfo> Go = {
      section(llvm::MachO::S_SYMBOL_STUBS, 58 * 12, 0, 6),
      section(llvm::MachO::S_NON_LAZY_SYMBOL_POINTERS, 58 * 8, 58)};
  const StubRun Run = stubRun(Go, 0, 116);
  EXPECT_EQ(Run.Count, 58u);
  EXPECT_EQ(Run.Bytes, 12u);
}

TEST(MachOStubRun, AHeaderTheTableAgreesWithStands) {
  // x86-64 stubs are six bytes, and the section's run holds one symbol a
  // stub.
  const std::vector<SectionInfo> X64 = {
      section(llvm::MachO::S_SYMBOL_STUBS, 58 * 6, 0, 6),
      section(llvm::MachO::S_NON_LAZY_SYMBOL_POINTERS, 58 * 8, 58)};
  StubRun Run = stubRun(X64, 0, 116);
  EXPECT_EQ(Run.Count, 58u);
  EXPECT_EQ(Run.Bytes, 6u);

  // The stubs come last: the table's end bounds their run.
  const std::vector<SectionInfo> Last = {
      section(llvm::MachO::S_NON_LAZY_SYMBOL_POINTERS, 4 * 8, 0),
      section(llvm::MachO::S_SYMBOL_STUBS, 3 * 12, 4, 6)};
  Run = stubRun(Last, 1, 7);
  EXPECT_EQ(Run.Count, 3u);
  EXPECT_EQ(Run.Bytes, 12u);

  // A section of another kind owns no run, and one that owns none of the
  // table's symbols bounds nothing.
  const std::vector<SectionInfo> Mixed = {
      section(llvm::MachO::S_SYMBOL_STUBS, 4 * 12, 0, 12),
      section(llvm::MachO::S_REGULAR, 64, 2),
      section(llvm::MachO::S_LAZY_SYMBOL_POINTERS, 0, 1)};
  Run = stubRun(Mixed, 0, 4);
  EXPECT_EQ(Run.Count, 4u);
  EXPECT_EQ(Run.Bytes, 12u);
}

TEST(MachOStubRun, ARunThatDoesNotDivideTheSectionLeavesTheHeader) {
  // Five owned symbols do not divide 36 bytes: nothing says what the stubs
  // are, and the header's six stubs stay, bounded by the table as before.
  const std::vector<SectionInfo> Odd = {
      section(llvm::MachO::S_SYMBOL_STUBS, 36, 0, 6),
      section(llvm::MachO::S_NON_LAZY_SYMBOL_POINTERS, 5 * 8, 5)};
  StubRun Run = stubRun(Odd, 0, 10);
  EXPECT_EQ(Run.Count, 6u);
  EXPECT_EQ(Run.Bytes, 6u);

  // No stub size, no stubs.
  const std::vector<SectionInfo> None = {
      section(llvm::MachO::S_SYMBOL_STUBS, 36, 0, 0)};
  Run = stubRun(None, 0, 10);
  EXPECT_EQ(Run.Count, 0u);
}

} // namespace

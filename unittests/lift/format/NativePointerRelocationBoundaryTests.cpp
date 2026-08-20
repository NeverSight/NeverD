//===- NativePointerRelocationBoundaryTests.cpp --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"
#include "gtest/gtest.h"

#include "neverd/loader/PointerRelocation.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <filesystem>
#include <regex>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace neverd;

class NativePointerRelocationBoundary : public NeverDLiftTest {
protected:
  static fs::path fixture(const char *Name) {
    return fs::path(TEST_OBJ_DIR) / Name;
  }

  static bool hasBaseRelocation(const BinaryImage &Img, va_t Address) {
    return std::any_of(
        Img.BaseRelocations.begin(), Img.BaseRelocations.end(),
        [&](const BaseRelocation &R) { return R.Address == Address; });
  }
};

TEST_F(NativePointerRelocationBoundary,
       SharedClassifierSeparatesDataSlotsFromCodeImmediates) {
  BinaryImage Img;
  Img.Format = BinaryFormat::ELF;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;

  auto AddSegment = [&](const char *Name, va_t VA, size_t Size,
                        SegmentFlags Flags, bool FileBacked = true) {
    Segment S;
    S.Name = Name;
    S.VA = VA;
    S.Size = Size;
    S.FileSz = FileBacked ? Size : 0;
    S.Flags = Flags;
    if (FileBacked)
      S.Data.resize(Size);
    Img.Segments.push_back(std::move(S));
  };
  AddSegment(".text", 0x1000, 0x40,
             SegmentFlags::Readable | SegmentFlags::Executable);
  AddSegment(".rodata", 0x2000, 0x80, SegmentFlags::Readable);
  AddSegment(".data", 0x3000, 0x40,
             SegmentFlags::Readable | SegmentFlags::Writable);
  AddSegment(".bss", 0x4000, 0x40,
             SegmentFlags::Readable | SegmentFlags::Writable,
             /*FileBacked=*/false);

  EXPECT_TRUE(recordAbsolutePointerRelocation(Img, 0x2000, 0x2020));
  EXPECT_TRUE(recordAbsolutePointerRelocation(Img, 0x2008, 0x3000));
  EXPECT_TRUE(recordAbsolutePointerRelocation(Img, 0x2010, 0x4000));
  EXPECT_TRUE(recordAbsolutePointerRelocation(Img, 0x2018, 0x1000));
  EXPECT_TRUE(recordAbsolutePointerRelocation(Img, 0x1008, 0x2028));
  EXPECT_TRUE(recordAbsolutePointerRelocation(Img, 0x1010, 0x1000));

  EXPECT_EQ(Img.DataPtrRelocSlots, (std::set<va_t>{0x2000, 0x2008, 0x2010}));
  EXPECT_EQ(Img.CodePtrRelocSlots, (std::set<va_t>{0x2018}));
  EXPECT_EQ(Img.RelocDataAddrs, (std::set<va_t>{0x2020, 0x2028}));
  EXPECT_EQ(Img.WritableRelocDataAddrs, (std::set<va_t>{0x3000, 0x4000}));
  EXPECT_EQ(Img.CodeRefTargets, (std::set<va_t>{0x1000}));
  EXPECT_FALSE(Img.hasObjectDataProvenance(0));
  EXPECT_FALSE(Img.hasObjectDataProvenance(0x1000));
  EXPECT_TRUE(Img.hasObjectDataProvenance(0x2000));
  EXPECT_TRUE(Img.hasObjectDataProvenance(0x3000));
  EXPECT_FALSE(Img.hasObjectDataProvenance(0x4000));
  EXPECT_TRUE(Img.isPotentiallyRelocatableAddress(0x2000));
  EXPECT_TRUE(Img.isPotentiallyRelocatableAddress(0x2080));
  EXPECT_TRUE(Img.isPotentiallyRelocatableAddress(0x4000));
  EXPECT_TRUE(Img.isPotentiallyRelocatableAddress(0x4040));
  EXPECT_FALSE(Img.isPotentiallyRelocatableAddress(0));
  EXPECT_FALSE(Img.isPotentiallyRelocatableAddress(0x1040));
  for (va_t Slot : {0x2000, 0x2008, 0x2010, 0x2018})
    EXPECT_TRUE(Img.hasRelocationProvenanceAt(Slot));
  EXPECT_FALSE(Img.hasRelocationProvenanceAt(0x2020));

  Img.Relocations.push_back(RelocationEntry{.Address = 0x1020});
  Img.BaseRelocations.push_back(BaseRelocation{.Address = 0x1028});
  Img.RelDataPtrRelocSlots.insert(0x2030);
  Img.RelCodeRelocSlots.insert(0x2038);
  Img.ImportPtrSlots[0x2040] = "_import";
  Img.DyldBindSlots[0x2048] = {"_bind", 0};
  for (va_t Slot : {0x1020, 0x1028, 0x2030, 0x2038, 0x2040, 0x2048})
    EXPECT_TRUE(Img.hasRelocationProvenanceAt(Slot));
}

TEST_F(NativePointerRelocationBoundary,
       PEBaseRelocationsClassifyLocalDataPointerSlots) {
  struct Case {
    const char *Name;
    Arch TargetArch;
    uint32_t PointerSize;
  };
  for (const Case &C : {Case{"test_pe_pointer_reloc_x64.exe", Arch::X64, 8},
                        Case{"test_pe_pointer_reloc_x86.exe", Arch::X86, 4}}) {
    SCOPED_TRACE(C.Name);
    const fs::path Path = fixture(C.Name);
    if (!fs::exists(Path))
      GTEST_SKIP() << C.Name << " not built";

    auto ImgOrErr = loadBinary(Path);
    ASSERT_TRUE(static_cast<bool>(ImgOrErr))
        << llvm::toString(ImgOrErr.takeError());
    const BinaryImage &Img = *ImgOrErr;
    ASSERT_EQ(Img.Format, BinaryFormat::COFF);
    ASSERT_EQ(Img.Arch, C.TargetArch);
    ASSERT_EQ(Img.getPointerSize(), C.PointerSize);

    const Section *Table = Img.getSectionByName(".rdata");
    ASSERT_NE(Table, nullptr);
    const va_t Slot0 = Table->VA;
    const va_t Slot1 = Slot0 + C.PointerSize;
    const va_t Target0 = Slot0 + 2 * C.PointerSize;
    const va_t Target1 = Target0 + sizeof(uint32_t);

    ASSERT_NE(Img.readVA(Slot0, 2 * C.PointerSize), nullptr);
    EXPECT_EQ(readPtr(Img.readVA(Slot0, C.PointerSize), C.PointerSize == 8),
              Target0);
    EXPECT_EQ(readPtr(Img.readVA(Slot1, C.PointerSize), C.PointerSize == 8),
              Target1);
    EXPECT_TRUE(hasBaseRelocation(Img, Slot0));
    EXPECT_TRUE(hasBaseRelocation(Img, Slot1));
    EXPECT_TRUE(Img.hasRelocationProvenanceAt(Slot0));
    EXPECT_TRUE(Img.hasRelocationProvenanceAt(Slot1));
    EXPECT_EQ(Img.DataPtrRelocSlots.count(Slot0), 1u);
    EXPECT_EQ(Img.DataPtrRelocSlots.count(Slot1), 1u);
    EXPECT_EQ(Img.RelocDataAddrs.count(Target0), 1u);
    EXPECT_EQ(Img.RelocDataAddrs.count(Target1), 1u);
  }
}

TEST_F(NativePointerRelocationBoundary,
       ELFDynamicRelativeRelocationsAreAppliedAndClassified) {
  for (const char *Name :
       {"test_elf_pointer_reloc_x64", "test_elf_pointer_relr_x64",
        "test_elf_pointer_android_relr_x64",
        "test_elf_pointer_android_packed_x64"}) {
    SCOPED_TRACE(Name);
    const fs::path Path = fixture(Name);
    if (!fs::exists(Path))
      GTEST_SKIP() << Name << " not built";

    auto ImgOrErr = loadBinary(Path);
    ASSERT_TRUE(static_cast<bool>(ImgOrErr))
        << llvm::toString(ImgOrErr.takeError());
    const BinaryImage &Img = *ImgOrErr;
    ASSERT_EQ(Img.Format, BinaryFormat::ELF);
    ASSERT_EQ(Img.Arch, Arch::X64);
    ASSERT_FALSE(Img.IsRelocatable);

    const Section *Table = Img.getSectionByName(".data.rel.ro");
    const Section *Values = Img.getSectionByName(".rodata");
    ASSERT_NE(Table, nullptr);
    ASSERT_NE(Values, nullptr);
    const va_t Slot0 = Table->VA;
    const va_t Slot1 = Slot0 + sizeof(uint64_t);
    const va_t Target0 = Values->VA;
    const va_t Target1 = Target0 + sizeof(uint32_t);

    ASSERT_NE(Img.readVA(Slot0, 2 * sizeof(uint64_t)), nullptr);
    EXPECT_EQ(readLE<uint64_t>(Img.readVA(Slot0, sizeof(uint64_t))), Target0);
    EXPECT_EQ(readLE<uint64_t>(Img.readVA(Slot1, sizeof(uint64_t))), Target1);
    EXPECT_TRUE(hasBaseRelocation(Img, Slot0));
    EXPECT_TRUE(hasBaseRelocation(Img, Slot1));
    EXPECT_TRUE(Img.hasRelocationProvenanceAt(Slot0));
    EXPECT_TRUE(Img.hasRelocationProvenanceAt(Slot1));
    EXPECT_EQ(Img.DataPtrRelocSlots.count(Slot0), 1u);
    EXPECT_EQ(Img.DataPtrRelocSlots.count(Slot1), 1u);
    EXPECT_EQ(Img.RelocDataAddrs.count(Target0), 1u);
    EXPECT_EQ(Img.RelocDataAddrs.count(Target1), 1u);

    if (std::string(Name) == "test_elf_pointer_reloc_x64" ||
        std::string(Name) == "test_elf_pointer_android_packed_x64") {
      for (const auto &[Slot, Target] :
           {std::pair{Slot0, Target0}, std::pair{Slot1, Target1}}) {
        auto It = std::find_if(
            Img.Relocations.begin(), Img.Relocations.end(),
            [&](const RelocationEntry &R) { return R.Address == Slot; });
        ASSERT_NE(It, Img.Relocations.end());
        EXPECT_TRUE(It->HasExplicitAddend);
        EXPECT_EQ(static_cast<uint64_t>(It->Addend), Target);
      }
    }
  }
}

TEST_F(NativePointerRelocationBoundary,
       ELFRelocatableLookupIgnoresUnmappedSectionAddressAliases) {
  const fs::path Path = fixture("test_elf_pointer_reloc_arm32.o");
  ASSERT_TRUE(fs::exists(Path));
  auto ImgOrErr = loadBinary(Path);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;
  ASSERT_EQ(Img.Format, BinaryFormat::ELF);
  ASSERT_EQ(Img.Arch, Arch::ARM);
  ASSERT_TRUE(Img.IsRelocatable);

  const Section *Table = Img.getSectionByName(".data.rel.ro");
  const Section *StringTable = Img.getSectionByName(".strtab");
  ASSERT_NE(Table, nullptr);
  ASSERT_NE(StringTable, nullptr);
  ASSERT_TRUE(Table->isReadable());
  ASSERT_FALSE(StringTable->isReadable());

  // ET_REL metadata sections retain VA zero.  This fixture deliberately uses
  // long symbol names so .strtab numerically covers the synthesized table VA;
  // a mapped-address lookup must nevertheless return the SHF_ALLOC table.
  ASSERT_TRUE(StringTable->contains(Table->VA));
  for (unsigned I = 0; I < 3; ++I) {
    const va_t Slot = Table->VA + I * sizeof(uint32_t);
    EXPECT_EQ(Img.getSectionFor(Slot), Table);
    EXPECT_EQ(Img.CodePtrRelocSlots.count(Slot), 1u);
    EXPECT_TRUE(Img.hasRelocationProvenanceAt(Slot));
  }
}

TEST_F(NativePointerRelocationBoundary,
       LiftedPEAndELFUseRelocatablePointerMirrors) {
  for (const char *Name :
       {"test_pe_pointer_reloc_x64.exe", "test_elf_pointer_reloc_x64",
        "test_pe_pointer_reloc_x86.exe", "test_elf_pointer_relr_x64",
        "test_elf_pointer_android_relr_x64",
        "test_elf_pointer_android_packed_x64"}) {
    const fs::path Path = fixture(Name);
    if (!fs::exists(Path))
      GTEST_SKIP() << Name << " not built";
    RunResult R = liftToLLVMIR(Path);
    ASSERT_EQ(R.exitCode, 0) << Name << ": " << R.err;
    EXPECT_NE(R.out.find("@__nd_codeptr_"), std::string::npos)
        << Name << " retained a raw pointer-table byte array:\n"
        << R.out;
    EXPECT_NE(R.out.find("ptrtoint"), std::string::npos) << Name;
  }
}

TEST_F(NativePointerRelocationBoundary,
       RematerializedTableBaseRecurrenceLiftsAcrossELFAndPE) {
  struct Case {
    const char *Name;
    BinaryFormat Format;
    Arch TargetArch;
    const char *TableSection;
  };
  for (const Case &C : {
           Case{"test_rematerialized_table_base_a64_elf", BinaryFormat::ELF,
                Arch::AArch64, ".data.rel.ro"},
           Case{"test_rematerialized_table_base_x64_elf", BinaryFormat::ELF,
                Arch::X64, ".data.rel.ro"},
           Case{"test_rematerialized_table_base_a64_pe.exe", BinaryFormat::COFF,
                Arch::AArch64, ".rdata"},
           Case{"test_rematerialized_table_base_x64_pe.exe", BinaryFormat::COFF,
                Arch::X64, ".rdata"},
       }) {
    SCOPED_TRACE(C.Name);
    const fs::path Path = fixture(C.Name);
    if (!fs::exists(Path))
      GTEST_SKIP() << C.Name << " not built";

    auto ImgOrErr = loadBinary(Path);
    ASSERT_TRUE(static_cast<bool>(ImgOrErr))
        << llvm::toString(ImgOrErr.takeError());
    const BinaryImage &Img = *ImgOrErr;
    ASSERT_EQ(Img.Format, C.Format);
    ASSERT_EQ(Img.Arch, C.TargetArch);
    ASSERT_EQ(Img.getPointerSize(), 8u);

    const Section *Table = Img.getSectionByName(C.TableSection);
    ASSERT_NE(Table, nullptr);
    ASSERT_TRUE(Table->isReadable());
    ASSERT_FALSE(Table->isExecutable());
    if (Table->isWritable())
      EXPECT_TRUE(section_names::isReadOnlyAfterRelocSectionName(Table->Name));

    const va_t Slot0 = Table->VA;
    const va_t Slot1 = Slot0 + Img.getPointerSize();
    const uint8_t *SlotBytes = Img.readVA(Slot0, 2 * Img.getPointerSize());
    ASSERT_NE(SlotBytes, nullptr);
    const va_t Target0 = readPtr(SlotBytes, /*Is64=*/true);
    const va_t Target1 =
        readPtr(SlotBytes + Img.getPointerSize(), /*Is64=*/true);
    EXPECT_EQ(Target0, Target1);
    EXPECT_NE(Img.getSectionFor(Target0), nullptr);
    EXPECT_TRUE(Img.hasRelocationProvenanceAt(Slot0));
    EXPECT_TRUE(Img.hasRelocationProvenanceAt(Slot1));
    EXPECT_EQ(Img.DataPtrRelocSlots.count(Slot0), 1u);
    EXPECT_EQ(Img.DataPtrRelocSlots.count(Slot1), 1u);
    EXPECT_EQ(Img.RelocDataAddrs.count(Target0), 1u);

    RunResult R = liftToLLVMIRUnopt(Path);
    ASSERT_EQ(R.exitCode, 0) << R.err;
    EXPECT_EQ(R.err.find("refusing stale-address fallback"), std::string::npos)
        << R.err;
    if (C.Format == BinaryFormat::ELF)
      EXPECT_NE(R.out.find("@probe("), std::string::npos) << R.out;
    EXPECT_NE(R.out.find("@__nd_codeptr_"), std::string::npos) << R.out;
    EXPECT_NE(R.out.find("getelementptr"), std::string::npos) << R.out;
  }
}

TEST_F(NativePointerRelocationBoundary,
       ReentrantFeasibleEdgePhiPreservesRelocatableAddressRelations) {
  struct Case {
    const char *Name;
    BinaryFormat Format;
  };
  unsigned TestedCases = 0;
  for (const Case &C : {
#if defined(__APPLE__)
           Case{"test_reentrant_feasible_phi_a64_macho", BinaryFormat::MachO},
#endif
           Case{"test_reentrant_feasible_phi_a64_elf", BinaryFormat::ELF},
           Case{"test_reentrant_feasible_phi_a64_pe.exe", BinaryFormat::COFF},
       }) {
    SCOPED_TRACE(C.Name);
    const fs::path Path = fixture(C.Name);
    if (!fs::exists(Path))
      continue;
    ++TestedCases;

    auto ImgOrErr = loadBinary(Path);
    ASSERT_TRUE(static_cast<bool>(ImgOrErr))
        << llvm::toString(ImgOrErr.takeError());
    const BinaryImage &Img = *ImgOrErr;
    ASSERT_EQ(Img.Format, C.Format);
    ASSERT_EQ(Img.Arch, Arch::AArch64);
    ASSERT_EQ(Img.getPointerSize(), 8u);

    RunResult Med = liftToMedIR(Path);
    ASSERT_EQ(Med.exitCode, 0) << Med.err;
    EXPECT_EQ(Med.err.find("ambiguous reachable read-only table-base PHI"),
              std::string::npos)
        << Med.err;
    EXPECT_NE(Med.out.find("PHI X13"), std::string::npos) << Med.out;
    EXPECT_TRUE(std::regex_search(
        Med.out, std::regex(R"(block 0 succs=\[[0-9]+,[0-9]+\])")))
        << Med.out;
    RunResult LLVM = liftToLLVMIRUnopt(Path);
    ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
    EXPECT_EQ(LLVM.err.find("refusing stale-address fallback"),
              std::string::npos)
        << LLVM.err;
    auto lineHasTwoRelocatableOperands = [&](std::string_view Opcode) {
      size_t LineStart = 0;
      while (LineStart < LLVM.out.size()) {
        size_t LineEnd = LLVM.out.find('\n', LineStart);
        if (LineEnd == std::string::npos)
          LineEnd = LLVM.out.size();
        std::string_view Line(LLVM.out.data() + LineStart, LineEnd - LineStart);
        if (Line.find(Opcode) != std::string_view::npos) {
          const size_t First = Line.find("ptrtoint");
          if (First != std::string_view::npos &&
              Line.find("ptrtoint", First + 1) != std::string_view::npos)
            return true;
        }
        LineStart = LineEnd + 1;
      }
      return false;
    };
    EXPECT_TRUE(lineHasTwoRelocatableOperands("sub")) << LLVM.out;
    EXPECT_TRUE(lineHasTwoRelocatableOperands("icmp")) << LLVM.out;

#if defined(__APPLE__) && defined(__aarch64__)
    if (C.Format == BinaryFormat::MachO)
      for (unsigned Attempt = 0; Attempt < 3; ++Attempt) {
        RunResult Native = exec(Path.string(), {});
        EXPECT_EQ(Native.exitCode, 7) << Native.err;
      }
#endif
  }
  if (TestedCases == 0)
    GTEST_SKIP() << "cross-format fixtures not built";
}

TEST_F(NativePointerRelocationBoundary,
       WritablePointerTableInductionLiftsAcrossFormatsAndArchitectures) {
  struct Case {
    const char *Name;
    BinaryFormat Format;
    Arch TargetArch;
    const char *TableSection;
  };
  for (const Case &C : {
#if defined(__APPLE__)
           Case{"test_writable_pointer_table_induction_a64_macho",
                BinaryFormat::MachO, Arch::AArch64, "__data"},
           Case{"test_writable_pointer_table_induction_x64_macho",
                BinaryFormat::MachO, Arch::X64, "__data"},
#endif
           Case{"test_writable_pointer_table_induction_a64_elf",
                BinaryFormat::ELF, Arch::AArch64, ".data"},
           Case{"test_writable_pointer_table_induction_x64_elf",
                BinaryFormat::ELF, Arch::X64, ".data"},
           Case{"test_writable_pointer_table_induction_a64_pe.exe",
                BinaryFormat::COFF, Arch::AArch64, ".data"},
           Case{"test_writable_pointer_table_induction_x64_pe.exe",
                BinaryFormat::COFF, Arch::X64, ".data"},
       }) {
    SCOPED_TRACE(C.Name);
    const fs::path Path = fixture(C.Name);
    if (!fs::exists(Path))
      GTEST_SKIP() << C.Name << " not built";

    auto ImgOrErr = loadBinary(Path);
    ASSERT_TRUE(static_cast<bool>(ImgOrErr))
        << llvm::toString(ImgOrErr.takeError());
    const BinaryImage &Img = *ImgOrErr;
    ASSERT_EQ(Img.Format, C.Format);
    ASSERT_EQ(Img.Arch, C.TargetArch);
    ASSERT_EQ(Img.getPointerSize(), 8u);

    const Section *Table = Img.getSectionByName(C.TableSection);
    ASSERT_NE(Table, nullptr);
    ASSERT_TRUE(Table->isReadable());
    ASSERT_TRUE(Table->isWritable());
    ASSERT_FALSE(Table->isExecutable());
    const va_t Slot0 = Table->VA;
    const va_t Slot1 = Slot0 + Img.getPointerSize();
    const uint8_t *Slots = Img.readVA(Slot0, 2 * Img.getPointerSize());
    ASSERT_NE(Slots, nullptr);
    const va_t Target0 = readPtr(Slots, /*Is64=*/true);
    const va_t Target1 = readPtr(Slots + Img.getPointerSize(), /*Is64=*/true);
    ASSERT_NE(Target0, Target1);
    ASSERT_NE(Img.getSectionFor(Target0), nullptr);
    ASSERT_NE(Img.getSectionFor(Target1), nullptr);
    ASSERT_NE(Img.readVA(Target0, sizeof(uint32_t)), nullptr);
    ASSERT_NE(Img.readVA(Target1, sizeof(uint32_t)), nullptr);
    EXPECT_EQ(readLE<uint32_t>(Img.readVA(Target0, sizeof(uint32_t))), 3u);
    EXPECT_EQ(readLE<uint32_t>(Img.readVA(Target1, sizeof(uint32_t))), 4u);
    EXPECT_TRUE(Img.hasRelocationProvenanceAt(Slot0));
    EXPECT_TRUE(Img.hasRelocationProvenanceAt(Slot1));
    EXPECT_EQ(Img.DataPtrRelocSlots.count(Slot0), 1u);
    EXPECT_EQ(Img.DataPtrRelocSlots.count(Slot1), 1u);

    RunResult Med = liftToMedIR(Path);
    ASSERT_EQ(Med.exitCode, 0) << Med.err;
    EXPECT_NE(Med.out.find("PHI"), std::string::npos) << Med.out;
    EXPECT_NE(Med.out.find("INT_ADD"), std::string::npos) << Med.out;

    RunResult LLVM = liftToLLVMIRUnopt(Path);
    ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
    EXPECT_EQ(LLVM.err.find("refusing stale-address fallback"),
              std::string::npos)
        << LLVM.err;
    EXPECT_NE(LLVM.out.find("@__nd_codeptr_"), std::string::npos) << LLVM.out;
    EXPECT_NE(LLVM.out.find("%cptsel"), std::string::npos) << LLVM.out;

#if defined(__APPLE__) && defined(__aarch64__)
    if (C.Format == BinaryFormat::MachO && C.TargetArch == Arch::AArch64)
      for (unsigned Attempt = 0; Attempt < 3; ++Attempt) {
        RunResult Native = exec(Path.string(), {});
        EXPECT_EQ(Native.exitCode, 7) << Native.err;
      }
#endif
  }
}

} // namespace

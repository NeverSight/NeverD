//===- MIPSLoaderTests.cpp - MIPS ELF profile and mapping tests ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independently encoded ELF fixtures exercise width, byte order and ABI
/// admission without depending on a host MIPS cross compiler.
///
//===----------------------------------------------------------------------===//

#include "MIPSFixtures.h"
#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/loader/DWARF/EHFrame.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/loader/ELF/ELFProgramMetadata.h"
#include "neverd/loader/FunctionDiscovery.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/MipsABIFlags.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

using namespace neverd;

namespace {

using mips_test::ELFBytes;

class MIPSLoader : public ::testing::TestWithParam<Arch> {};

TEST_P(MIPSLoader, MapsLinkedImageAndReadsTargetOrderPointers) {
  ELFBytes File(GetParam());
  ELFLoader Loader;
  EXPECT_EQ(llvm::toString(Loader.validateBuffer(File.buffer())), "");
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->Arch, GetParam());
  EXPECT_EQ(Image->getPointerSize(), File.Is64 ? 8u : 4u);
  EXPECT_EQ(Image->Entry, 0x10100u);
  ASSERT_TRUE(Image->MIPS);
  EXPECT_EQ(Image->MIPS->ABI, File.Is64 ? MIPSABI::N64 : MIPSABI::O32);
  EXPECT_FALSE(Image->MIPS->HasABIFlags);
  EXPECT_FALSE(Image->MIPS->EntryGP);
  const uint64_t Expected =
      File.Is64 ? (File.Little ? 0x8877665544332211ull : 0x1122334455667788ull)
                : (File.Little ? 0x44332211ull : 0x11223344ull);
  const auto Values = Image->readPtrArray(0x10110, 1);
  ASSERT_EQ(Values.size(), 1u);
  EXPECT_EQ(Values.front(), Expected);
  ASSERT_TRUE(Image->patchPtr(0x10110, 0x1020304050607080ull));
  const uint8_t *Patched = Image->readVA(0x10110, File.Is64 ? 8 : 4);
  ASSERT_NE(Patched, nullptr);
  EXPECT_EQ(Patched[0], File.Little ? 0x80 : (File.Is64 ? 0x10 : 0x50));
  EXPECT_EQ(Patched[(File.Is64 ? 8 : 4) - 1],
            File.Little ? (File.Is64 ? 0x10 : 0x50) : 0x80);
}

TEST(MIPSN32Loader, IdentifiesELF32With64BitGPRsInBothByteOrders) {
  for (const bool Little : {false, true}) {
    ELFBytes File(Little ? Arch::MIPS32EL : Arch::MIPS32);
    File.abiFlags();
    File.put(36, 0xa0000020, 4); // MIPS64r6, ABI2, ELFCLASS32.
    File.Bytes[0xe2] = 64;
    File.Bytes[0xe3] = 6;
    File.Bytes[0xe4] = File.Bytes[0xe5] = 2;
    File.Bytes[0xe7] = 1;
    ELFLoader Loader;
    std::vector<LoadCandidate> Rows;
    ELFLoader::identify(File.buffer(), Rows);
    ASSERT_EQ(Rows.size(), 1u);
    ASSERT_TRUE(Rows[0].Loadable) << Rows[0].Reason;
    EXPECT_EQ(llvm::toString(Loader.validateBuffer(File.buffer())), "");
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    const Arch A = Little ? Arch::MIPS64N32EL : Arch::MIPS64N32;
    EXPECT_EQ(Rows[0].TheArch, A);
    EXPECT_EQ(Image->Arch, A);
    EXPECT_EQ(Image->getPointerSize(), 4u);
    EXPECT_EQ(Image->MIPS->ABI, MIPSABI::N32);
    File.Bytes[0xe4] = 1;
    auto Invalid = Loader.loadBuffer(File.buffer());
    ASSERT_FALSE(bool(Invalid));
    EXPECT_EQ(llvm::toString(Loader.validateBuffer(File.buffer())),
              llvm::toString(Invalid.takeError()));
  }
}

TEST(MIPSO32Loader, MIPS64CompatibilityModeRetainsReleaseAndPointerWidth) {
  using namespace llvm::ELF;
  ELFLoader Loader;
  for (bool Little : {false, true}) {
    for (unsigned Revision : {1u, 2u, 3u, 5u, 6u}) {
      SCOPED_TRACE(Revision);
      ELFBytes File(Little ? Arch::MIPS32EL : Arch::MIPS32);
      const uint32_t ISA = Revision == 1   ? EF_MIPS_ARCH_64
                           : Revision == 6 ? EF_MIPS_ARCH_64R6
                                           : EF_MIPS_ARCH_64R2;
      File.put(36, ISA | EF_MIPS_32BITMODE | EF_MIPS_ABI_O32, 4);
      File.abiFlags();
      File.Bytes[0xe2] = 64;
      File.Bytes[0xe3] = Revision;
      File.Bytes[0xe4] = File.Bytes[0xe5] = 2;
      File.Bytes[0xe7] = 6;
      auto Image = Loader.loadBuffer(File.buffer());
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      EXPECT_EQ(Image->Arch, Little ? Arch::MIPS64O32EL : Arch::MIPS64O32);
      EXPECT_EQ(Image->getPointerSize(), 4u);
      EXPECT_EQ(Image->MIPS->ABI, MIPSABI::O32);
      EXPECT_EQ(Image->MIPS->ISARevision, Revision);
      EXPECT_TRUE(Image->MIPS->hasScalarFP64ABI(Image->Arch));
      File.Bytes[0xe2] = 32;
      auto Conflict = Loader.loadBuffer(File.buffer());
      ASSERT_FALSE(bool(Conflict));
      llvm::consumeError(Conflict.takeError());
    }
  }
}

TEST(MIPSHistoricalLoader, MIPSIIIO32RetainsMachineAndABIWidths) {
  for (bool Little : {false, true}) {
    const Arch A = Little ? Arch::MIPS64O32EL : Arch::MIPS64O32;
    for (unsigned GPR : {0u, 1u, 2u, 3u}) {
      ELFBytes File(Little ? Arch::MIPS32EL : Arch::MIPS32);
      File.put(36, 0x20001101, 4); // GNU-linked MIPS III ELF32/o32.
      File.abiFlags();
      File.Bytes[0xe2] = 3;
      File.Bytes[0xe3] = 0;
      File.Bytes[0xe4] = GPR; // GNU emits 1, Clang can emit 2.
      File.Bytes[0xe5] = 1;
      File.Bytes[0xe7] = 1;
      std::vector<LoadCandidate> Rows;
      ELFLoader::identify(File.buffer(), Rows);
      ELFLoader Loader;
      auto Image = Loader.loadBuffer(File.buffer());
      ASSERT_EQ(Rows.size(), 1u);
      if (GPR != 1 && GPR != 2) {
        EXPECT_FALSE(Rows[0].Loadable);
        ASSERT_FALSE(bool(Image));
        llvm::consumeError(Image.takeError());
        continue;
      }
      ASSERT_TRUE(Rows[0].Loadable) << Rows[0].Reason;
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      EXPECT_EQ(Rows[0].TheArch, A);
      EXPECT_EQ(Image->Arch, A);
      EXPECT_EQ(Image->getPointerSize(), 4u);
      EXPECT_EQ(Image->MIPS->ABI, MIPSABI::O32);
      EXPECT_TRUE(Image->MIPS->hasScalarFP32ABI(A));
      EXPECT_FALSE(Image->MIPS->hasScalarFP64ABI(A));
    }
    ELFBytes GP(Little ? Arch::MIPS32EL : Arch::MIPS32);
    GP.put(36, 0x20001000, 4);
    GP.globalPointer(0xfedc8000);
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(GP.buffer());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    EXPECT_EQ(Image->MIPS->EntryGP, 0xfffffffffedc8000ull);
    const auto &TRI = getTargetRegInfo(A);
    EXPECT_EQ(TRI.addressRegisterBytes(), 8u);
    EXPECT_EQ(TRI.integerReturnWordBytes(), 4u);
    EXPECT_EQ(TRI.integerReturnWordOffset(0), Little ? 0u : 4u);
    const auto Layout = TRI.integerArgumentLayout(BinaryFormat::ELF);
    EXPECT_EQ(Layout.Registers.size(), 4u);
    EXPECT_EQ(Layout.SlotBytes, 4u);
    EXPECT_EQ(Layout.EntryStackBase, 16);
    EXPECT_EQ(guaranteedEntryStackAlignment(A, BinaryFormat::ELF), 8u);
    EXPECT_STREQ(TRI.GetRegName(mipsreg::r(8), 8), "t0");
    EXPECT_EQ(TRI.callPreservedPrefixSize(mipsreg::r(16), 8), 4u);
  }
}

TEST_P(MIPSLoader, GlobalPointerUsesLinkedRegisterInfoAndPhysicalGPRWidth) {
  ELFLoader Loader;
  for (bool N32 : {false, true}) {
    ELFBytes File(GetParam());
    if (N32 && File.Is64)
      continue;
    if (N32)
      File.put(36, 0x80000020, 4); // ELF32, MIPS64r2, ABI2.
    const uint64_t GP = File.Is64 ? 0x12345678fedc8000ull : 0xfedc8000;
    File.globalPointer(GP);
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    const uint64_t WideGP = N32 ? 0xfffffffffedc8000ull : GP;
    EXPECT_EQ(Image->MIPS->EntryGP, WideGP);
    EXPECT_FALSE(Image->MIPS->PICGP);

    const uint32_t Flags = File.Is64 ? 0x80000000
                           : N32     ? 0x80000020
                                     : 0x70001000;
    for (unsigned Extra : {2u, 4u, 0u}) {
      if (!Extra)
        File.put(16, 3, 2); // ET_DYN
      File.put(File.Is64 ? 48 : 36, Flags | Extra, 4);
      auto PositionIndependent = Loader.loadBuffer(File.buffer());
      ASSERT_TRUE(bool(PositionIndependent))
          << llvm::toString(PositionIndependent.takeError());
      EXPECT_FALSE(PositionIndependent->MIPS->EntryGP);
      if (Extra)
        EXPECT_EQ(PositionIndependent->MIPS->PICGP, WideGP);
      else
        EXPECT_FALSE(PositionIndependent->MIPS->PICGP);
    }
  }
}

TEST_P(MIPSLoader, SectionAndProgramGlobalPointerRecordsMustAgree) {
  ELFBytes File(GetParam());
  File.globalPointer(0x18340);
  const size_t Width = File.Is64 ? 8 : 4, Header = File.Is64 ? 64 : 40;
  File.Bytes.resize(0x180 + 2 * Header);
  File.put(File.Is64 ? 40 : 32, 0x180, Width); // Section table.
  File.put(File.Is64 ? 60 : 48, 2, 2);
  const size_t Section = 0x180 + Header;
  File.put(Section + 4, File.Is64 ? 0x7000000d : 0x70000006, 4);
  File.put(Section + (File.Is64 ? 24 : 16), 0x120, Width);
  const size_t Size = File.Is64 ? 48 : 24;
  File.put(Section + (File.Is64 ? 32 : 20), Size, Width);
  std::copy_n(File.Bytes.begin() + 0xb8, Size, File.Bytes.begin() + 0x120);
  ELFLoader Loader;
  for (unsigned Programs : {1u, 2u}) {
    File.put(File.Is64 ? 56 : 44, Programs, 2);
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    EXPECT_EQ(Image->MIPS->EntryGP, 0x18340u);
  }
  File.put(0x120 + Size - Width, 0x18348, Width);
  auto Conflict = Loader.loadBuffer(File.buffer());
  ASSERT_FALSE(bool(Conflict));
  EXPECT_NE(
      llvm::toString(Conflict.takeError()).find("conflicting global pointer"),
      std::string::npos);
  std::vector<LoadCandidate> Rows;
  ELFLoader::identify(File.buffer(), Rows);
  ASSERT_EQ(Rows.size(), 1u);
  EXPECT_FALSE(Rows.front().Loadable);
}

TEST_P(MIPSLoader, GlobalPointerRecordsRequireBoundedPayloadsAndImageScope) {
  ELFLoader Loader;
  ELFBytes File(GetParam());
  File.globalPointer(0x18340);
  if (File.Is64) {
    for (bool Group : {false, true}) {
      ELFBytes Scoped = File;
      if (Group)
        Scoped.Bytes[0xb8] = 9; // ODK_GP_GROUP
      else
        Scoped.put(0xc2, 1, 2); // Section-specific ODK_REGINFO.
      for (unsigned Flags : {0x80000000u, 0x80000006u}) {
        Scoped.put(48, Flags, 4);
        auto Image = Loader.loadBuffer(Scoped.buffer());
        ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
        EXPECT_FALSE(Image->MIPS->EntryGP);
        EXPECT_FALSE(Image->MIPS->PICGP);
      }
    }
  }
  for (unsigned Size : {0u, 7u, 23u, 41u, 255u}) {
    if (File.Is64)
      File.Bytes[0xc1] = Size;
    else
      File.put(84 + 16, Size, 4);
    auto Malformed = Loader.loadBuffer(File.buffer());
    ASSERT_FALSE(bool(Malformed)) << Size;
    llvm::consumeError(Malformed.takeError());
  }
}

TEST(MIPSHistoricalLoader,
     HeaderAndABIRecordsRetainIndependentArchitectureLevel) {
  for (bool Little : {false, true})
    for (MIPSABI ABI : {MIPSABI::O32, MIPSABI::N32, MIPSABI::N64})
      for (unsigned Level : {4, 5})
        for (bool HasABIFlags : {false, true}) {
          const bool Is64 = ABI == MIPSABI::N64;
          ELFBytes File(Is64 ? (Little ? Arch::MIPS64EL : Arch::MIPS64)
                             : (Little ? Arch::MIPS32EL : Arch::MIPS32));
          const uint32_t ABIFlags = ABI == MIPSABI::O32   ? 0x1100
                                    : ABI == MIPSABI::N32 ? 0x20
                                                          : 0;
          File.put(Is64 ? 48 : 36, ((Level - 1) << 28) | ABIFlags, 4);
          if (HasABIFlags) {
            File.abiFlags();
            File.Bytes[0xe2] = Level;
            File.Bytes[0xe3] = 0;
            File.Bytes[0xe4] = File.Bytes[0xe5] = 2;
            File.Bytes[0xe7] = ABI == MIPSABI::O32 ? 6 : 1;
          }
          ELFLoader Loader;
          auto Image = Loader.loadBuffer(File.buffer());
          ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
          EXPECT_EQ(Image->MIPS->ABI, ABI);
          EXPECT_EQ(Image->getPointerSize(), Is64 ? 8u : 4u);
          if (ABI == MIPSABI::O32)
            EXPECT_EQ(Image->Arch,
                      Little ? Arch::MIPS64O32EL : Arch::MIPS64O32);
          EXPECT_EQ(Image->MIPS->HistoricalISA, Level);
          EXPECT_EQ(Image->MIPS->ISARevision, 0);
          if (HasABIFlags) {
            for (unsigned Wrong : {32u, 64u, 9u - Level}) {
              File.Bytes[0xe2] = Wrong;
              auto Bad = Loader.loadBuffer(File.buffer());
              EXPECT_FALSE(bool(Bad));
              if (!Bad)
                llvm::consumeError(Bad.takeError());
            }
          }
        }
}

TEST(MIPSHistoricalLoader, EarlyProfilesKeepWidthFPAndExtensionBoundaries) {
  for (bool Little : {false, true})
    for (unsigned Level : {1u, 2u, 3u})
      for (bool N32 : {false, true}) {
        if (Level < 3 && N32)
          continue;
        const bool WideFile = Level == 3 && !N32;
        ELFBytes File(WideFile ? (Little ? Arch::MIPS64EL : Arch::MIPS64)
                               : (Little ? Arch::MIPS32EL : Arch::MIPS32));
        const uint32_t Flags = ((Level - 1) << 28) | (Level < 3 ? 0x1000u
                                                      : N32     ? 0x20u
                                                                : 0u);
        File.put(WideFile ? 48 : 36, Flags, 4);
        File.abiFlags();
        File.Bytes[0xe2] = Level;
        File.Bytes[0xe3] = 0;
        File.Bytes[0xe4] = File.Bytes[0xe5] = Level < 3 ? 1 : 2;
        File.Bytes[0xe7] = 1;
        ELFLoader Loader;
        auto Image = Loader.loadBuffer(File.buffer());
        ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
        EXPECT_EQ(Image->MIPS->HistoricalISA, Level);
        EXPECT_EQ(isMIPS64(Image->Arch), Level == 3);
        EXPECT_EQ(Image->MIPS->hasScalarFP32ABI(Level == 3), Level < 3);
        for (uint32_t Extension : {0x2000000u, 0x4000000u, 0x400u}) {
          File.put(WideFile ? 48 : 36, Flags | Extension, 4);
          auto Invalid = Loader.loadBuffer(File.buffer());
          EXPECT_FALSE(bool(Invalid));
          if (!Invalid)
            llvm::consumeError(Invalid.takeError());
        }
        if (Level < 3) {
          File.put(36, Flags | 0x200, 4);
          File.Bytes[0xe5] = 2;
          File.Bytes[0xe7] = 6;
          auto Invalid = Loader.loadBuffer(File.buffer());
          EXPECT_FALSE(bool(Invalid));
          if (!Invalid)
            llvm::consumeError(Invalid.takeError());
        }
      }
}

TEST_P(MIPSLoader, ProgramABIFlagsAgreeWithHeader) {
  ELFBytes File(GetParam());
  File.abiFlags();
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  ASSERT_TRUE(Image->MIPS);
  EXPECT_TRUE(Image->MIPS->HasABIFlags);
  EXPECT_EQ(Image->MIPS->FPABI, 3);
}

TEST_P(MIPSLoader, ScalarFP64ABIRequiresMatchingCPR1Evidence) {
  for (unsigned Size = 0; Size < 4; ++Size) {
    ELFBytes File(GetParam());
    File.abiFlags();
    File.Bytes[0xe5] = Size;
    File.Bytes[0xe7] = File.Is64 ? 1 : 6;
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_EQ(bool(Image), Size == 2);
    if (Image)
      EXPECT_TRUE(Image->MIPS->hasScalarFP64ABI(File.Is64));
    else
      EXPECT_NE(llvm::toString(Image.takeError()).find("CPR1 width"),
                std::string::npos);
    std::vector<LoadCandidate> Candidates;
    ELFLoader::identify(File.buffer(), Candidates);
    ASSERT_EQ(Candidates.size(), 1u);
    EXPECT_EQ(Candidates[0].Loadable, Size == 2);
  }
}

TEST_P(MIPSLoader, ScalarFP32ABIRequiresMatchingCPR1Evidence) {
  if (isMIPS64(GetParam()))
    return;
  for (unsigned Size = 0; Size < 4; ++Size) {
    ELFBytes File(GetParam());
    File.abiFlags();
    File.Bytes[0xe5] = Size;
    File.Bytes[0xe7] = 1;
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_EQ(bool(Image), Size == 1);
    if (Image) {
      EXPECT_TRUE(Image->MIPS->hasScalarFP32ABI(false));
      EXPECT_FALSE(Image->MIPS->hasScalarFP64ABI(false));
    } else
      EXPECT_NE(llvm::toString(Image.takeError()).find("FP32"),
                std::string::npos);
    std::vector<LoadCandidate> Candidates;
    ELFLoader::identify(File.buffer(), Candidates);
    ASSERT_EQ(Candidates.size(), 1u);
    EXPECT_EQ(Candidates[0].Loadable, Size == 1);
  }
}

TEST_P(MIPSLoader, RuntimeSymbolModesAreValidatedWithoutSectionHeaders) {
  for (uint8_t Mode : {uint8_t(0x80), uint8_t(0xf0)}) {
    ELFBytes File(GetParam());
    File.dynamicSymbols(Mode);
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_FALSE(bool(Image));
    EXPECT_NE(llvm::toString(Image.takeError()).find("symbol encoding"),
              std::string::npos);
  }
  ELFBytes File(GetParam());
  File.dynamicSymbols(0);
  File.put(0x180 + (File.Is64 ? 8 : 4) * 5, 0, File.Is64 ? 8 : 4);
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_FALSE(bool(Image));
  EXPECT_NE(llvm::toString(Image.takeError()).find("bounded size"),
            std::string::npos);
}

TEST_P(MIPSLoader, DSPCapabilityUsesWidthAndEncodingEvidence) {
  for (uint32_t ASE : {1u, 2u, 3u, 0x200u, 0x801u}) {
    ELFBytes File(GetParam());
    File.abiFlags();
    File.put(0xec, ASE, 4);
    if (ASE & 0x800)
      File.put(File.Is64 ? 48 : 36,
               (File.Is64 ? 0x80000000u : 0x70001000u) | 0x02000000, 4);
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    const bool Admitted = ASE == 1 || ASE == 2 || ASE == 3 || ASE == 0x801;
    ASSERT_EQ(bool(Image), Admitted);
    if (Admitted) {
      ASSERT_TRUE(Image->MIPS);
      EXPECT_TRUE(Image->MIPS->HasDSP);
      EXPECT_EQ(Image->MIPS->HasDSPR2, bool(ASE & 2));
    } else {
      llvm::consumeError(Image.takeError());
    }
    std::vector<LoadCandidate> Candidates;
    ELFLoader::identify(File.buffer(), Candidates);
    ASSERT_EQ(Candidates.size(), 1u);
    EXPECT_EQ(Candidates[0].Loadable, Admitted);
  }
}

TEST_P(MIPSLoader, ClassicReleaseIsEstablishedByHeaderAndABIRecord) {
  ELFLoader Loader;
  for (bool N32 : {false, true}) {
    ELFBytes File(GetParam());
    if (N32 && File.Is64)
      continue;
    const bool Wide = File.Is64 || N32;
    const uint32_t ABI = N32 ? 0x20u : File.Is64 ? 0u : 0x1000u;
    for (bool First : {false, true}) {
      const uint32_t Flags = (Wide ? (First ? 6u : 8u) : (First ? 5u : 7u))
                             << 28;
      auto Input = File;
      Input.put(Input.Is64 ? 48 : 36, Flags | ABI, 4);
      auto HeaderOnly = Loader.loadBuffer(Input.buffer());
      ASSERT_TRUE(bool(HeaderOnly)) << llvm::toString(HeaderOnly.takeError());
      EXPECT_EQ(HeaderOnly->MIPS->ISARevision, First ? 1 : 2);
      Input.abiFlags();
      Input.Bytes[0xe2] = Wide ? 64 : 32;
      Input.Bytes[0xe4] = Wide ? 2 : 1;
      for (unsigned Revision : {0u, 1u, 2u, 3u, 4u, 5u, 6u}) {
        SCOPED_TRACE(Revision);
        Input.Bytes[0xe3] = Revision;
        const bool Match =
            First ? Revision == 1
                  : Revision == 2 || Revision == 3 || Revision == 5;
        auto Image = Loader.loadBuffer(Input.buffer());
        std::vector<LoadCandidate> Rows;
        ELFLoader::identify(Input.buffer(), Rows);
        ASSERT_EQ(Rows.size(), 1u);
        EXPECT_EQ(Rows.front().Loadable, Match);
        ASSERT_EQ(bool(Image), Match);
        if (Match) {
          EXPECT_EQ(Image->MIPS->ISARevision, Revision);
          EXPECT_EQ(Image->MIPS->HistoricalISA, 0);
          EXPECT_EQ(isMIPS64(Image->Arch), Wide);
        } else {
          llvm::consumeError(Image.takeError());
        }
      }
    }
  }
}

TEST_P(MIPSLoader, ScalarMicroReleaseUsesTheABIRecord) {
  using namespace llvm::ELF;
  ELFLoader Loader;
  for (bool N32 : {false, true}) {
    ELFBytes File(GetParam());
    if (N32 && File.Is64)
      continue;
    const bool Wide = File.Is64 || N32;
    const uint32_t ABI = N32 ? EF_MIPS_ABI2 : File.Is64 ? 0 : EF_MIPS_ABI_O32;
    File.put(File.Is64 ? 48 : 36,
             (Wide ? EF_MIPS_ARCH_64R2 : EF_MIPS_ARCH_32R2) |
                 EF_MIPS_MICROMIPS | ABI,
             4);
    File.put(24, 0x10101, File.Is64 ? 8 : 4);
    File.abiFlags();
    File.put(0xec, llvm::Mips::AFL_ASE_MICROMIPS, 4);
    File.Bytes[0xe2] = Wide ? 64 : 32;
    File.Bytes[0xe4] = Wide ? 2 : 1;
    for (unsigned Revision : {1u, 2u, 3u, 4u, 5u, 6u}) {
      SCOPED_TRACE(Revision);
      File.Bytes[0xe3] = Revision;
      const bool Match = Revision == 2 || Revision == 3 || Revision == 5;
      auto Image = Loader.loadBuffer(File.buffer());
      std::vector<LoadCandidate> Candidates;
      ELFLoader::identify(File.buffer(), Candidates);
      ASSERT_EQ(Candidates.size(), 1u);
      EXPECT_EQ(Candidates.front().Loadable, Match);
      ASSERT_EQ(bool(Image), Match) << llvm::toString(Image.takeError());
      if (Image) {
        EXPECT_EQ(Image->MIPS->ISARevision, Revision);
        EXPECT_EQ(isMIPS64(Image->Arch), Wide);
        EXPECT_EQ(Image->instructionModeAt(Image->Entry),
                  InstructionMode::MicroMIPS);
      } else {
        llvm::consumeError(Image.takeError());
      }
    }
  }
}

TEST_P(MIPSLoader, MSARequiresRevisionFiveAndFP64VectorEvidence) {
  for (unsigned Variant = 0; Variant != 7; ++Variant) {
    ELFBytes File(GetParam());
    File.abiFlags();
    File.Bytes[0xe3] = Variant == 1 ? 2 : 5;
    File.Bytes[0xe5] = Variant == 2 ? 2 : 3;
    File.Bytes[0xe7] = Variant == 3 ? 3 : 6;
    File.put(0xec, Variant == 4 ? 0 : Variant == 5 ? 0x201 : 0x200, 4);
    if (Variant == 6)
      File.Bytes[0xe3] = 6;
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    const bool Admitted = Variant == 0 && !File.Is64;
    ASSERT_EQ(bool(Image), Admitted);
    if (Image) {
      EXPECT_TRUE(Image->MIPS->HasMSA);
      EXPECT_EQ(Image->MIPS->ISARevision, 5);
    } else {
      llvm::consumeError(Image.takeError());
    }
    std::vector<LoadCandidate> Candidates;
    ELFLoader::identify(File.buffer(), Candidates);
    ASSERT_EQ(Candidates.size(), 1u);
    EXPECT_EQ(Candidates[0].Loadable, Admitted);
  }
}

TEST_P(MIPSLoader, Release6HeaderAndABIFlagsMustAgree) {
  for (bool WithABI : {false, true}) {
    ELFBytes File(GetParam());
    File.put(File.Is64 ? 48 : 36, File.Is64 ? 0xa0000000u : 0x90001000u, 4);
    if (WithABI) {
      File.abiFlags();
      File.Bytes[0xe3] = 6;
    }
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    ASSERT_TRUE(Image->MIPS);
    EXPECT_EQ(Image->MIPS->ISARevision, 6);
    File.abiFlags(); // r2 payload conflicts with the r6 header.
    auto Conflict = Loader.loadBuffer(File.buffer());
    ASSERT_FALSE(bool(Conflict));
    llvm::consumeError(Conflict.takeError());
  }
}

TEST_P(MIPSLoader, MicroRelease6TaggedELFEntryAndABIRecordAgree) {
  ELFBytes File(GetParam());
  File.abiFlags();
  File.put(File.Is64 ? 48 : 36, File.Is64 ? 0xa2000000u : 0x92001000u, 4);
  File.Bytes[0xe3] = 6;
  File.put(0xec, 0x800, 4);
  File.put(24, 0x10101, File.Is64 ? 8 : 4);
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->MIPS->ISARevision, 6);
  EXPECT_TRUE(Image->MIPS->HasMicroMIPS);
  EXPECT_EQ(Image->Entry, 0x10100u);
  EXPECT_EQ(Image->instructionModeAt(Image->Entry), InstructionMode::MicroMIPS);
  File.put(0xec, 0, 4);
  auto Conflict = Loader.loadBuffer(File.buffer());
  ASSERT_FALSE(bool(Conflict));
  llvm::consumeError(Conflict.takeError());
}

TEST_P(MIPSLoader, XPAHighWordTransferFlagRequiresRelease6) {
  for (bool R6 : {false, true}) {
    ELFBytes File(GetParam());
    File.abiFlags();
    if (R6) {
      File.put(File.Is64 ? 48 : 36, File.Is64 ? 0xa0000000u : 0x90001000u, 4);
      File.Bytes[0xe3] = 6;
    }
    File.put(0xec, 0x1000, 4); // AFL_ASE_XPA
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_EQ(bool(Image), R6);
    if (!Image)
      llvm::consumeError(Image.takeError());
  }
}

TEST_P(MIPSLoader, DynamicProgramTagsUseTheImageByteOrder) {
  ELFBytes File(GetParam());
  File.dynamicSymbols(0);
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Dynamic = readELFProgramDynamicTable(*Image);
  ASSERT_TRUE(bool(Dynamic)) << llvm::toString(Dynamic.takeError());
  ASSERT_EQ(Dynamic->size(), 3u);
  EXPECT_EQ(Dynamic->front().Tag, 6);
  EXPECT_EQ(Dynamic->front().Value, 0x10200u);
  EXPECT_EQ(Dynamic->back().Tag, 0x70000011);
  EXPECT_EQ(Dynamic->back().Value, 2u);
}

TEST_P(MIPSLoader, GlobalGOTSuffixBindsImportsWithoutPerCellRelocations) {
  ELFBytes File(GetParam());
  File.dynamicGOT();
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const va_t Slot = 0x10380 + 2 * (File.Is64 ? 8 : 4);
  const auto *Imported = Image->findImportAt(Slot);
  ASSERT_NE(Imported, nullptr);
  EXPECT_EQ(Imported->Name, "fixture_import");
  ASSERT_TRUE(Image->ImportStorageSlots.count(Slot));
  EXPECT_EQ(Image->ImportStorageSlots.at(Slot).Name, "fixture_import");
  // A GOT with a first global index beyond the symbol table is malformed.
  File.put(0x180 + 15 * (File.Is64 ? 8 : 4), 3, File.Is64 ? 8 : 4);
  auto Invalid = Loader.loadBuffer(File.buffer());
  ASSERT_FALSE(bool(Invalid));
  const auto Message = llvm::toString(Invalid.takeError());
  EXPECT_NE(Message.find("inconsistent"), std::string::npos);
  EXPECT_EQ(llvm::toString(Loader.validateBuffer(File.buffer())), Message);
}

TEST_P(MIPSLoader, Packed64RelocationsAreRejectedExplicitly) {
  if (!isMIPS64(GetParam()))
    return;
  ELFBytes File(GetParam());
  File.dynamicGOT();
  // Runtime RELA metadata, without any section headers. r_info has a
  // target-order symbol word followed by ssym, type3, type2, type1 bytes.
  const std::pair<uint64_t, uint64_t> Extra[] = {
      {7, 0x103a0}, {8, 24}, {9, 24}, {0, 0}};
  for (unsigned I = 0; I < 4; ++I) {
    File.put(0x200 + 16 * I, Extra[I].first, 8);
    File.put(0x208 + 16 * I, Extra[I].second, 8);
  }
  File.put(120 + 32, 12 * 16, 8);
  File.put(120 + 40, 12 * 16, 8);
  File.put(0x3a0, 0x10380, 8);
  File.Bytes[0x3ae] = 18; // type2 = R_MIPS_64
  File.Bytes[0x3af] = 3;  // type1 = R_MIPS_REL32
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_FALSE(bool(Image));
  EXPECT_NE(llvm::toString(Image.takeError()).find("packed MIPS64"),
            std::string::npos);
}

TEST_P(MIPSLoader, RuntimeRelocationsCannotSilentlyKeepUnresolvedAddends) {
  for (unsigned Type : {2u, 3u, 18u, 38u, 127u}) {
    for (bool HiddenBySections : {false, true}) {
      SCOPED_TRACE(Type);
      SCOPED_TRACE(HiddenBySections);
      ELFBytes File(GetParam());
      File.runtimeRelocation(Type, true);
      if (HiddenBySections) {
        // PT_DYNAMIC still names the relocation even when the only section
        // header left in the file is SHT_NULL.
        File.Bytes.resize(File.Is64 ? 0x440 : 0x428);
        File.put(File.Is64 ? 40 : 32, 0x400, File.Is64 ? 8 : 4);
        File.put(File.Is64 ? 60 : 48, 1, 2);
      }
      ELFLoader Loader;
      auto Image = Loader.loadBuffer(File.buffer());
      ASSERT_FALSE(bool(Image));
      EXPECT_NE(
          llvm::toString(Image.takeError()).find("MIPS runtime relocation"),
          std::string::npos);
    }
  }
}

TEST_P(MIPSLoader, GOTMayEndInTheZeroFilledLoadTailButNotOutsideTheMapping) {
  ELFBytes File(GetParam());
  File.dynamicGOT();
  const unsigned Width = File.Is64 ? 8 : 4;
  const size_t DataHeader = File.Is64 ? 176 : 116;
  const size_t FileSizeField = DataHeader + (File.Is64 ? 32 : 16);
  const size_t MemorySizeField = DataHeader + (File.Is64 ? 40 : 20);
  // The final GOT cell has a file-backed prefix and a zero-filled suffix,
  // as in the sectionless Archer C7 ubus executable.
  File.put(FileSizeField, 0x80 + 3 * Width - 3, Width);
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  EXPECT_NE(Image->findImportAt(0x10380 + 2 * Width), nullptr);
  File.put(MemorySizeField, 0x80 + 3 * Width - 1, Width);
  auto Invalid = Loader.loadBuffer(File.buffer());
  ASSERT_FALSE(bool(Invalid));
  EXPECT_NE(llvm::toString(Invalid.takeError()).find("not mapped"),
            std::string::npos);
}

TEST_P(MIPSLoader, CompletePLTVeneerNamesOnlyItsBoundImportSlot) {
  ELFBytes File(GetParam());
  File.put(0x100, 0x3c0f0001, 4); // lui r15,1
  File.put(0x104, File.Is64 ? 0xddf90118 : 0x8df90118, 4);
  File.put(0x108, 0x03200008, 4); // jr t9
  File.put(0x10c, File.Is64 ? 0x65f80118 : 0x25f80118, 4);
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  Import Imported;
  Imported.Name = "fixture_import";
  Imported.IATAddr = 0x10118;
  Image->Imports.push_back(std::move(Imported));
  const auto *Found = Image->findImportAt(0x10100);
  ASSERT_NE(Found, nullptr);
  EXPECT_EQ(Found->Name, "fixture_import");
  EXPECT_EQ(Image->findImportAt(0x10104), nullptr);
  // A changed delay slot no longer states the PLT contract.
  Image->Segments[0].Data[0x10c] ^= 1;
  EXPECT_EQ(Image->findImportAt(0x10100), nullptr);
}

TEST_P(MIPSLoader, RejectsConflictingOrUnsupportedProfilesInBothEntrypoints) {
  const auto Reject = [&](ELFBytes &File, llvm::StringRef Expected) {
    SCOPED_TRACE(Expected.str());
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_FALSE(bool(Image));
    const std::string Error = llvm::toString(Image.takeError());
    EXPECT_NE(Error.find(Expected.str()), std::string::npos) << Error;
    EXPECT_EQ(llvm::toString(Loader.validateBuffer(File.buffer())), Error);
    std::vector<LoadCandidate> Rows;
    ELFLoader::identify(File.buffer(), Rows);
    ASSERT_EQ(Rows.size(), 1u);
    EXPECT_FALSE(Rows[0].Loadable);
    EXPECT_NE(Rows[0].Reason.find(Expected.str()), std::string::npos)
        << Rows[0].Reason;
  };
  for (const auto &[Bits, Reason] :
       std::array<std::pair<uint32_t, const char *>, 4>{
           {{0x20, "n32"},
            {0x06000000, "MIPS16"},
            {0x04000000, "MIPS16"},
            {0x00000100, "ABI"}}}) {
    ELFBytes File(GetParam());
    if (Bits == 0x04000000 && !File.Is64)
      continue; // MIPS16/o32 has separate admission and occurrence tests.
    File.put(File.Is64 ? 48 : 36, (File.Is64 ? 0x80000000 : 0x70001000) | Bits,
             4);
    Reject(File, Reason);
  }
  ELFBytes ClassConflict(GetParam());
  ClassConflict.put(ClassConflict.Is64 ? 48 : 36,
                    ClassConflict.Is64 ? 0x70000000 : 0x80001000, 4);
  Reject(ClassConflict, "matching");
  ELFBytes Metadata(GetParam());
  Metadata.abiFlags();
  Metadata.Bytes[0xe4] = Metadata.Is64 ? 1 : 2;
  Reject(Metadata, "conflict");
  ELFBytes Truncated(GetParam());
  Truncated.abiFlags();
  Truncated.Bytes.resize(0xf0);
  Reject(Truncated, "malformed .MIPS.abiflags");
  ELFBytes Relocatable(GetParam());
  Relocatable.put(16, 1, 2);
  Reject(Relocatable, "linked");
}

TEST(MicroMIPSLoader, MixedRuntimeSymbolsRetainExactModeAndBoundedRanges) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL}) {
    ELFBytes File(Target);
    File.dynamicSymbols(0x80);
    File.put(36, 0x72001000, 4);
    File.put(0x214, 0x10108, 4); // Even st_value; st_other owns the mode.
    File.put(0x218, 6, 4);
    File.put(0x21c, 0x12, 1); // STT_FUNC, global
    File.put(0x21e, 1, 2);
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    EXPECT_EQ(Image->Mode, InstructionMode::MixedMIPS);
    EXPECT_EQ(Image->instructionModeAt(0x10100), InstructionMode::MIPSClassic);
    EXPECT_EQ(Image->instructionModeAt(0x10108), InstructionMode::MicroMIPS);
    EXPECT_EQ(Image->instructionModeAt(0x1010c), InstructionMode::MicroMIPS);
    EXPECT_FALSE(Image->instructionModeAt(0x1010e));
    EXPECT_EQ(Image->instructionModeAt(0x1010e, InstructionMode::MicroMIPS),
              InstructionMode::MicroMIPS);
    EXPECT_FALSE(
        Image->instructionModeAt(0x10108, InstructionMode::MIPSClassic));
    EXPECT_FALSE(Image->instructionModeAt(0x10100, InstructionMode::MicroMIPS));
    EXPECT_TRUE(Image->isFunctionEntryCandidateAt(0x1010a));
    EXPECT_FALSE(Image->isFunctionEntryCandidateAt(0x1010e));
    File.put(24, 0x10109, 4);
    auto Tagged = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Tagged)) << llvm::toString(Tagged.takeError());
    EXPECT_EQ(Tagged->Entry, 0x10108u);
    File.put(24, 0x10108, 4); // Header says classic, symbol says micro.
    auto Conflict = Loader.loadBuffer(File.buffer());
    ASSERT_FALSE(bool(Conflict));
    EXPECT_NE(llvm::toString(Conflict.takeError()).find("conflicting"),
              std::string::npos);
  }
}

TEST(MIPS16Loader, HeaderSymbolsAndABIFlagsRetainTheMIPS16Family) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL}) {
    ELFBytes File(Target);
    File.dynamicSymbols(
        0xf0); // Includes the microMIPS bit, but denotes MIPS16.
    File.put(36, 0x74001000, 4);
    File.put(0x214, 0x10108, 4);
    File.put(0x218, 6, 4);
    File.put(0x21c, 0x12, 1);
    File.put(0x21e, 1, 2);
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    ASSERT_TRUE(Image->MIPS);
    EXPECT_TRUE(Image->MIPS->HasMIPS16);
    EXPECT_FALSE(Image->MIPS->HasMicroMIPS);
    EXPECT_EQ(Image->instructionModeAt(0x10100), InstructionMode::MIPSClassic);
    EXPECT_EQ(Image->instructionModeAt(0x10108), InstructionMode::MIPS16);
    EXPECT_EQ(Image->instructionModeAt(0x1010c), InstructionMode::MIPS16);
    EXPECT_FALSE(Image->instructionModeAt(0x1010e));
    EXPECT_FALSE(Image->instructionModeAt(0x10108, InstructionMode::MicroMIPS));
    File.put(24, 0x10109, 4);
    auto Tagged = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Tagged)) << llvm::toString(Tagged.takeError());
    EXPECT_EQ(Tagged->Entry, 0x10108u);
    for (uint32_t Flags : {0x70001000u, 0x72001000u, 0x76001000u}) {
      File.put(36, Flags, 4);
      auto Bad = Loader.loadBuffer(File.buffer());
      ASSERT_FALSE(bool(Bad));
      llvm::consumeError(Bad.takeError());
    }
    ELFBytes ABI(Target);
    ABI.abiFlags();
    ABI.put(36, 0x74001000, 4);
    ABI.put(24, 0x10101, 4);
    ABI.put(0xec, 0x400, 4); // AFL_ASE_MIPS16
    auto Matched = Loader.loadBuffer(ABI.buffer());
    ASSERT_TRUE(bool(Matched)) << llvm::toString(Matched.takeError());
    for (uint32_t ASE : {0u, 0x800u, 0xc00u, 0x401u, 0x600u}) {
      ABI.put(0xec, ASE, 4);
      auto Bad = Loader.loadBuffer(ABI.buffer());
      ASSERT_FALSE(bool(Bad));
      llvm::consumeError(Bad.takeError());
    }
  }
}

TEST(MIPS16Loader, E2RequiresMatchingExplicitABIRecords) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL}) {
    ELFBytes F(Target);
    F.abiFlags();
    F.put(36, 0x74001000, 4);
    F.put(24, 0x10101, 4);
    ELFLoader L;
    for (uint32_t ASE :
         {0x400u, 0x4400u, 0x4440u, 0x440u, 0x4000u, 0x4800u, 0x4401u}) {
      F.put(0xec, ASE, 4);
      auto I = L.loadBuffer(F.buffer());
      const bool Valid = ASE == 0x400 || ASE == 0x4400 || ASE == 0x4440;
      ASSERT_EQ(bool(I), Valid);
      if (I)
        EXPECT_EQ(I->MIPS->HasMIPS16E2, bool(ASE & 0x4000));
      else
        llvm::consumeError(I.takeError());
    }
    F.put(0xec, 0x4400, 4);
    // Add a second program record with conflicting e2 evidence.
    F.put(44, 3, 2);
    F.put(116, 0x70000003, 4);
    F.put(120, 0xc0, 4);
    F.put(132, 24, 4);
    std::copy_n(F.Bytes.begin() + 0xe0, 24, F.Bytes.begin() + 0xc0);
    F.put(0xcc, 0x400, 4);
    auto Bad = L.loadBuffer(F.buffer());
    ASSERT_FALSE(bool(Bad));
    EXPECT_NE(llvm::toString(Bad.takeError()).find("conflicting"),
              std::string::npos);
  }
}

TEST(MIPS16Loader, DefinedGOTFunctionsSerializeTagsWithoutTaggingData) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL})
    for (bool Function : {false, true}) {
      ELFBytes File(Target);
      File.dynamicGOT();
      File.put(36, 0x74001000, 4);
      File.put(0x314, 0x10108, 4);
      File.put(0x318, 4, 4);
      File.put(0x31c, Function ? 0x12 : 0x11, 1);
      File.put(0x31d, Function ? 0xf0 : 0, 1);
      File.put(0x31e, 1, 2);
      File.put(0x388, Function ? 0x10109 : 0x10108, 4);
      ELFLoader Loader;
      auto Image = Loader.loadBuffer(File.buffer());
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      const auto Pointers = Image->readPtrArray(0x10388, 1);
      ASSERT_EQ(Pointers.size(), 1u);
      EXPECT_EQ(Pointers.front(), Function ? 0x10109u : 0x10108u);
      File.put(0x388, Function ? 0x10108 : 0x10109, 4);
      auto Bad = Loader.loadBuffer(File.buffer());
      ASSERT_FALSE(bool(Bad));
      llvm::consumeError(Bad.takeError());
    }
}

TEST(MicroMIPSLoader, DefinedGOTFunctionsUseCallableTagsButDataDoesNot) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL}) {
    for (bool Function : {false, true}) {
      SCOPED_TRACE(Function);
      ELFBytes File(Target);
      File.dynamicGOT();
      File.put(36, 0x72001000, 4);
      File.put(0x314, 0x10108, 4); // Even symbol value.
      File.put(0x318, 4, 4);
      File.put(0x31c, Function ? 0x12 : 0x11, 1); // STT_FUNC / STT_OBJECT
      File.put(0x31d, Function ? 0x80 : 0, 1);
      File.put(0x31e, 1, 2);
      File.put(0x388, Function ? 0x10109 : 0x10108, 4);
      ELFLoader Loader;
      auto Image = Loader.loadBuffer(File.buffer());
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      const auto Pointers = Image->readPtrArray(0x10388, 1);
      ASSERT_EQ(Pointers.size(), 1u);
      EXPECT_EQ(Pointers.front(), Function ? 0x10109u : 0x10108u);
      File.put(0x388, Function ? 0x10108 : 0x10109, 4);
      auto Invalid = Loader.loadBuffer(File.buffer());
      ASSERT_FALSE(bool(Invalid));
      EXPECT_NE(llvm::toString(Invalid.takeError()).find("symbol resolution"),
                std::string::npos);
    }
  }
}

TEST(MicroMIPSLoader, HeaderAndABIFlagsMustAgreeAboutEncodingCapability) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL}) {
    ELFBytes File(Target);
    File.abiFlags();
    File.put(36, 0x72001000, 4);
    File.put(24, 0x10101, 4);
    ELFLoader Loader;
    auto Missing = Loader.loadBuffer(File.buffer());
    ASSERT_FALSE(bool(Missing));
    llvm::consumeError(Missing.takeError());
    File.put(0xec, 0x800, 4);
    auto Admitted = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Admitted)) << llvm::toString(Admitted.takeError());
    EXPECT_EQ(Admitted->Entry, 0x10100u);
    EXPECT_EQ(Admitted->instructionModeAt(Admitted->Entry),
              InstructionMode::MicroMIPS);
    File.put(36, 0x70001000, 4);
    auto Contradiction = Loader.loadBuffer(File.buffer());
    ASSERT_FALSE(bool(Contradiction));
    llvm::consumeError(Contradiction.takeError());
  }
}

TEST_P(MIPSLoader, FrameLengthsPointersAndCFIUseTargetByteOrder) {
  BinaryImage Image;
  Image.Arch = GetParam();
  Image.Bits = isMIPS64(GetParam()) ? Bitness::Bits64 : Bitness::Bits32;
  std::vector<uint8_t> Bytes;
  const auto Append = [&](uint64_t Value, unsigned Width) {
    for (unsigned I = 0; I < Width; ++I)
      Bytes.push_back(uint8_t(
          Value >> (8 * (archLittleEndian(GetParam()) ? I : Width - I - 1))));
  };
  Append(16, 4); // CIE length
  Append(0, 4);  // CIE id
  Bytes.insert(Bytes.end(), {1, 'z', 'R', 0, 1, 0x7c, 31, 1, 0, 0x0c, 29,
                             32}); // CFA = sp + 32
  const unsigned PointerBytes = Image.getPointerSize();
  const uint64_t FDEStart = Bytes.size();
  Append(10 + PointerBytes * 2, 4);
  Append(FDEStart + 4, 4); // backwards CIE offset
  Append(0x10100, PointerBytes);
  Append(0x180, PointerBytes);
  Append(0, 1); // empty augmentation
  Append(3, 1); // DW_CFA_advance_loc2
  Append(0x104, 2);
  Bytes.insert(Bytes.end(), {0x9f, 1}); // ra saved at CFA - 4
  Append(0, 4);
  const auto Frames =
      dwarf_eh::parseEHFrame(Image, {Bytes.data(), Bytes.size(), 0x30000}, {});
  EXPECT_EQ(Frames.ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_EQ(Frames.CIEs.size(), 1u);
  EXPECT_EQ(Frames.CIEs[0].ReturnAddressRegister, 31u);
  ASSERT_EQ(Frames.FDEs.size(), 1u);
  EXPECT_EQ(Frames.FDEs[0].InitialLocation, 0x10100u);
  EXPECT_EQ(Frames.FDEs[0].AddressRange, 0x180u);
  ASSERT_EQ(Frames.FDEs[0].Instructions.size(), 2u);
  EXPECT_EQ(Frames.FDEs[0].Instructions.back().CodeOffset, 0x104u);
}

INSTANTIATE_TEST_SUITE_P(Profiles, MIPSLoader,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL,
                                           Arch::MIPS64, Arch::MIPS64EL));

} // namespace

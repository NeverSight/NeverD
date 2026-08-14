//===- ELFExceptionTests.cpp - ELF unwind-record rewrite tests ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/ELF/ELFExceptionPatch.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace {

using namespace neverd;

// A synthetic ELF64 image laid out so that every offset a patch has to reason
// about is known in advance: one PT_LOAD covering the whole file, a
// PT_GNU_EH_FRAME over the header, and `.text` / `.eh_frame` / `.eh_frame_hdr`
// carrying a single original function's records.
constexpr uint64_t kBase = 0x400000;
constexpr uint64_t kPhOff = 64;
constexpr uint64_t kPhEnt = 56;
constexpr uint64_t kTextOff = 176;
constexpr uint64_t kEhFrameOff = 208;
constexpr uint64_t kEhFrameSize = 169;
constexpr uint64_t kEhFrameHdrOff = 384;
constexpr uint64_t kEhFrameHdrSize = 20;
constexpr uint64_t kShStrTabOff = 532;
constexpr uint64_t kShStrTabSize = 41;
constexpr uint64_t kShOff = 576;
constexpr uint64_t kShEnt = 64;
constexpr uint64_t kFileEnd = kShOff + 5 * kShEnt;

constexpr uint64_t kFunc0VA = kBase + 0x1000;
constexpr uint64_t kFunc1VA = kBase + 0x5000;
constexpr uint64_t kEhFrameVA = kBase + kEhFrameOff;
constexpr uint64_t kEhFrameHdrVA = kBase + kEhFrameHdrOff;

void putU16(std::vector<uint8_t> &B, size_t Off, uint16_t V) {
  B[Off] = V & 0xff;
  B[Off + 1] = (V >> 8) & 0xff;
}
void putU32(std::vector<uint8_t> &B, size_t Off, uint32_t V) {
  for (int I = 0; I < 4; ++I)
    B[Off + I] = (V >> (8 * I)) & 0xff;
}
void putU64(std::vector<uint8_t> &B, size_t Off, uint64_t V) {
  for (int I = 0; I < 8; ++I)
    B[Off + I] = (V >> (8 * I)) & 0xff;
}
void putS32(std::vector<uint8_t> &B, size_t Off, int64_t V) {
  putU32(B, Off, static_cast<uint32_t>(static_cast<int32_t>(V)));
}

uint32_t getU32(const std::vector<uint8_t> &B, size_t Off) {
  uint32_t V;
  std::memcpy(&V, B.data() + Off, sizeof(V));
  return V;
}
int32_t getS32(const std::vector<uint8_t> &B, size_t Off) {
  int32_t V;
  std::memcpy(&V, B.data() + Off, sizeof(V));
  return V;
}
uint64_t getU64(const std::vector<uint8_t> &B, size_t Off) {
  uint64_t V;
  std::memcpy(&V, B.data() + Off, sizeof(V));
  return V;
}

// One CIE (augmentation "zR" with an absolute FDE pointer encoding) and one FDE
// naming \p InitLocVA, terminated by a zero-length record.  The absolute
// encoding keeps the FDE's initial_location a plain 8-byte address, which is
// all the search-table rebuild has to read back out.
std::vector<uint8_t> makeEhFrameFragment(uint64_t InitLocVA) {
  std::vector<uint8_t> F;
  auto PushU32 = [&](uint32_t V) {
    for (int I = 0; I < 4; ++I)
      F.push_back((V >> (8 * I)) & 0xff);
  };
  auto PushU64 = [&](uint64_t V) {
    for (int I = 0; I < 8; ++I)
      F.push_back((V >> (8 * I)) & 0xff);
  };

  const std::vector<uint8_t> CIE = {
      0,    0,    0,   0, 1, 'z', 'R', 0x00, 0x01, /*data align -8*/ 0x78,
      0x10, 0x01, 0x00};
  PushU32(static_cast<uint32_t>(CIE.size()));
  F.insert(F.end(), CIE.begin(), CIE.end());

  PushU32(21);        // FDE length
  PushU32(21);        // CIE pointer: back to the CIE at fragment offset 0
  PushU64(InitLocVA); // initial_location (absolute)
  PushU64(0x10);      // address_range
  F.push_back(0);     // empty FDE augmentation data
  PushU32(0);         // terminator
  return F;
}

std::vector<uint8_t> makeELF64WithEH() {
  std::vector<uint8_t> B(kFileEnd, 0);

  // ELF header.
  std::memcpy(B.data(),
              "\x7f"
              "ELF",
              4);
  B[llvm::ELF::EI_CLASS] = llvm::ELF::ELFCLASS64;
  B[llvm::ELF::EI_DATA] = llvm::ELF::ELFDATA2LSB;
  B[llvm::ELF::EI_VERSION] = llvm::ELF::EV_CURRENT;
  putU16(B, 16, llvm::ELF::ET_DYN);
  putU16(B, 18, llvm::ELF::EM_X86_64);
  putU32(B, 20, llvm::ELF::EV_CURRENT);
  putU64(B, 24, kBase + kTextOff); // e_entry
  putU64(B, 32, kPhOff);           // e_phoff
  putU64(B, 40, kShOff);           // e_shoff
  putU16(B, 52, 64);               // e_ehsize
  putU16(B, 54, kPhEnt);           // e_phentsize
  putU16(B, 56, 2);                // e_phnum
  putU16(B, 58, kShEnt);           // e_shentsize
  putU16(B, 60, 5);                // e_shnum
  putU16(B, 62, 4);                // e_shstrndx

  // PT_LOAD over the whole file.
  putU32(B, kPhOff + 0, llvm::ELF::PT_LOAD);
  putU32(B, kPhOff + 4, llvm::ELF::PF_R | llvm::ELF::PF_X);
  putU64(B, kPhOff + 8, 0);
  putU64(B, kPhOff + 16, kBase);
  putU64(B, kPhOff + 24, kBase);
  putU64(B, kPhOff + 32, kFileEnd);
  putU64(B, kPhOff + 40, kFileEnd);
  putU64(B, kPhOff + 48, 0x1000);

  // PT_GNU_EH_FRAME over .eh_frame_hdr.
  const uint64_t GnuOff = kPhOff + kPhEnt;
  putU32(B, GnuOff + 0, llvm::ELF::PT_GNU_EH_FRAME);
  putU32(B, GnuOff + 4, llvm::ELF::PF_R);
  putU64(B, GnuOff + 8, kEhFrameHdrOff);
  putU64(B, GnuOff + 16, kEhFrameHdrVA);
  putU64(B, GnuOff + 24, kEhFrameHdrVA);
  putU64(B, GnuOff + 32, kEhFrameHdrSize);
  putU64(B, GnuOff + 40, kEhFrameHdrSize);
  putU64(B, GnuOff + 48, 4);

  // .text placeholder.
  for (uint64_t I = 0; I < 32; ++I)
    B[kTextOff + I] = 0x90;

  // .eh_frame: one original function's records.
  std::vector<uint8_t> EhFrame = makeEhFrameFragment(kFunc0VA);
  std::memcpy(B.data() + kEhFrameOff, EhFrame.data(), EhFrame.size());

  // .eh_frame_hdr: version 1, pcrel eh_frame_ptr, udata4 count, datarel sdata4
  // search table with the one original entry.
  B[kEhFrameHdrOff + 0] = 1;
  B[kEhFrameHdrOff + 1] = 0x1b; // DW_EH_PE_pcrel | sdata4
  B[kEhFrameHdrOff + 2] = 0x03; // DW_EH_PE_udata4
  B[kEhFrameHdrOff + 3] = 0x3b; // DW_EH_PE_datarel | sdata4
  putS32(B, kEhFrameHdrOff + 4,
         static_cast<int64_t>(kEhFrameVA) -
             static_cast<int64_t>(kEhFrameHdrVA + 4));
  putU32(B, kEhFrameHdrOff + 8, 1); // fde_count
  putS32(B, kEhFrameHdrOff + 12,
         static_cast<int64_t>(kFunc0VA) - static_cast<int64_t>(kEhFrameHdrVA));
  putS32(B, kEhFrameHdrOff + 16,
         static_cast<int64_t>(kEhFrameVA + 17) -
             static_cast<int64_t>(kEhFrameHdrVA));

  // .shstrtab.
  const char *Names = "\0.text\0.eh_frame\0.eh_frame_hdr\0.shstrtab";
  std::memcpy(B.data() + kShStrTabOff, Names, kShStrTabSize);

  auto shdr = [&](unsigned Idx, uint32_t Name, uint32_t Type, uint64_t Flags,
                  uint64_t Addr, uint64_t Off, uint64_t Size, uint64_t Align) {
    uint64_t H = kShOff + Idx * kShEnt;
    putU32(B, H + 0, Name);
    putU32(B, H + 4, Type);
    putU64(B, H + 8, Flags);
    putU64(B, H + 16, Addr);
    putU64(B, H + 24, Off);
    putU64(B, H + 32, Size);
    putU64(B, H + 48, Align);
  };
  shdr(0, 0, llvm::ELF::SHT_NULL, 0, 0, 0, 0, 0);
  shdr(1, 1, llvm::ELF::SHT_PROGBITS,
       llvm::ELF::SHF_ALLOC | llvm::ELF::SHF_EXECINSTR, kBase + kTextOff,
       kTextOff, 32, 16);
  shdr(2, 7, llvm::ELF::SHT_PROGBITS, llvm::ELF::SHF_ALLOC, kEhFrameVA,
       kEhFrameOff, kEhFrameSize, 8);
  shdr(3, 17, llvm::ELF::SHT_PROGBITS, llvm::ELF::SHF_ALLOC, kEhFrameHdrVA,
       kEhFrameHdrOff, kEhFrameHdrSize, 4);
  shdr(4, 31, llvm::ELF::SHT_STRTAB, 0, 0, kShStrTabOff, kShStrTabSize, 1);
  return B;
}

std::unique_ptr<llvm::Module> makeModule(llvm::LLVMContext &C, bool WithEH) {
  auto M = std::make_unique<llvm::Module>("m", C);
  auto *FT = llvm::FunctionType::get(llvm::Type::getVoidTy(C), false);
  auto *F = llvm::Function::Create(FT, llvm::GlobalValue::ExternalLinkage, "f",
                                   M.get());
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(C, "entry", F));
  B.CreateRetVoid();
  if (WithEH) {
    auto *PT = llvm::FunctionType::get(llvm::Type::getInt32Ty(C), true);
    auto *P = llvm::Function::Create(PT, llvm::GlobalValue::ExternalLinkage,
                                     "__gxx_personality_v0", M.get());
    F->setPersonalityFn(P);
  }
  return M;
}

bool hadError(llvm::Error E) {
  bool B = static_cast<bool>(E);
  llvm::consumeError(std::move(E));
  return B;
}

CompiledImage makeGeneratedEHFrame(const ELFEHFrameRegion &Region) {
  CompiledImage Image;
  Image.Success = true;
  CompiledSection Section;
  Section.Name = ".eh_frame";
  Section.IsAllocated = true;
  Section.IsInImage = false;
  Section.VA = Region.AppendVA;
  Section.ExternalBytes = makeEhFrameFragment(kFunc1VA);
  Section.Size = Section.ExternalBytes.size();
  Image.Sections.push_back(std::move(Section));
  Image.SymbolAddrs["f"] = kFunc1VA;
  return Image;
}

TEST(ELFExceptionPatch, RequiresRegisteredDetectsPersonality) {
  llvm::LLVMContext C;
  EXPECT_TRUE(requiresRegisteredELFEHFrame(*makeModule(C, /*WithEH=*/true)));
  EXPECT_FALSE(requiresRegisteredELFEHFrame(*makeModule(C, /*WithEH=*/false)));
}

TEST(ELFExceptionPatch, FindsRegionAndHeader) {
  std::vector<uint8_t> Bin = makeELF64WithEH();
  auto Region = findELFEHFrameRegion(Bin);
  ASSERT_TRUE(Region.has_value());
  EXPECT_EQ(Region->SectionVA, kEhFrameVA);
  // The append point sits at the logical end of the CIE + FDE, before the
  // zero-length terminator.
  EXPECT_EQ(Region->AppendVA, kEhFrameVA + 42);
  EXPECT_EQ(Region->AppendFileOff, kEhFrameOff + 42);
  EXPECT_TRUE(Region->HasHdr);
  EXPECT_EQ(Region->HdrVA, kEhFrameHdrVA);
  EXPECT_EQ(Region->GnuEhFramePhdrOff, kPhOff + kPhEnt);
}

TEST(ELFExceptionPatch, RejectsMalformedSectionStringTables) {
  std::vector<uint8_t> WrongType = makeELF64WithEH();
  putU32(WrongType, kShOff + 4 * kShEnt + 4, llvm::ELF::SHT_PROGBITS);
  EXPECT_FALSE(findELFEHFrameRegion(WrongType).has_value());

  std::vector<uint8_t> Unterminated = makeELF64WithEH();
  Unterminated[kShStrTabOff + kShStrTabSize - 1] = 'x';
  EXPECT_FALSE(findELFEHFrameRegion(Unterminated).has_value());
}

TEST(ELFExceptionPatch, RejectsMalformedHeaderTables) {
  auto Rejected = [](std::vector<uint8_t> Binary) {
    EXPECT_FALSE(findELFEHFrameRegion(Binary).has_value());
  };

  std::vector<uint8_t> WrongVersion = makeELF64WithEH();
  WrongVersion[llvm::ELF::EI_VERSION] = 0;
  Rejected(std::move(WrongVersion));

  std::vector<uint8_t> WrongHeaderSize = makeELF64WithEH();
  putU16(WrongHeaderSize, 52, 63);
  Rejected(std::move(WrongHeaderSize));

  std::vector<uint8_t> WrongProgramEntrySize = makeELF64WithEH();
  putU16(WrongProgramEntrySize, 54, 8);
  Rejected(std::move(WrongProgramEntrySize));

  std::vector<uint8_t> WrongSectionEntrySize = makeELF64WithEH();
  putU16(WrongSectionEntrySize, 58, 8);
  Rejected(std::move(WrongSectionEntrySize));

  std::vector<uint8_t> NoProgramHeaders = makeELF64WithEH();
  putU16(NoProgramHeaders, 56, 0);
  Rejected(std::move(NoProgramHeaders));

  std::vector<uint8_t> NoSectionHeaders = makeELF64WithEH();
  putU16(NoSectionHeaders, 60, 0);
  Rejected(std::move(NoSectionHeaders));

  std::vector<uint8_t> BadStringIndex = makeELF64WithEH();
  putU16(BadStringIndex, 62, 5);
  Rejected(std::move(BadStringIndex));

  std::vector<uint8_t> ProgramTableOverflow = makeELF64WithEH();
  putU64(ProgramTableOverflow, 32, std::numeric_limits<uint64_t>::max() - 8);
  Rejected(std::move(ProgramTableOverflow));
}

TEST(ELFExceptionPatch, FailsClosedWhenRegistrationImpossible) {
  llvm::LLVMContext C;
  CompiledImage Empty;
  Empty.Success = true;
  std::vector<uint8_t> Bin;
  // A module that needs unwind records, with nothing to place them in, must be
  // rejected rather than written unregistered.
  EXPECT_TRUE(hadError(installELFEHFrame(Bin, std::nullopt, Empty,
                                         *makeModule(C, /*WithEH=*/true))));
  // A module with no exception contract is free to drop CFI-only records.
  EXPECT_FALSE(hadError(installELFEHFrame(Bin, std::nullopt, Empty,
                                          *makeModule(C, /*WithEH=*/false))));
}

TEST(ELFExceptionPatch, InstallsRecordsAndGrowsSearchTable) {
  llvm::LLVMContext C;
  std::vector<uint8_t> Bin = makeELF64WithEH();
  auto Region = findELFEHFrameRegion(Bin);
  ASSERT_TRUE(Region.has_value());

  CompiledImage Img;
  Img = makeGeneratedEHFrame(*Region);
  const CompiledSection &Sec = Img.Sections.front();

  ASSERT_FALSE(hadError(
      installELFEHFrame(Bin, Region, Img, *makeModule(C, /*WithEH=*/true))));

  // The search table went from one entry to two, still sorted by address.
  EXPECT_EQ(getU32(Bin, kEhFrameHdrOff + 8), 2u);
  int32_t Entry1InitRel = getS32(Bin, kEhFrameHdrOff + 20);
  int32_t Entry1FdeRel = getS32(Bin, kEhFrameHdrOff + 24);
  EXPECT_EQ(kEhFrameHdrVA + static_cast<int64_t>(Entry1InitRel), kFunc1VA);
  EXPECT_EQ(kEhFrameHdrVA + static_cast<int64_t>(Entry1FdeRel),
            Region->AppendVA + 17);

  // The covering section and segment grew to match the enlarged table.
  const uint64_t NewHdrSize = 8 + 4 + 2 * 8;
  EXPECT_EQ(getU64(Bin, kShOff + 3 * kShEnt + 32), NewHdrSize);
  EXPECT_EQ(getU64(Bin, kPhOff + kPhEnt + 32), NewHdrSize);
  // `.eh_frame` grew by the appended fragment.
  EXPECT_EQ(getU64(Bin, kShOff + 2 * kShEnt + 32),
            42 + Sec.ExternalBytes.size());
}

TEST(ELFExceptionPatch, RejectsOldHeaderEntriesThatDoNotNameExactFDEs) {
  enum class Corruption { OutsideSection, CIE, FDEInterior, WrongInitial };
  for (Corruption Kind : {Corruption::OutsideSection, Corruption::CIE,
                          Corruption::FDEInterior, Corruption::WrongInitial}) {
    std::vector<uint8_t> Bin = makeELF64WithEH();
    auto Region = findELFEHFrameRegion(Bin);
    ASSERT_TRUE(Region.has_value());
    switch (Kind) {
    case Corruption::OutsideSection:
      putS32(Bin, kEhFrameHdrOff + 16,
             static_cast<int64_t>(kBase) - static_cast<int64_t>(kEhFrameHdrVA));
      break;
    case Corruption::CIE:
      putS32(Bin, kEhFrameHdrOff + 16,
             static_cast<int64_t>(kEhFrameVA) -
                 static_cast<int64_t>(kEhFrameHdrVA));
      break;
    case Corruption::FDEInterior:
      putS32(Bin, kEhFrameHdrOff + 16,
             static_cast<int64_t>(kEhFrameVA + 18) -
                 static_cast<int64_t>(kEhFrameHdrVA));
      break;
    case Corruption::WrongInitial:
      putS32(Bin, kEhFrameHdrOff + 12,
             static_cast<int64_t>(kFunc0VA + 1) -
                 static_cast<int64_t>(kEhFrameHdrVA));
      break;
    }
    const std::vector<uint8_t> Before = Bin;
    llvm::LLVMContext Context;
    CompiledImage Image = makeGeneratedEHFrame(*Region);
    EXPECT_TRUE(hadError(installELFEHFrame(
        Bin, Region, Image, *makeModule(Context, /*WithEH=*/true))));
    EXPECT_EQ(Bin, Before);
  }
}

TEST(ELFExceptionPatch, RejectsForgedRegionWithoutChangingBytes) {
  std::vector<uint8_t> Bin = makeELF64WithEH();
  auto Region = findELFEHFrameRegion(Bin);
  ASSERT_TRUE(Region.has_value());
  ++Region->LimitFileOff;
  const std::vector<uint8_t> Before = Bin;
  llvm::LLVMContext Context;
  CompiledImage Image = makeGeneratedEHFrame(*Region);
  EXPECT_TRUE(hadError(installELFEHFrame(
      Bin, Region, Image, *makeModule(Context, /*WithEH=*/true))));
  EXPECT_EQ(Bin, Before);
}

TEST(ELFExceptionPatch, RejectsInsufficientTailCapacityWithoutChangingBytes) {
  std::vector<uint8_t> Bin = makeELF64WithEH();
  auto Region = findELFEHFrameRegion(Bin);
  ASSERT_TRUE(Region.has_value());
  const std::vector<uint8_t> Before = Bin;
  llvm::LLVMContext Context;
  CompiledImage Image = makeGeneratedEHFrame(*Region);
  CompiledSection &Generated = Image.Sections.front();
  Generated.ExternalBytes.resize(
      static_cast<size_t>(Region->LimitFileOff - Region->AppendFileOff + 1), 0);
  Generated.Size = Generated.ExternalBytes.size();

  EXPECT_TRUE(hadError(installELFEHFrame(
      Bin, Region, Image, *makeModule(Context, /*WithEH=*/true))));
  EXPECT_EQ(Bin, Before);
}

} // namespace

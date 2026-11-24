//===- MIPSObjectTests.cpp - MIPS relocatable ELF code generation ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check emitted ELF headers, ABI records and instructions through Codegen.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/loader/ELF/MIPSImageMetadata.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MipsABIFlags.h"

using namespace neverd;

namespace {

struct ObjectFixture {
  llvm::LLVMContext Context;
  llvm::Module Module{"leaf", Context};
  llvm::Function *Leaf;

  ObjectFixture() {
    llvm::IRBuilder<> B(Context);
    Leaf = llvm::Function::Create(
        llvm::FunctionType::get(B.getInt32Ty(), false),
        llvm::GlobalValue::ExternalLinkage, "leaf", Module);
    B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Leaf));
    B.CreateRet(B.getInt32(37));
  }
};

void checkObject(Arch Target, const MIPSImageMetadata *Profile,
                 unsigned ISALevel, unsigned Revision, unsigned FPABI) {
  SCOPED_TRACE(getArchName(Target));
  ObjectFixture F;
  std::string Diagnostic;
  const auto Emitted =
      Codegen().compile(F.Module, Target, BinaryFormat::ELF, {},
                        ObjectConsumer::PlatformLinker, Profile, Diagnostic);
  ASSERT_TRUE(Emitted.Success) << Diagnostic;
  ASSERT_FALSE(Emitted.ObjectData.empty());
  const llvm::StringRef Bytes(
      reinterpret_cast<const char *>(Emitted.ObjectData.data()),
      Emitted.ObjectData.size());
  auto Object = llvm::object::ObjectFile::createObjectFile(
      llvm::MemoryBufferRef(Bytes, "leaf.o"));
  ASSERT_TRUE(bool(Object)) << llvm::toString(Object.takeError());
  auto *ELF = llvm::dyn_cast<llvm::object::ELFObjectFileBase>(Object->get());
  ASSERT_NE(ELF, nullptr);
  EXPECT_EQ(ELF->getEMachine(), llvm::ELF::EM_MIPS);
  EXPECT_EQ(ELF->getEType(), llvm::ELF::ET_REL);
  EXPECT_EQ(ELF->isLittleEndian(), archLittleEndian(Target));
  const unsigned PointerBytes = isMIPSN64(Target) ? 8 : 4;
  EXPECT_EQ(ELF->getBytesInAddress(), PointerBytes);
  EXPECT_EQ(F.Module.getDataLayout().getPointerSize(), PointerBytes);
  EXPECT_EQ(bool(ELF->getPlatformFlags() & llvm::ELF::EF_MIPS_ABI2),
            isMIPSN32(Target));
  EXPECT_EQ(ELF->getPlatformFlags() & llvm::ELF::EF_MIPS_ABI,
            isMIPSO32(Target) ? unsigned(llvm::ELF::EF_MIPS_ABI_O32) : 0);
  if (Profile) {
    EXPECT_EQ(bool(ELF->getPlatformFlags() & llvm::ELF::EF_MIPS_PIC),
              bool(Profile->ELFFlags & llvm::ELF::EF_MIPS_PIC));
    EXPECT_EQ(bool(ELF->getPlatformFlags() & llvm::ELF::EF_MIPS_CPIC),
              bool(Profile->ELFFlags &
                   (llvm::ELF::EF_MIPS_PIC | llvm::ELF::EF_MIPS_CPIC)));
  }
  ASSERT_EQ(Emitted.Functions.size(), 1u);
  EXPECT_EQ(Emitted.Functions[0].Name, "leaf");
  EXPECT_GT(Emitted.Functions[0].Size, 0u);
  bool SawABI = false, SawText = false;
  for (const auto &Section : ELF->sections()) {
    auto Name = Section.getName();
    ASSERT_TRUE(bool(Name)) << llvm::toString(Name.takeError());
    if (*Name != ".MIPS.abiflags" && *Name != ".text")
      continue;
    auto Data = Section.getContents();
    ASSERT_TRUE(bool(Data)) << llvm::toString(Data.takeError());
    if (*Name == ".MIPS.abiflags") {
      SawABI = true;
      ASSERT_EQ(Data->size(), 24u);
      EXPECT_EQ((*Data)[2], ISALevel);
      EXPECT_EQ((*Data)[3], Revision);
      EXPECT_EQ((*Data)[4], isMIPS64(Target) ? llvm::Mips::AFL_REG_64
                                             : llvm::Mips::AFL_REG_32);
      EXPECT_EQ((*Data)[7], FPABI);
      if (Profile && Profile->HasMSA) {
        EXPECT_EQ((*Data)[5], llvm::Mips::AFL_REG_128);
        const auto ASE =
            ELF->isLittleEndian()
                ? llvm::support::endian::read32le(Data->data() + 12)
                : llvm::support::endian::read32be(Data->data() + 12);
        EXPECT_NE(ASE & llvm::Mips::AFL_ASE_MSA, 0u);
      }
    } else {
      SawText = true;
      EXPECT_FALSE(Data->empty());
      if (Revision != 6) {
        // Classic pre-R6 returns through $ra and owns the following delay slot.
        bool FoundReturn = false;
        for (size_t I = 0; I + 8 <= Data->size(); I += 4) {
          const char *Word = Data->data() + I;
          const auto Value = ELF->isLittleEndian()
                                 ? llvm::support::endian::read32le(Word)
                                 : llvm::support::endian::read32be(Word);
          FoundReturn |= Value == 0x03e00008;
        }
        EXPECT_TRUE(FoundReturn);
      }
    }
  }
  EXPECT_TRUE(SawABI);
  EXPECT_TRUE(SawText);
}

TEST(MIPSObject, ClassicGPRAndPointerWidthsAreIndependent) {
  using namespace llvm::Mips;
  for (auto Target : {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64N32,
                      Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL})
    checkObject(Target, nullptr, isMIPS64(Target) ? 64 : 32, 2,
                Val_GNU_MIPS_ABI_FP_DOUBLE);
  for (auto Target : {Arch::MIPS64O32, Arch::MIPS64O32EL}) {
    MIPSImageMetadata Profile;
    Profile.HistoricalISA = 3;
    Profile.ISARevision = 0;
    Profile.FPABI = Val_GNU_MIPS_ABI_FP_DOUBLE;
    checkObject(Target, &Profile, 3, 0, Val_GNU_MIPS_ABI_FP_DOUBLE);
  }
}

TEST(MIPSObject, ImageRevisionAndFPABIReachTheObjectWriter) {
  using namespace llvm::Mips;
  for (auto Target : {Arch::MIPS32, Arch::MIPS32EL}) {
    MIPSImageMetadata Profile;
    for (auto ABI : {Val_GNU_MIPS_ABI_FP_DOUBLE, Val_GNU_MIPS_ABI_FP_SINGLE,
                     Val_GNU_MIPS_ABI_FP_SOFT, Val_GNU_MIPS_ABI_FP_XX,
                     Val_GNU_MIPS_ABI_FP_64, Val_GNU_MIPS_ABI_FP_64A}) {
      Profile.FPABI = ABI;
      checkObject(Target, &Profile, 32, 2, ABI);
    }
    Profile.ISARevision = 6;
    Profile.ELFFlags = llvm::ELF::EF_MIPS_NAN2008;
    Profile.FPABI = Val_GNU_MIPS_ABI_FP_64;
    checkObject(Target, &Profile, 32, 6, Val_GNU_MIPS_ABI_FP_64);
  }
}

TEST(MIPSObject, MSAUsesTheRelease5ABIInBothByteOrders) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64N32,
                      Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    MIPSImageMetadata Profile;
    Profile.ABI = isMIPSN64(Target)   ? MIPSABI::N64
                  : isMIPSN32(Target) ? MIPSABI::N32
                                      : MIPSABI::O32;
    Profile.ISARevision = 5;
    Profile.HasMSA = true;
    Profile.FPABI = isMIPSO32(Target) ? llvm::Mips::Val_GNU_MIPS_ABI_FP_64
                                      : llvm::Mips::Val_GNU_MIPS_ABI_FP_DOUBLE;
    checkObject(Target, &Profile, isMIPS64(Target) ? 64 : 32, 5,
                *Profile.FPABI);
  }
}

TEST(MIPSObject, HistoricalELFRecordsRetainTheOriginalISA) {
  using namespace llvm::Mips;
  for (auto Target : {Arch::MIPS32, Arch::MIPS32EL}) {
    for (unsigned ISA : {1, 2}) {
      MIPSImageMetadata Profile;
      Profile.HistoricalISA = ISA;
      Profile.ISARevision = 0;
      Profile.FPABI = Val_GNU_MIPS_ABI_FP_SOFT;
      checkObject(Target, &Profile, ISA, 0, Val_GNU_MIPS_ABI_FP_SOFT);
    }
  }
  for (auto Target :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    for (unsigned ISA : {3, 4, 5}) {
      MIPSImageMetadata Profile;
      Profile.ABI = isMIPSN32(Target) ? MIPSABI::N32 : MIPSABI::N64;
      Profile.HistoricalISA = ISA;
      Profile.ISARevision = 0;
      Profile.FPABI = Val_GNU_MIPS_ABI_FP_DOUBLE;
      checkObject(Target, &Profile, ISA, 0, Val_GNU_MIPS_ABI_FP_DOUBLE);
    }
  }
}

TEST(MIPSObject, RejectsWrongTriplesAndUnsupportedInstructionSelectors) {
  ObjectFixture F;
  for (const char *Triple : {"mipsel-linux-gnu", "mips64-linux-gnuabin32",
                             "mips64el-linux-gnuabi64", "aarch64-linux-gnu"})
    EXPECT_FALSE(
        Codegen()
            .compile(F.Module, Arch::MIPS64N32EL, BinaryFormat::ELF, Triple)
            .Success);
  for (auto Format : {BinaryFormat::COFF, BinaryFormat::MachO})
    EXPECT_FALSE(Codegen().compile(F.Module, Arch::MIPS32, Format).Success);

  std::string Diagnostic;
  MIPSImageMetadata Profile;
  Profile.ABI = MIPSABI::N32;
  EXPECT_FALSE(Codegen()
                   .compile(F.Module, Arch::MIPS64, BinaryFormat::ELF, {},
                            ObjectConsumer::PlatformLinker, &Profile,
                            Diagnostic)
                   .Success);
  F.Leaf->addFnAttr("micromips");
  EXPECT_FALSE(
      Codegen().compile(F.Module, Arch::MIPS64, BinaryFormat::ELF).Success);
  F.Leaf->removeFnAttr("micromips");
  F.Leaf->addFnAttr("target-features", "+micromips");
  EXPECT_FALSE(
      Codegen().compile(F.Module, Arch::MIPS64N32, BinaryFormat::ELF).Success);
  F.Leaf->addFnAttr("target-features", "+noabicalls");
  EXPECT_FALSE(
      Codegen().compile(F.Module, Arch::MIPS32, BinaryFormat::ELF).Success);
}

TEST(MIPSObject, InvalidFunctionFeaturesReturnDiagnosticsToTheCaller) {
  ObjectFixture F;
  MIPSImageMetadata Profile;
  std::string Diagnostic;
  auto Check = [&](Arch Target, const char *Features, const char *Expected) {
    SCOPED_TRACE(Features);
    F.Leaf->addFnAttr("target-features", Features);
    auto Emitted =
        Codegen().compile(F.Module, Target, BinaryFormat::ELF, {},
                          ObjectConsumer::PlatformLinker, &Profile, Diagnostic);
    EXPECT_FALSE(Emitted.Success);
    EXPECT_TRUE(Emitted.ObjectData.empty());
    EXPECT_NE(Diagnostic.find(Expected), std::string::npos) << Diagnostic;
    EXPECT_EQ(F.Leaf->getFnAttribute("target-features").getValueAsString(),
              Features);
  };
  Profile.ISARevision = 5;
  Profile.HasMSA = true;
  Profile.FPABI = llvm::Mips::Val_GNU_MIPS_ABI_FP_64;
  Check(Arch::MIPS32, "-fp64", "MSA requires");
  Profile.ABI = MIPSABI::N32;
  Profile.FPABI = llvm::Mips::Val_GNU_MIPS_ABI_FP_DOUBLE;
  Check(Arch::MIPS64N32EL, "+fpxx", "require o32");
  Check(Arch::MIPS64N32EL, "+nooddspreg", "require o32");
  Check(Arch::MIPS64N32EL, "+unknown-isa", "unknown target feature");
}

TEST(MIPSObject, CompressedFunctionRetainsSymbolModeAndImageFPABI) {
  for (bool Mips16 : {false, true})
    for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL}) {
      SCOPED_TRACE(getArchName(Target));
      SCOPED_TRACE(Mips16);
      ObjectFixture F;
      MIPSImageMetadata Profile;
      Profile.HasMicroMIPS = !Mips16;
      Profile.HasMIPS16 = Mips16;
      Profile.FPABI = llvm::Mips::Val_GNU_MIPS_ABI_FP_SOFT;
      F.Leaf->addFnAttr(Mips16 ? "mips16" : "micromips");
      F.Leaf->addFnAttr("target-features", "-msa");
      std::string Diagnostic;
      auto Result = Codegen().compile(F.Module, Target, BinaryFormat::ELF, {},
                                      ObjectConsumer::PlatformLinker, &Profile,
                                      Diagnostic);
      ASSERT_TRUE(Result.Success) << Diagnostic;
      ASSERT_EQ(Result.Functions.size(), 1u);
      const llvm::StringRef Bytes(
          reinterpret_cast<const char *>(Result.ObjectData.data()),
          Result.ObjectData.size());
      auto Obj = llvm::object::ObjectFile::createObjectFile(
          llvm::MemoryBufferRef(Bytes, "compressed.o"));
      ASSERT_TRUE(bool(Obj)) << llvm::toString(Obj.takeError());
      const auto &ELF = llvm::cast<llvm::object::ELFObjectFileBase>(**Obj);
      bool FoundABI = false, FoundLeaf = false;
      for (const auto &Symbol : ELF.symbols()) {
        auto Name = Symbol.getName();
        ASSERT_TRUE(bool(Name)) << llvm::toString(Name.takeError());
        if (*Name == "leaf") {
          FoundLeaf = true;
          EXPECT_EQ(Symbol.getOther(), Mips16 ? llvm::ELF::STO_MIPS_MIPS16
                                              : llvm::ELF::STO_MIPS_MICROMIPS);
        }
      }
      for (const auto &Section : ELF.sections()) {
        if (llvm::object::ELFSectionRef(Section).getType() !=
            llvm::ELF::SHT_MIPS_ABIFLAGS)
          continue;
        auto Data = Section.getContents();
        ASSERT_TRUE(bool(Data)) << llvm::toString(Data.takeError());
        ASSERT_EQ(Data->size(), 24u);
        EXPECT_EQ((*Data)[7], llvm::Mips::Val_GNU_MIPS_ABI_FP_SOFT);
        const uint32_t ASE =
            ELF.isLittleEndian()
                ? llvm::support::endian::read32le(Data->data() + 12)
                : llvm::support::endian::read32be(Data->data() + 12);
        EXPECT_NE(ASE & (Mips16 ? llvm::Mips::AFL_ASE_MIPS16
                                : llvm::Mips::AFL_ASE_MICROMIPS),
                  0u);
        FoundABI = true;
      }
      EXPECT_TRUE(FoundABI);
      EXPECT_TRUE(FoundLeaf);
    }
}

TEST(MIPSObject, FixedAddressEmissionKeepsEntryPointersAndFunctionRanges) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64N32,
                      Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    SCOPED_TRACE(getArchName(Target));
    ObjectFixture F;
    new llvm::GlobalVariable(F.Module, F.Leaf->getType(), false,
                             llvm::GlobalValue::ExternalLinkage, F.Leaf,
                             "entry");
    F.Leaf->setUWTableKind(llvm::UWTableKind::Default);
    const uint64_t Base = isMIPSN64(Target) ? 0x1200010000ULL : 0x10000;
    llvm::mc_rewrite::RewriteOptions Opt;
    Opt.Model.TextVA = Base;
    Opt.Model.getSectionVA = [Base](llvm::StringRef Name) {
      return Name == ".text" ? Base : Base + 0x10000;
    };
    auto Result =
        Codegen().compileForRewrite(F.Module, Target, Opt, BinaryFormat::ELF);
    ASSERT_TRUE(Result.ImageValid);
    ASSERT_TRUE(Result.FunctionRangesValid);
    EXPECT_TRUE(Result.Unresolved.empty());
    ASSERT_EQ(Result.FunctionRanges.size(), 1u);
    const auto &Range = Result.FunctionRanges.front();
    EXPECT_EQ(Range.OwnerSymbol, "leaf");
    EXPECT_EQ(Range.BeginVA, Base);
    EXPECT_EQ(Range.EndVA, Base + 8);
    const auto Data = llvm::find_if(Result.Sections, [](const auto &Section) {
      return Section.Name == ".data";
    });
    ASSERT_NE(Data, Result.Sections.end());
    const auto Endian = archLittleEndian(Target) ? llvm::endianness::little
                                                 : llvm::endianness::big;
    ASSERT_GE(Data->Bytes.size(), isMIPSN64(Target) ? 8u : 4u);
    EXPECT_EQ(isMIPSN64(Target)
                  ? llvm::support::endian::read64(Data->Bytes.data(), Endian)
                  : llvm::support::endian::read32(Data->Bytes.data(), Endian),
              Base);
  }
}

TEST(MIPSObject, EquivalentTriplesSelectTheSameABI) {
  for (const auto &[Target, Triple] :
       {std::pair{Arch::MIPS32, "mips-linux-musl"},
        std::pair{Arch::MIPS64EL, "mips64el-linux-gnu"},
        std::pair{Arch::MIPS64N32EL, "mips64el-linux-muslabin32"}}) {
    ObjectFixture F;
    EXPECT_TRUE(
        Codegen().compile(F.Module, Target, BinaryFormat::ELF, Triple).Success);
  }
  ObjectFixture F;
  EXPECT_FALSE(Codegen()
                   .compile(F.Module, Arch::MIPS32, BinaryFormat::ELF,
                            "mips-linux-gnuabi64")
                   .Success);
}

} // namespace

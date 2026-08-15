//===- MachOExceptionPatchTests.cpp - Mach-O EH frame layout tests -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/DwarfEHFrame.h"
#include "neverd/backend/codegen/MachO/MachOCompactUnwindPatch.h"
#include "neverd/backend/codegen/MachO/MachOExceptionPatch.h"
#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/object/SectionNames.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace {

using namespace neverd;
using namespace llvm::MachO;

constexpr uint64_t kBaseVA = 0x100000000;
constexpr uint32_t kEHFrameOff = 300;
constexpr uint64_t kEHFrameSize = 16;
constexpr uint32_t kTextOff = 400;
constexpr uint64_t kTextSize = 16;

void appendU32(std::vector<uint8_t> &Bytes, uint32_t Value) {
  for (unsigned I = 0; I < 4; ++I)
    Bytes.push_back(static_cast<uint8_t>(Value >> (I * 8)));
}

void appendU64(std::vector<uint8_t> &Bytes, uint64_t Value) {
  for (unsigned I = 0; I < 8; ++I)
    Bytes.push_back(static_cast<uint8_t>(Value >> (I * 8)));
}

void putU32(std::vector<uint8_t> &Bytes, size_t Offset, uint32_t Value) {
  for (unsigned I = 0; I < 4; ++I)
    Bytes[Offset + I] = static_cast<uint8_t>(Value >> (I * 8));
}

uint64_t getU64(llvm::ArrayRef<uint8_t> Bytes, size_t Offset) {
  uint64_t Value = 0;
  for (unsigned I = 0; I < 8; ++I)
    Value |= uint64_t(Bytes[Offset + I]) << (I * 8);
  return Value;
}

uint32_t getU32(llvm::ArrayRef<uint8_t> Bytes, size_t Offset) {
  uint32_t Value = 0;
  for (unsigned I = 0; I < 4; ++I)
    Value |= uint32_t(Bytes[Offset + I]) << (I * 8);
  return Value;
}

std::vector<uint8_t> makeEHFrameFragment(uint64_t FunctionVA) {
  std::vector<uint8_t> Bytes;
  const std::vector<uint8_t> CIE = {0, 0, 0,    0,  1, 'z', 'R',
                                    0, 1, 0x78, 16, 1, 0};
  appendU32(Bytes, static_cast<uint32_t>(CIE.size()));
  Bytes.insert(Bytes.end(), CIE.begin(), CIE.end());

  appendU32(Bytes, 21);
  appendU32(Bytes, 21);
  appendU64(Bytes, FunctionVA);
  appendU64(Bytes, 0x10);
  Bytes.push_back(0);
  appendU32(Bytes, 0);
  return Bytes;
}

void setName(char (&Destination)[16], llvm::StringRef Name) {
  ASSERT_LT(Name.size(), sizeof(Destination));
  std::memcpy(Destination, Name.data(), Name.size());
}

std::vector<uint8_t> makeMachO64(uint32_t TextOff = kTextOff) {
  constexpr uint32_t CommandSize =
      sizeof(segment_command_64) + 2 * sizeof(section_64);
  std::vector<uint8_t> Binary(512, 0);
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
  setName(Segment->segname, "__TEXT");
  Segment->vmaddr = kBaseVA;
  Segment->vmsize = Binary.size();
  Segment->fileoff = 0;
  Segment->filesize = Binary.size();
  Segment->maxprot = 5;
  Segment->initprot = 5;
  Segment->nsects = 2;

  auto *EHFrame = reinterpret_cast<section_64 *>(Segment + 1);
  setName(EHFrame->sectname, "__eh_frame");
  setName(EHFrame->segname, "__TEXT");
  EHFrame->addr = kBaseVA + kEHFrameOff;
  EHFrame->size = kEHFrameSize;
  EHFrame->offset = kEHFrameOff;
  EHFrame->align = 2;
  EHFrame->flags = S_REGULAR;

  auto *Text = EHFrame + 1;
  setName(Text->sectname, "__text");
  setName(Text->segname, "__TEXT");
  Text->addr = kBaseVA + TextOff;
  Text->size = kTextSize;
  Text->offset = TextOff;
  Text->align = 2;
  Text->flags = S_REGULAR;
  return Binary;
}

void setExistingEHFrame(std::vector<uint8_t> &Binary,
                        llvm::ArrayRef<uint8_t> Bytes) {
  ASSERT_LE(Bytes.size(), kTextOff - kEHFrameOff);
  std::memcpy(Binary.data() + kEHFrameOff, Bytes.data(), Bytes.size());
  auto *Segment = reinterpret_cast<segment_command_64 *>(
      Binary.data() + sizeof(mach_header_64));
  reinterpret_cast<section_64 *>(Segment + 1)->size = Bytes.size();
}

std::unique_ptr<llvm::Module> makeRequiredModule(llvm::LLVMContext &Context) {
  auto Module = std::make_unique<llvm::Module>("m", Context);
  auto *Type = llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto *Function = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, "f", Module.get());
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  Builder.CreateRetVoid();
  Function->setUWTableKind(llvm::UWTableKind::Default);
  return Module;
}

std::unique_ptr<llvm::Module>
makeCompactUnwindEHModule(llvm::LLVMContext &Context) {
  auto Module = std::make_unique<llvm::Module>("compact-unwind-eh", Context);
  auto *VoidFunctionType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto *PersonalityType = llvm::FunctionType::get(
      llvm::Type::getInt32Ty(Context), /*isVarArg=*/true);
  auto *Personality = llvm::Function::Create(
      PersonalityType, llvm::GlobalValue::ExternalLinkage,
      "__gxx_personality_v0", Module.get());
  auto *MayThrow = llvm::Function::Create(VoidFunctionType,
                                          llvm::GlobalValue::ExternalLinkage,
                                          "may_throw", Module.get());
  auto *Function = llvm::Function::Create(
      VoidFunctionType, llvm::GlobalValue::ExternalLinkage, "f", Module.get());
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

void addGeneratedFunctionProvenance(CompiledImage &Image, uint64_t FunctionVA) {
  Image.Success = true;
  Image.TargetArch = Arch::AArch64;
  Image.Format = BinaryFormat::MachO;
  Image.PointerWidth = 8;
  Image.ByteOrder = llvm::endianness::little;
  CompiledSection Code;
  Code.Name = "__text";
  Code.VA = FunctionVA;
  Code.Size = 0x10;
  Code.Kind = llvm::mc_rewrite::RewriteSectionKind::Code;
  Code.IsAllocated = true;
  Image.Sections.push_back(std::move(Code));
  Image.SymbolAddrs["f"] = FunctionVA;
  Image.FunctionOwnerAddrs["_f"] = FunctionVA;
  Image.SourceFunctionOwners.push_back({"f", "_f", FunctionVA});
  Image.FunctionRanges.push_back(
      {1, "_f", FunctionVA, "L_begin_f", FunctionVA, "L_end_f",
       FunctionVA + 0x10});
}

CompiledImage makeGeneratedEHFrame(const MachOEHFrameRegion &Region,
                                   uint64_t FunctionVA) {
  CompiledSection Section;
  Section.Name = "__eh_frame";
  Section.IsAllocated = true;
  Section.IsInImage = false;
  Section.VA = Region.AppendVA;
  Section.ExternalBytes = makeEHFrameFragment(FunctionVA);
  Section.Size = Section.ExternalBytes.size();

  CompiledImage Image;
  Image.Sections.push_back(std::move(Section));
  addGeneratedFunctionProvenance(Image, FunctionVA);
  return Image;
}

bool hadError(llvm::Error Error) {
  const bool Failed = static_cast<bool>(Error);
  llvm::consumeError(std::move(Error));
  return Failed;
}

llvm::GlobalVariable &addCompactUnwindBytes(llvm::Module &Module,
                                            llvm::ArrayRef<uint8_t> Bytes) {
  llvm::Constant *Initializer =
      llvm::ConstantDataArray::get(Module.getContext(), Bytes);
  auto *Global = new llvm::GlobalVariable(
      Module, Initializer->getType(), /*isConstant=*/true,
      llvm::GlobalValue::PrivateLinkage, Initializer, "compact_unwind_record");
  Global->setSection("__LD,__compact_unwind,regular,debug");
  Global->setAlignment(llvm::Align(1));
  return *Global;
}

const CompiledSection *findCompiledSection(const CompiledImage &Image,
                                           llvm::StringRef Name) {
  for (const CompiledSection &Section : Image.Sections)
    if (Section.Name == Name)
      return &Section;
  return nullptr;
}

TEST(CompiledImage, PreservesSingleMachONonAllocatedMetadataBytes) {
  const std::vector<uint8_t> RawBytes = {
      0x11, 0x29, 0x37, 0x43, 0x59, 0x61, 0x73, 0x83,
      0x97, 0xa1, 0xb3, 0xc7, 0xd9, 0xe5, 0xf1, 0x0d,
  };
  llvm::LLVMContext Context;
  llvm::Module Module("single-compact-unwind-metadata", Context);
  addCompactUnwindBytes(Module, RawBytes);

  CompiledImage Image = compileImageForPatch(
      Module, Arch::AArch64, BinaryFormat::MachO, kBaseVA,
      [](llvm::StringRef, uint32_t) -> std::optional<uint64_t> {
        return std::nullopt;
      },
      kBaseVA);

  ASSERT_TRUE(Image.Success);
  EXPECT_TRUE(Image.Bytes.empty());
  const CompiledSection *Metadata =
      findCompiledSection(Image, section_names::macho::CompactUnwind);
  ASSERT_NE(Metadata, nullptr);
  EXPECT_FALSE(Metadata->IsAllocated);
  EXPECT_FALSE(Metadata->IsInImage);
  EXPECT_EQ(Metadata->Offset, 0u);
  EXPECT_EQ(Metadata->VA, 0u);
  EXPECT_EQ(Metadata->Size, RawBytes.size());
  EXPECT_EQ(Metadata->ExternalBytes, RawBytes);
}

TEST(CompiledImage, KeepsMachOMetadataOutsideMultiSectionImage) {
  const std::vector<uint8_t> RawBytes = {
      0x03, 0x17, 0x2b, 0x3f, 0x53, 0x67, 0x7b, 0x8f,
      0xa3, 0xb7, 0xcb, 0xdf, 0xf3, 0x07, 0x1b, 0x2f,
  };
  llvm::LLVMContext Context;
  llvm::Module Module("multi-compact-unwind-metadata", Context);
  auto *FunctionType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto *Function = llvm::Function::Create(
      FunctionType, llvm::GlobalValue::ExternalLinkage, "f", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  Builder.CreateRetVoid();
  addCompactUnwindBytes(Module, RawBytes);

  CompiledImage Image = compileImageForPatch(
      Module, Arch::AArch64, BinaryFormat::MachO, kBaseVA,
      [](llvm::StringRef, uint32_t) -> std::optional<uint64_t> {
        return std::nullopt;
      },
      kBaseVA);

  ASSERT_TRUE(Image.Success);
  const CompiledSection *Metadata =
      findCompiledSection(Image, section_names::macho::CompactUnwind);
  ASSERT_NE(Metadata, nullptr);
  EXPECT_FALSE(Metadata->IsAllocated);
  EXPECT_FALSE(Metadata->IsInImage);
  EXPECT_EQ(Metadata->Offset, 0u);
  EXPECT_EQ(Metadata->VA, 0u);
  ASSERT_EQ(Metadata->Size, Metadata->ExternalBytes.size());
  ASSERT_GT(Metadata->ExternalBytes.size(), RawBytes.size());
  EXPECT_TRUE(std::equal(RawBytes.begin(), RawBytes.end(),
                         Metadata->ExternalBytes.begin()));

  bool HasAllocatedSection = false;
  for (const CompiledSection &Section : Image.Sections) {
    if (!Section.IsAllocated)
      continue;
    HasAllocatedSection = true;
    EXPECT_TRUE(Section.IsInImage);
    EXPECT_TRUE(Section.ExternalBytes.empty());
    ASSERT_LE(Section.Offset, Image.Bytes.size());
    EXPECT_LE(Section.Size, Image.Bytes.size() - Section.Offset);
  }
  EXPECT_TRUE(HasAllocatedSection);
  EXPECT_EQ(std::search(Image.Bytes.begin(), Image.Bytes.end(),
                        RawBytes.begin(), RawBytes.end()),
            Image.Bytes.end());
}

TEST(CompiledImage, OwnsFinalMachOCompactUnwindFixupProvenance) {
  constexpr uint64_t TextVA = kBaseVA + 0x1000;
  constexpr uint64_t PersonalityVA = kBaseVA + 0x8000;
  constexpr uint64_t MayThrowVA = kBaseVA + 0x9000;
  constexpr uint64_t UnwindResumeVA = kBaseVA + 0xa000;
  llvm::LLVMContext Context;
  auto Module = makeCompactUnwindEHModule(Context);
  std::string Verification;
  llvm::raw_string_ostream VerificationOS(Verification);
  ASSERT_FALSE(llvm::verifyModule(*Module, &VerificationOS)) << Verification;

  CompiledImage Image = compileImageForPatch(
      *Module, Arch::X64, BinaryFormat::MachO, TextVA,
      [=](llvm::StringRef Symbol, uint32_t) -> std::optional<uint64_t> {
        if (Symbol.contains("gxx_personality_v0"))
          return PersonalityVA;
        if (Symbol.contains("may_throw"))
          return MayThrowVA;
        if (Symbol.contains("Unwind_Resume"))
          return UnwindResumeVA;
        return std::nullopt;
      },
      kBaseVA);

  ASSERT_TRUE(Image.Success);
  ASSERT_TRUE(Image.Unresolved.empty());
  const CompiledSection *Compact =
      findCompiledSection(Image, section_names::macho::CompactUnwind);
  ASSERT_NE(Compact, nullptr);
  ASSERT_FALSE(Compact->IsAllocated);
  ASSERT_FALSE(Compact->IsInImage);
  ASSERT_EQ(Compact->ExternalBytes.size(), 32u);
  EXPECT_EQ(Compact->Size, Compact->ExternalBytes.size());

  auto FindUniqueFixup = [&](uint64_t Offset) {
    const CompiledFixupReference *Result = nullptr;
    for (const CompiledFixupReference &Reference : Compact->FixupReferences) {
      EXPECT_LT(Reference.Offset, Compact->ExternalBytes.size());
      if (Reference.Offset != Offset)
        continue;
      EXPECT_EQ(Result, nullptr)
          << "duplicate fixup at compact offset " << Offset;
      Result = &Reference;
    }
    return Result;
  };

  const CompiledFixupReference *FunctionFixup = FindUniqueFixup(0);
  const CompiledFixupReference *PersonalityFixup = FindUniqueFixup(16);
  const CompiledFixupReference *LSDAFixup = FindUniqueFixup(24);
  ASSERT_NE(FunctionFixup, nullptr);
  ASSERT_NE(PersonalityFixup, nullptr);
  ASSERT_NE(LSDAFixup, nullptr);

  EXPECT_TRUE(FunctionFixup->SubtractSymbol.empty());
  EXPECT_NE(PersonalityFixup->Symbol.find("gxx_personality_v0"),
            std::string::npos);
  EXPECT_TRUE(PersonalityFixup->SubtractSymbol.empty());
  EXPECT_FALSE(LSDAFixup->Symbol.empty());
  EXPECT_TRUE(LSDAFixup->SubtractSymbol.empty());
  for (const CompiledFixupReference *Reference :
       {FunctionFixup, PersonalityFixup, LSDAFixup}) {
    EXPECT_EQ(Reference->Kind, llvm::FK_Data_8);
    EXPECT_EQ(Reference->Addend, 0);
    EXPECT_EQ(Reference->Specifier, 0u);
    EXPECT_FALSE(Reference->IsPCRel);
    EXPECT_TRUE(Reference->IsResolved);
    EXPECT_EQ(Reference->BitWidth, 64u);
  }

  auto Function = Image.SymbolAddrs.find("_f");
  ASSERT_NE(Function, Image.SymbolAddrs.end());
  EXPECT_EQ(getU64(Compact->ExternalBytes, 0), Function->second);
  EXPECT_EQ(getU64(Compact->ExternalBytes, 16), PersonalityVA);
  const uint64_t LSDAVA = getU64(Compact->ExternalBytes, 24);
  EXPECT_NE(LSDAVA, 0u);
  bool LSDAIsAllocated = false;
  for (const CompiledSection &Section : Image.Sections)
    if (Section.IsAllocated && LSDAVA >= Section.VA &&
        LSDAVA - Section.VA < Section.Size)
      LSDAIsAllocated = true;
  EXPECT_TRUE(LSDAIsAllocated);

  ASSERT_TRUE(Image.FunctionRangesValid);
  ASSERT_EQ(Image.FunctionRanges.size(), 1u);
  const CompiledFunctionRange &Range = Image.FunctionRanges.front();
  EXPECT_NE(Range.Id, 0u);
  EXPECT_EQ(Range.OwnerSymbol, "_f");
  EXPECT_EQ(Range.OwnerVA, Function->second);
  EXPECT_FALSE(Range.BeginSymbol.empty());
  EXPECT_FALSE(Range.EndSymbol.empty());
  EXPECT_NE(Range.BeginSymbol, Range.EndSymbol);
  EXPECT_EQ(FunctionFixup->Symbol, Range.BeginSymbol);
  EXPECT_EQ(Range.BeginVA, Function->second);
  EXPECT_EQ(Range.EndVA, Range.BeginVA + getU32(Compact->ExternalBytes, 8));
  EXPECT_GT(Range.EndVA, Range.BeginVA);

  const CompiledSection *OwningCode = nullptr;
  for (const CompiledSection &Section : Image.Sections) {
    if (!Section.IsAllocated ||
        Section.Kind != llvm::mc_rewrite::RewriteSectionKind::Code ||
        Range.BeginVA < Section.VA || Range.EndVA < Range.BeginVA ||
        Range.EndVA - Section.VA > Section.Size)
      continue;
    ASSERT_EQ(OwningCode, nullptr);
    OwningCode = &Section;
  }
  ASSERT_NE(OwningCode, nullptr);
  EXPECT_GE(Range.OwnerVA, OwningCode->VA);
  EXPECT_LT(Range.OwnerVA - OwningCode->VA, OwningCode->Size);
  // A half-open fragment may end exactly at its code section boundary.
  EXPECT_EQ(Range.EndVA - OwningCode->VA, OwningCode->Size);

  // CFI range endpoints retain exact private identities for provenance without
  // being published as ordinary program symbols.
  EXPECT_EQ(Image.SymbolAddrs.count(Range.BeginSymbol), 0u);
  EXPECT_EQ(Image.SymbolAddrs.count(Range.EndSymbol), 0u);
  EXPECT_EQ(Image.SymbolAddrs.count("_f"), 1u);
}

TEST(RewriteFunctionRange, ValidatesIdentityAndExactGlobalRanges) {
  using llvm::mc_rewrite::RewriteFunctionRange;

  const std::map<std::string, uint64_t> Symbols = {
      {"_f", 0x1000},
      {"_g", 0x3000},
  };
  const RewriteFunctionRange First{9,      "_f",     0x1000, "Lbegin_f",
                                   0x1000, "Lend_f", 0x1010};
  const RewriteFunctionRange Second{3,      "_g",     0x3000, "Lbegin_g",
                                    0x3000, "Lend_g", 0x3020};

  // IDs are opaque: valid ranges need not be ordered by ID or address.
  EXPECT_TRUE(llvm::mc_rewrite::validateRewriteFunctionRanges({Second, First},
                                                              Symbols));

  RewriteFunctionRange Invalid = First;
  Invalid.Id = 0;
  EXPECT_FALSE(
      llvm::mc_rewrite::validateRewriteFunctionRanges({Invalid}, Symbols));
  EXPECT_FALSE(llvm::mc_rewrite::validateRewriteFunctionRanges(
      {First,
       RewriteFunctionRange{First.Id, "_g", 0x3000, "Lbegin_duplicate_id",
                            0x3000, "Lend_duplicate_id", 0x3010}},
      Symbols));

  Invalid = First;
  Invalid.OwnerSymbol.clear();
  EXPECT_FALSE(
      llvm::mc_rewrite::validateRewriteFunctionRanges({Invalid}, Symbols));
  Invalid = First;
  Invalid.BeginSymbol.clear();
  EXPECT_FALSE(
      llvm::mc_rewrite::validateRewriteFunctionRanges({Invalid}, Symbols));
  Invalid = First;
  Invalid.EndSymbol.clear();
  EXPECT_FALSE(
      llvm::mc_rewrite::validateRewriteFunctionRanges({Invalid}, Symbols));
  Invalid = First;
  Invalid.OwnerVA = 0x1001;
  EXPECT_FALSE(
      llvm::mc_rewrite::validateRewriteFunctionRanges({Invalid}, Symbols));
  Invalid = First;
  Invalid.EndVA = Invalid.BeginVA;
  EXPECT_FALSE(
      llvm::mc_rewrite::validateRewriteFunctionRanges({Invalid}, Symbols));
  Invalid = First;
  Invalid.BeginVA = std::numeric_limits<uint64_t>::max();
  Invalid.EndVA = 0;
  EXPECT_FALSE(
      llvm::mc_rewrite::validateRewriteFunctionRanges({Invalid}, Symbols));

  const RewriteFunctionRange Overlap{11,     "_g",      0x3000, "Lbegin_ov",
                                     0x1008, "Lend_ov", 0x1020};
  EXPECT_FALSE(llvm::mc_rewrite::validateRewriteFunctionRanges({First, Overlap},
                                                               Symbols));
  const RewriteFunctionRange Duplicate{12,     "_g",       0x3000, "Lbegin_dup",
                                       0x1000, "Lend_dup", 0x1010};
  EXPECT_FALSE(llvm::mc_rewrite::validateRewriteFunctionRanges(
      {First, Duplicate}, Symbols));
}

TEST(CompiledImage, AuthenticatesEveryGeneratedCFIFragment) {
  constexpr uint64_t TextVA = kBaseVA + 0x5000;
  llvm::LLVMContext Context;
  auto Module = makeRequiredModule(Context);
  auto *Type = llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto *Second = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, "g", Module.get());
  llvm::IRBuilder<> Builder(llvm::BasicBlock::Create(Context, "entry", Second));
  Builder.CreateRetVoid();
  Second->setUWTableKind(llvm::UWTableKind::Default);

  CompiledImage Image = compileImageForPatch(
      *Module, Arch::X64, BinaryFormat::MachO, TextVA,
      [](llvm::StringRef, uint32_t) -> std::optional<uint64_t> {
        return std::nullopt;
      },
      kBaseVA);

  ASSERT_TRUE(Image.Success);
  ASSERT_TRUE(Image.FunctionRangesValid);
  ASSERT_EQ(Image.FunctionRanges.size(), 2u);

  std::set<uint64_t> RangeIds;
  std::set<std::string> Owners;
  std::set<std::string> Endpoints;
  for (const CompiledFunctionRange &Range : Image.FunctionRanges) {
    EXPECT_NE(Range.Id, 0u);
    EXPECT_TRUE(RangeIds.insert(Range.Id).second);
    EXPECT_TRUE(Owners.insert(Range.OwnerSymbol).second);
    const auto Symbol = Image.SymbolAddrs.find(Range.OwnerSymbol);
    ASSERT_NE(Symbol, Image.SymbolAddrs.end());
    EXPECT_EQ(Range.OwnerVA, Symbol->second);
    EXPECT_FALSE(Range.BeginSymbol.empty());
    EXPECT_FALSE(Range.EndSymbol.empty());
    EXPECT_TRUE(Endpoints.insert(Range.BeginSymbol).second);
    EXPECT_TRUE(Endpoints.insert(Range.EndSymbol).second);
    EXPECT_EQ(Image.SymbolAddrs.count(Range.BeginSymbol), 0u);
    EXPECT_EQ(Image.SymbolAddrs.count(Range.EndSymbol), 0u);
    EXPECT_LT(Range.BeginVA, Range.EndVA);
  }
  EXPECT_EQ(Owners, (std::set<std::string>{"_f", "_g"}));

  // Only the two program symbols are public; four private CFI endpoints were
  // resolved through the provenance channel instead of SymbolAddrs.
  EXPECT_EQ(Image.SymbolAddrs.size(), 2u);
}

TEST(MachOExceptionPatch, FindsAnExactNonoverlappingEHFrameRegion) {
  const std::vector<uint8_t> Binary = makeMachO64();
  auto Region = findMachOEHFrameRegion(Binary);
  ASSERT_TRUE(Region.has_value());
  EXPECT_TRUE(Region->Is64);
  EXPECT_EQ(Region->SectionVA, kBaseVA + kEHFrameOff);
  EXPECT_EQ(Region->SectionFileOff, kEHFrameOff);
  EXPECT_EQ(Region->AppendFileOff, kEHFrameOff);
  EXPECT_EQ(Region->LimitFileOff, kTextOff);
}

TEST(MachOExceptionPatch,
     FixedEHFrameVariableExpressionsTargetEveryGeneratedFunction) {
  constexpr uint64_t TextVA = kBaseVA + 0x1000;
  // Keep the frame after the generated text so the FDE's anonymous
  // `function - field` variable exercises a real negative Darwin delta.
  constexpr uint64_t EHFrameVA = kBaseVA + 0x3000;
  llvm::LLVMContext Context;
  auto Module = makeRequiredModule(Context);
  auto *Type = llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto *Second = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, "g", Module.get());
  llvm::IRBuilder<> Builder(llvm::BasicBlock::Create(Context, "entry", Second));
  Builder.CreateRetVoid();
  Second->setUWTableKind(llvm::UWTableKind::Default);
  CompiledImage Image = compileImageForPatchWithFixedSectionVAs(
      *Module, Arch::X64, BinaryFormat::MachO, TextVA,
      [](llvm::StringRef, uint32_t) -> std::optional<uint64_t> {
        return std::nullopt;
      },
      [=](llvm::StringRef Name) -> std::optional<uint64_t> {
        if (Name == "__eh_frame")
          return EHFrameVA;
        return std::nullopt;
      });
  ASSERT_TRUE(Image.Success);

  const CompiledSection *EHFrame = nullptr;
  for (const CompiledSection &Section : Image.Sections)
    if (Section.Name == "__eh_frame")
      EHFrame = &Section;
  ASSERT_NE(EHFrame, nullptr);
  ASSERT_FALSE(EHFrame->IsInImage);

  auto FirstFunction = Image.SymbolAddrs.find("_f");
  auto SecondFunction = Image.SymbolAddrs.find("_g");
  ASSERT_NE(FirstFunction, Image.SymbolAddrs.end());
  ASSERT_NE(SecondFunction, Image.SymbolAddrs.end());
  ASSERT_GT(SecondFunction->second, FirstFunction->second);
  auto Records =
      decodeDwarfEHFrameRecords(EHFrame->ExternalBytes, EHFrame->VA, true);
  ASSERT_TRUE(static_cast<bool>(Records))
      << llvm::toString(Records.takeError());
  ASSERT_EQ(Records->size(), 2u);
  EXPECT_EQ((*Records)[0].BeginVA, FirstFunction->second);
  EXPECT_EQ((*Records)[1].BeginVA, SecondFunction->second);
}

TEST(MachOExceptionPatch, AArch64FixedEHFrameCoversEveryGeneratedFunction) {
  constexpr uint64_t TextVA = kBaseVA + 0x1000;
  constexpr uint64_t EHFrameVA = kBaseVA + 0x3000;
  llvm::LLVMContext Context;
  auto Module = makeRequiredModule(Context);
  auto *Type = llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto *Second = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, "g", Module.get());
  llvm::IRBuilder<> Builder(llvm::BasicBlock::Create(Context, "entry", Second));
  Builder.CreateRetVoid();
  Second->setUWTableKind(llvm::UWTableKind::Default);
  CompiledImage Image = compileImageForPatchWithFixedSectionVAs(
      *Module, Arch::AArch64, BinaryFormat::MachO, TextVA,
      [](llvm::StringRef, uint32_t) -> std::optional<uint64_t> {
        return std::nullopt;
      },
      [=](llvm::StringRef Name) -> std::optional<uint64_t> {
        if (Name == "__eh_frame")
          return EHFrameVA;
        return std::nullopt;
      });
  ASSERT_TRUE(Image.Success);

  const CompiledSection *EHFrame = nullptr;
  for (const CompiledSection &Section : Image.Sections)
    if (Section.Name == "__eh_frame")
      EHFrame = &Section;
  ASSERT_NE(EHFrame, nullptr);
  ASSERT_FALSE(EHFrame->IsInImage);

  auto FirstFunction = Image.SymbolAddrs.find("_f");
  auto SecondFunction = Image.SymbolAddrs.find("_g");
  ASSERT_NE(FirstFunction, Image.SymbolAddrs.end());
  ASSERT_NE(SecondFunction, Image.SymbolAddrs.end());
  ASSERT_GT(SecondFunction->second, FirstFunction->second);
  auto Records =
      decodeDwarfEHFrameRecords(EHFrame->ExternalBytes, EHFrame->VA, true);
  ASSERT_TRUE(static_cast<bool>(Records))
      << llvm::toString(Records.takeError());
  ASSERT_EQ(Records->size(), 2u);
  EXPECT_EQ((*Records)[0].BeginVA, FirstFunction->second);
  EXPECT_EQ((*Records)[1].BeginVA, SecondFunction->second);
}

TEST(MachOExceptionPatch, RejectsOverlappingFileBackedSections) {
  // __text starts in the declared __eh_frame range.  Even though the logical
  // append point is its leading zero terminator, those bytes have two owners
  // and cannot be rewritten safely.
  const std::vector<uint8_t> Binary = makeMachO64(kEHFrameOff + 8);
  EXPECT_FALSE(findMachOEHFrameRegion(Binary).has_value());
}

TEST(MachOExceptionPatch, RejectsNonExactSegmentCommandSize) {
  std::vector<uint8_t> Binary = makeMachO64();
  auto *Segment = reinterpret_cast<segment_command_64 *>(
      Binary.data() + sizeof(mach_header_64));
  Segment->cmdsize += 8;
  reinterpret_cast<mach_header_64 *>(Binary.data())->sizeofcmds += 8;
  EXPECT_FALSE(findMachOEHFrameRegion(Binary).has_value());
}

TEST(MachOExceptionPatch, AcceptsARMImageWithoutUnwindAsNoOp) {
  llvm::LLVMContext Context;
  llvm::Module Module("arm-no-unwind", Context);
  CompiledImage Compiled;
  Compiled.Success = true;
  Compiled.Format = BinaryFormat::MachO;
  Compiled.TargetArch = Arch::ARM;
  Compiled.PointerWidth = 4;
  Compiled.ByteOrder = llvm::endianness::little;
  Compiled.FunctionRangesValid = true;
  std::vector<uint8_t> Binary = {0x41, 0x52, 0x4d};
  const std::vector<uint8_t> Before = Binary;

  auto Receipt = installMachOEHFrameWithReceipt(
      Binary, std::nullopt, Compiled, Module, nullptr);
  ASSERT_TRUE(static_cast<bool>(Receipt))
      << llvm::toString(Receipt.takeError());
  EXPECT_EQ(Receipt->disposition(), MachOEHFrameInstallDisposition::Unchanged);
  EXPECT_EQ(Binary, Before);
}

TEST(MachOExceptionPatch,
     AcceptsVerifiedARMCompactCoverageWithoutInstallingAnFDE) {
  constexpr uint64_t FunctionVA = 0x1000;
  llvm::LLVMContext Context;
  CompiledImage Generated;
  addGeneratedFunctionProvenance(Generated, FunctionVA);
  Generated.TargetArch = Arch::ARM;
  Generated.PointerWidth = 4;

  MachOCompactUnwindRecords Compact;
  Compact.TargetArch = Arch::ARM;
  Compact.PointerWidth = 4;
  Compact.ByteOrder = llvm::endianness::little;
  Compact.Records.push_back({1,
                             "_f",
                             FunctionVA,
                             FunctionVA,
                             FunctionVA + 0x10,
                             0x10,
                             macho_unwind::kARMModeFrame,
                             "L_begin_f"});

  std::vector<uint8_t> Binary = {0x41, 0x52, 0x4d};
  const std::vector<uint8_t> Before = Binary;
  auto Receipt = installMachOEHFrameWithReceipt(
      Binary, std::nullopt, Generated, *makeRequiredModule(Context), &Compact);
  ASSERT_TRUE(static_cast<bool>(Receipt))
      << llvm::toString(Receipt.takeError());
  EXPECT_EQ(Receipt->disposition(), MachOEHFrameInstallDisposition::Unchanged);
  EXPECT_TRUE(Receipt->installedFDEs().empty());
  EXPECT_EQ(Binary, Before);
}

TEST(MachOExceptionPatch, AppendsOnlyAfterSemanticInputValidation) {
  constexpr uint64_t FunctionVA = kBaseVA + 0x800;
  llvm::LLVMContext Context;
  std::vector<uint8_t> Binary = makeMachO64();
  auto Region = findMachOEHFrameRegion(Binary);
  ASSERT_TRUE(Region.has_value());
  CompiledImage Generated = makeGeneratedEHFrame(*Region, FunctionVA);

  auto Receipt = installMachOEHFrameWithReceipt(Binary, Region, Generated,
                                                *makeRequiredModule(Context));
  ASSERT_TRUE(static_cast<bool>(Receipt))
      << llvm::toString(Receipt.takeError());
  EXPECT_EQ(Receipt->disposition(), MachOEHFrameInstallDisposition::Installed);
  ASSERT_TRUE(Receipt->region().has_value());
  EXPECT_EQ(Receipt->installedFileOff(), Region->AppendFileOff);
  EXPECT_EQ(Receipt->installedBytes(),
            llvm::ArrayRef<uint8_t>(Generated.Sections[0].ExternalBytes));
  ASSERT_EQ(Receipt->installedFDEs().size(), 1u);
  EXPECT_EQ(Receipt->installedFDEs().front().BeginVA, FunctionVA);
  EXPECT_EQ(Receipt->installedFDEs().front().EndVA, FunctionVA + 0x10);
  EXPECT_EQ(Receipt->installedSymbolAddrs().at("f"), FunctionVA);
  auto Updated = findMachOEHFrameRegion(Binary);
  ASSERT_TRUE(Updated.has_value());
  EXPECT_GT(Updated->AppendFileOff, Region->AppendFileOff);
}

TEST(MachOExceptionPatch,
     RequiresAnExactGeneratedFDEForEveryAuthenticatedFragment) {
  constexpr uint64_t FunctionVA = kBaseVA + 0x800;
  llvm::LLVMContext Context;

  auto Reject = [&](CompiledImage Generated) {
    std::vector<uint8_t> Binary = makeMachO64();
    const std::vector<uint8_t> Before = Binary;
    auto Region = findMachOEHFrameRegion(Binary);
    ASSERT_TRUE(Region.has_value());
    auto Receipt = installMachOEHFrameWithReceipt(
        Binary, Region, Generated, *makeRequiredModule(Context));
    EXPECT_FALSE(static_cast<bool>(Receipt));
    if (!Receipt)
      llvm::consumeError(Receipt.takeError());
    EXPECT_EQ(Binary, Before);
  };

  std::vector<uint8_t> Binary = makeMachO64();
  auto Region = findMachOEHFrameRegion(Binary);
  ASSERT_TRUE(Region.has_value());

  CompiledImage WrongEnd = makeGeneratedEHFrame(*Region, FunctionVA);
  WrongEnd.Sections[1].Size = 0x11;
  WrongEnd.FunctionRanges[0].EndVA = FunctionVA + 0x11;
  Reject(std::move(WrongEnd));

  CompiledImage MissingFragment =
      makeGeneratedEHFrame(*Region, FunctionVA);
  MissingFragment.Sections[1].Size = 0x20;
  MissingFragment.FunctionRanges.push_back(
      {2, "_f", FunctionVA, "L_begin_f_1", FunctionVA + 0x10,
       "L_end_f_1", FunctionVA + 0x20});
  Reject(std::move(MissingFragment));

  CompiledImage ExtraFDE = makeGeneratedEHFrame(*Region, FunctionVA);
  std::vector<uint8_t> &Frame = ExtraFDE.Sections[0].ExternalBytes;
  ASSERT_GE(Frame.size(), sizeof(uint32_t));
  Frame.resize(Frame.size() - sizeof(uint32_t));
  const std::vector<uint8_t> Unowned =
      makeEHFrameFragment(FunctionVA + 0x100);
  Frame.insert(Frame.end(), Unowned.begin(), Unowned.end());
  ExtraFDE.Sections[0].Size = Frame.size();
  Reject(std::move(ExtraFDE));
}

TEST(MachOExceptionPatch,
     AcceptsVerifiedNonDwarfCompactCoverageWithoutInstallingAnFDE) {
  constexpr uint64_t FunctionVA = kBaseVA + 0x800;
  llvm::LLVMContext Context;
  std::vector<uint8_t> Binary = makeMachO64();
  const std::vector<uint8_t> Before = Binary;
  auto Region = findMachOEHFrameRegion(Binary);
  ASSERT_TRUE(Region.has_value());

  CompiledImage Generated;
  addGeneratedFunctionProvenance(Generated, FunctionVA);
  MachOCompactUnwindRecords Compact;
  Compact.TargetArch = Arch::AArch64;
  Compact.PointerWidth = 8;
  Compact.ByteOrder = llvm::endianness::little;
  MachOCompactUnwindRecord Record;
  Record.FunctionVA = FunctionVA;
  Record.FunctionEndVA = FunctionVA + 0x10;
  Record.RangeLength = 0x10;
  Record.Encoding = macho_unwind::kARM64ModeFrame;
  Record.FunctionRangeId = 1;
  Record.OwnerSymbol = "_f";
  Record.OwnerVA = FunctionVA;
  Record.FunctionSymbol = "L_begin_f";
  Compact.Records.push_back(Record);

  auto Receipt = installMachOEHFrameWithReceipt(
      Binary, Region, Generated, *makeRequiredModule(Context), &Compact);
  ASSERT_TRUE(static_cast<bool>(Receipt))
      << llvm::toString(Receipt.takeError());
  EXPECT_EQ(Receipt->disposition(), MachOEHFrameInstallDisposition::Unchanged);
  EXPECT_TRUE(Receipt->installedFDEs().empty());
  EXPECT_EQ(Binary, Before);
}

TEST(MachOExceptionPatch,
     RejectsUnauthenticatedCompactCoverageWithoutChangingBytes) {
  constexpr uint64_t FunctionVA = kBaseVA + 0x800;
  llvm::LLVMContext Context;

  CompiledImage Generated;
  addGeneratedFunctionProvenance(Generated, FunctionVA);
  MachOCompactUnwindRecords Valid;
  Valid.TargetArch = Arch::AArch64;
  Valid.PointerWidth = 8;
  Valid.ByteOrder = llvm::endianness::little;
  Valid.Records.push_back({1,
                           "_f",
                           FunctionVA,
                           FunctionVA,
                           FunctionVA + 0x10,
                           0x10,
                           macho_unwind::kARM64ModeFrame,
                           "L_begin_f"});

  auto Reject = [&](const CompiledImage &Candidate,
                    const MachOCompactUnwindRecords &Coverage) {
    std::vector<uint8_t> Binary = makeMachO64();
    const std::vector<uint8_t> Before = Binary;
    auto Region = findMachOEHFrameRegion(Binary);
    ASSERT_TRUE(Region.has_value());
    auto Receipt = installMachOEHFrameWithReceipt(
        Binary, Region, Candidate, *makeRequiredModule(Context), &Coverage);
    EXPECT_FALSE(static_cast<bool>(Receipt));
    if (!Receipt)
      llvm::consumeError(Receipt.takeError());
    EXPECT_EQ(Binary, Before);
  };

  MachOCompactUnwindRecords MissingId = Valid;
  MissingId.Records[0].FunctionRangeId = 0;
  Reject(Generated, MissingId);

  MachOCompactUnwindRecords DanglingId = Valid;
  DanglingId.Records[0].FunctionRangeId = 99;
  Reject(Generated, DanglingId);

  MachOCompactUnwindRecords WrongOwner = Valid;
  WrongOwner.Records[0].OwnerSymbol = "_stale";
  Reject(Generated, WrongOwner);

  MachOCompactUnwindRecords WrongBounds = Valid;
  ++WrongBounds.Records[0].FunctionEndVA;
  Reject(Generated, WrongBounds);

  MachOCompactUnwindRecords WrongSymbol = Valid;
  WrongSymbol.Records[0].FunctionSymbol = "L_stale";
  Reject(Generated, WrongSymbol);

  MachOCompactUnwindRecords DuplicateId = Valid;
  DuplicateId.Records.push_back(DuplicateId.Records.front());
  Reject(Generated, DuplicateId);

  MachOCompactUnwindRecords WrongArch = Valid;
  WrongArch.TargetArch = Arch::X64;
  Reject(Generated, WrongArch);

  MachOCompactUnwindRecords WrongPointerWidth = Valid;
  WrongPointerWidth.PointerWidth = 4;
  Reject(Generated, WrongPointerWidth);

  MachOCompactUnwindRecords WrongByteOrder = Valid;
  WrongByteOrder.ByteOrder = llvm::endianness::big;
  Reject(Generated, WrongByteOrder);

  for (uint32_t ForgedEncoding :
       {0u, 0x05000000u,
        macho_unwind::kARM64ModeFrame | 0x00800000u,
        macho_unwind::kX86_64ModeRBPFrame}) {
    SCOPED_TRACE(ForgedEncoding);
    MachOCompactUnwindRecords Forged = Valid;
    Forged.Records[0].Encoding = ForgedEncoding;
    Reject(Generated, Forged);
  }
}

TEST(MachOExceptionPatch,
     DwarfCompactCoverageStillRequiresAnInstalledFDEAndRollsBack) {
  constexpr uint64_t FunctionVA = kBaseVA + 0x800;
  llvm::LLVMContext Context;
  std::vector<uint8_t> Binary = makeMachO64();
  const std::vector<uint8_t> Before = Binary;
  auto Region = findMachOEHFrameRegion(Binary);
  ASSERT_TRUE(Region.has_value());

  CompiledImage Generated;
  addGeneratedFunctionProvenance(Generated, FunctionVA);
  MachOCompactUnwindRecords Compact;
  Compact.TargetArch = Arch::AArch64;
  Compact.PointerWidth = 8;
  Compact.ByteOrder = llvm::endianness::little;
  MachOCompactUnwindRecord Record;
  Record.FunctionVA = FunctionVA;
  Record.FunctionEndVA = FunctionVA + 0x10;
  Record.RangeLength = 0x10;
  Record.Encoding = macho_unwind::kARM64ModeDwarf;
  Record.FunctionRangeId = 1;
  Record.OwnerSymbol = "_f";
  Record.OwnerVA = FunctionVA;
  Record.FunctionSymbol = "L_begin_f";
  Compact.Records.push_back(Record);

  auto Receipt = installMachOEHFrameWithReceipt(
      Binary, Region, Generated, *makeRequiredModule(Context), &Compact);
  EXPECT_FALSE(static_cast<bool>(Receipt));
  llvm::consumeError(Receipt.takeError());
  EXPECT_EQ(Binary, Before);
}

TEST(MachOExceptionPatch, RejectsForgedRegionWithoutChangingBytes) {
  constexpr uint64_t FunctionVA = kBaseVA + 0x800;
  llvm::LLVMContext Context;
  std::vector<uint8_t> Binary = makeMachO64();
  const std::vector<uint8_t> Before = Binary;
  auto Region = findMachOEHFrameRegion(Binary);
  ASSERT_TRUE(Region.has_value());
  CompiledImage Generated = makeGeneratedEHFrame(*Region, FunctionVA);

  ++Region->LimitFileOff;
  EXPECT_TRUE(hadError(installMachOEHFrame(Binary, Region, Generated,
                                           *makeRequiredModule(Context))));
  EXPECT_EQ(Binary, Before);
}

TEST(MachOExceptionPatch, RejectsInsufficientTailCapacityWithoutChangingBytes) {
  constexpr uint64_t FunctionVA = kBaseVA + 0x800;
  llvm::LLVMContext Context;
  std::vector<uint8_t> Binary = makeMachO64(kEHFrameOff + 20);
  const std::vector<uint8_t> Before = Binary;
  auto Region = findMachOEHFrameRegion(Binary);
  ASSERT_TRUE(Region.has_value());
  CompiledImage Generated = makeGeneratedEHFrame(*Region, FunctionVA);
  ASSERT_GT(Generated.Sections[0].ExternalBytes.size(),
            Region->LimitFileOff - Region->AppendFileOff);

  EXPECT_TRUE(hadError(installMachOEHFrame(Binary, Region, Generated,
                                           *makeRequiredModule(Context))));
  EXPECT_EQ(Binary, Before);
}

TEST(MachOExceptionPatch, RejectsMalformedInputEHFrameWithoutChangingBytes) {
  constexpr uint64_t FunctionVA = kBaseVA + 0x800;
  llvm::LLVMContext Context;
  std::vector<uint8_t> Binary = makeMachO64();
  // Framing alone accepts this five-byte CIE payload, but version 0xff has no
  // DWARF meaning and must fail the same semantic decoder used for output.
  putU32(Binary, kEHFrameOff, 5);
  putU32(Binary, kEHFrameOff + 4, 0);
  Binary[kEHFrameOff + 8] = 0xff;
  const std::vector<uint8_t> Before = Binary;
  auto Region = findMachOEHFrameRegion(Binary);
  ASSERT_TRUE(Region.has_value());
  CompiledImage Generated = makeGeneratedEHFrame(*Region, FunctionVA);

  EXPECT_TRUE(hadError(installMachOEHFrame(Binary, Region, Generated,
                                           *makeRequiredModule(Context))));
  EXPECT_EQ(Binary, Before);
}

TEST(MachOExceptionPatch,
     RejectsOverlappingInputAndGeneratedFDEsWithoutChangingBytes) {
  constexpr uint64_t FunctionVA = kBaseVA + 0x800;
  llvm::LLVMContext Context;
  std::vector<uint8_t> Binary = makeMachO64();
  setExistingEHFrame(Binary, makeEHFrameFragment(FunctionVA));
  const std::vector<uint8_t> Before = Binary;
  auto Region = findMachOEHFrameRegion(Binary);
  ASSERT_TRUE(Region.has_value());
  CompiledImage Generated = makeGeneratedEHFrame(*Region, FunctionVA);

  EXPECT_TRUE(hadError(installMachOEHFrame(Binary, Region, Generated,
                                           *makeRequiredModule(Context))));
  EXPECT_EQ(Binary, Before);
}

TEST(MachOExceptionPatch, AcceptsAdjacentHalfOpenFDERanges) {
  constexpr uint64_t ExistingFunctionVA = kBaseVA + 0x800;
  constexpr uint64_t GeneratedFunctionVA = ExistingFunctionVA + 0x10;
  llvm::LLVMContext Context;
  std::vector<uint8_t> Binary = makeMachO64();
  setExistingEHFrame(Binary, makeEHFrameFragment(ExistingFunctionVA));
  auto Region = findMachOEHFrameRegion(Binary);
  ASSERT_TRUE(Region.has_value());
  CompiledImage Generated = makeGeneratedEHFrame(*Region, GeneratedFunctionVA);

  EXPECT_FALSE(hadError(installMachOEHFrame(Binary, Region, Generated,
                                            *makeRequiredModule(Context))));
}

} // namespace

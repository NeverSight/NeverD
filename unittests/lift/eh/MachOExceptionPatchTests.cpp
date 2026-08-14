//===- MachOExceptionPatchTests.cpp - Mach-O EH frame layout tests -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/DwarfEHFrame.h"
#include "neverd/backend/codegen/MachO/MachOExceptionPatch.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <cstring>
#include <memory>
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
  Image.Success = true;
  Image.Sections.push_back(std::move(Section));
  Image.SymbolAddrs["f"] = FunctionVA;
  return Image;
}

bool hadError(llvm::Error Error) {
  const bool Failed = static_cast<bool>(Error);
  llvm::consumeError(std::move(Error));
  return Failed;
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

TEST(MachOExceptionPatch, AppendsOnlyAfterSemanticInputValidation) {
  constexpr uint64_t FunctionVA = kBaseVA + 0x800;
  llvm::LLVMContext Context;
  std::vector<uint8_t> Binary = makeMachO64();
  auto Region = findMachOEHFrameRegion(Binary);
  ASSERT_TRUE(Region.has_value());
  CompiledImage Generated = makeGeneratedEHFrame(*Region, FunctionVA);

  EXPECT_FALSE(hadError(installMachOEHFrame(Binary, Region, Generated,
                                            *makeRequiredModule(Context))));
  auto Updated = findMachOEHFrameRegion(Binary);
  ASSERT_TRUE(Updated.has_value());
  EXPECT_GT(Updated->AppendFileOff, Region->AppendFileOff);
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

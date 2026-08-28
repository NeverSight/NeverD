//===- LLVMImportVeneerBoundaryTests.cpp - Import veneer identity --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/object/SectionNames.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace neverd;

constexpr va_t TextVA = 0x140001000;
constexpr va_t CallerVA = TextVA;
constexpr va_t LocalVA = TextVA + 0x40;
constexpr va_t StubVA = TextVA + 0x100;
constexpr va_t DataVA = 0x140003000;
constexpr va_t IATVA = DataVA + 0x10;

BinaryImage makeCodePointerImage() {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Format = BinaryFormat::COFF;
  Image.Bits = Bitness::Bits64;
  Image.Base = 0x140000000;

  Segment Text;
  Text.Name = section_names::coff::Text;
  Text.VA = TextVA;
  Text.Size = 0x200;
  Text.FileSz = Text.Size;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  Image.Segments.push_back(std::move(Text));

  Segment Data;
  Data.Name = section_names::coff::Rdata;
  Data.VA = DataVA;
  Data.Size = 0x20;
  Data.FileSz = Data.Size;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(Data.Size);
  std::memcpy(Data.Data.data(), &StubVA, sizeof(StubVA));
  Image.Segments.push_back(std::move(Data));

  Image.CodePtrRelocSlots.insert(DataVA);
  return Image;
}

void addImport(BinaryImage &Image, llvm::StringRef Name, va_t IATAddress) {
  Import Imported;
  Imported.Module = "fixture.dll";
  Imported.Name = Name.str();
  Imported.IATAddr = IATAddress;
  Image.Imports.push_back(std::move(Imported));
}

std::vector<std::pair<va_t, std::string>>
importNames(const BinaryImage &Image) {
  const std::map<va_t, std::string> Names = Image.getImportAddressNames();
  return {Names.begin(), Names.end()};
}

MedFunc makeIndirectCaller() {
  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = "import_veneer_caller";
  Func.ReturnType = NdType::makeVoid();

  MedVar Slot;
  Slot.Kind = MedVar::Temp;
  Slot.TheArch = Arch::X64;
  Slot.Id = 1;
  Slot.SSAVer = 1;
  Slot.Size = 8;
  MedVar Target = Slot;
  Target.Id = 2;

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = CallerVA;
  Block.EndAddr = CallerVA + 0x10;

  MedOp Materialize;
  Materialize.Opcode = NdOp::COPY;
  Materialize.Addr = CallerVA;
  Materialize.Output = Slot;
  Materialize.addInput(
      MedVar::makeConst(DataVA, 8, ConstantAddressProvenance::Address));
  Block.Ops.push_back(std::move(Materialize));

  MedOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Addr = CallerVA + 4;
  Load.Output = Target;
  Load.addInput(Slot);
  Block.Ops.push_back(std::move(Load));

  MedOp Call;
  Call.Opcode = NdOp::INDIR_CALL;
  Call.Addr = CallerVA + 8;
  Call.addInput(Target);
  Block.Ops.push_back(std::move(Call));

  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = CallerVA + 12;
  Block.Ops.push_back(std::move(Return));
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

MedFunc makeLocalFunction(llvm::StringRef Name) {
  MedFunc Func;
  Func.Entry = LocalVA;
  Func.Name = Name.str();
  Func.ReturnType = NdType::makeVoid();

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = LocalVA;
  Block.EndAddr = LocalVA + 4;
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = LocalVA;
  Block.Ops.push_back(std::move(Return));
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

void expectValidModule(const llvm::Module &Module) {
  std::string Verification;
  llvm::raw_string_ostream OS(Verification);
  EXPECT_FALSE(llvm::verifyModule(Module, &OS)) << OS.str();
}

TEST(BinaryImageImportVeneerBoundary,
     ExactLookupDoesNotSpliceIATAndRangeEvidence) {
  BinaryImage Image = makeCodePointerImage();
  addImport(Image, "iat_collision", StubVA);
  addImport(Image, "exact_veneer", IATVA);
  ASSERT_TRUE(Image.recordImportStubRange(StubVA, 8));
  EXPECT_EQ(Image.findImportStubAt(StubVA), nullptr);

  ASSERT_TRUE(Image.recordImportStub(StubVA, 1));
  EXPECT_EQ(Image.findImportAt(StubVA), &Image.Imports[0]);
  EXPECT_EQ(Image.findImportStubAt(StubVA), &Image.Imports[1]);
}

TEST(LLVMImportVeneerBoundary,
     CodeRelocationUsesTheExactStubIdentityOverAnIATCollision) {
  BinaryImage Image = makeCodePointerImage();
  addImport(Image, "iat_collision", StubVA);
  addImport(Image, "exact_veneer", IATVA);
  ASSERT_TRUE(Image.recordImportStub(StubVA, 1));

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {makeIndirectCaller()}, Context, "exact-import-veneer", Arch::X64,
      importNames(Image), &Image, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  expectValidModule(*Module);
  EXPECT_NE(Module->getNamedValue("exact_veneer"), nullptr);
  EXPECT_EQ(Module->getNamedValue("iat_collision"), nullptr);
}

TEST(LLVMImportVeneerBoundary, RangeOnlyEvidenceCannotNameACodeRelocation) {
  BinaryImage Image = makeCodePointerImage();
  addImport(Image, "range_iat_collision", StubVA);
  ASSERT_TRUE(Image.recordImportStubRange(StubVA, 8));

  llvm::LLVMContext Context;
  testing::internal::CaptureStderr();
  auto Module = MedLLVMEmitter().emit(
      {makeIndirectCaller()}, Context, "range-only-import-veneer", Arch::X64,
      importNames(Image), &Image, BinaryFormat::COFF);
  const std::string Diagnostic = testing::internal::GetCapturedStderr();
  EXPECT_EQ(Module, nullptr);
  EXPECT_NE(Diagnostic.find("targets unresolved address"), std::string::npos)
      << Diagnostic;
}

TEST(LLVMImportVeneerBoundary,
     ImportedPointerCannotBindToASameNamedLiftedFunction) {
  BinaryImage Image = makeCodePointerImage();
  addImport(Image, "symbol_collision", IATVA);
  ASSERT_TRUE(Image.recordImportStub(StubVA, 0));

  llvm::LLVMContext Context;
  testing::internal::CaptureStderr();
  auto Module = MedLLVMEmitter().emit(
      {makeIndirectCaller(), makeLocalFunction("symbol_collision")}, Context,
      "import-local-name-collision", Arch::X64, importNames(Image), &Image,
      BinaryFormat::COFF);
  const std::string Diagnostic = testing::internal::GetCapturedStderr();
  EXPECT_EQ(Module, nullptr);
  EXPECT_NE(Diagnostic.find("collides with a lifted symbol"), std::string::npos)
      << Diagnostic;
}

} // namespace

//===- LLVMImportVeneerBoundaryTests.cpp - Import veneer identity --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/LLVMValueProvenance.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/object/SectionNames.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Instructions.h"
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

MedFunc makeLocalFunction(llvm::StringRef Name, va_t Entry = LocalVA) {
  MedFunc Func;
  Func.Entry = Entry;
  Func.Name = Name.str();
  Func.ReturnType = NdType::makeVoid();

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Entry;
  Block.EndAddr = Entry + 4;
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Entry;
  Block.Ops.push_back(std::move(Return));
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

/// A function at CallerVA calling \p Target directly by the symbol \p Name.
MedFunc makeDirectCaller(va_t Target, llvm::StringRef Name) {
  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = "direct_caller";
  Func.ReturnType = NdType::makeVoid();

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = CallerVA;
  Block.EndAddr = CallerVA + 8;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = CallerVA;
  Call.addInput(
      MedVar::makeConst(Target, 8, ConstantAddressProvenance::Address));
  Block.Ops.push_back(std::move(Call));
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = CallerVA + 5;
  Block.Ops.push_back(std::move(Return));
  Func.Blocks.push_back(std::move(Block));

  MedCallInfo Info;
  Info.BlockId = 0;
  Info.OpIdx = 0;
  Info.TargetAddr = Target;
  Info.TargetName = Name.str();
  Func.CallInfos.push_back(std::move(Info));
  return Func;
}

/// `jmp [Slot]` lifted at \p Entry: `RAX = call [Slot]; return RAX`.
MedFunc makeSlotThunk(va_t Entry, va_t Slot) {
  MedFunc Func;
  Func.Entry = Entry;
  Func.Name = "slot_thunk";
  Func.ReturnType = NdType::makeInt(8);

  MedVar Result;
  Result.Kind = MedVar::Reg;
  Result.TheArch = Arch::X64;
  Result.Id = 1;
  Result.SSAVer = 1;
  Result.Size = 8;
  Result.RegOff = getTargetRegInfo(Arch::X64).IntReturnReg;

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Entry;
  Block.EndAddr = Entry + 8;
  MedOp Call;
  Call.Opcode = NdOp::INDIR_CALL;
  Call.Addr = Entry;
  Call.Output = Result;
  Call.addInput(MedVar::makeConst(Slot, 8, ConstantAddressProvenance::Address));
  Block.Ops.push_back(std::move(Call));
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Entry + 6;
  Return.addInput(Result);
  Block.Ops.push_back(std::move(Return));
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

constexpr va_t RefptrVA = 0x140003000;
constexpr va_t DataImportSlotVA = 0x140004000;

/// MinGW's view of a data import: `.refptr.__imp__commode` in read-only data
/// holds the address of the import's slot in `.idata`, which the loader
/// binds to msvcrt's `_commode`.
BinaryImage makeRefptrImage() {
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

  Segment Rdata;
  Rdata.Name = section_names::coff::Rdata;
  Rdata.VA = RefptrVA;
  Rdata.Size = 0x10;
  Rdata.FileSz = Rdata.Size;
  Rdata.Flags = SegmentFlags::Readable;
  Rdata.Data.resize(Rdata.Size);
  std::memcpy(Rdata.Data.data(), &DataImportSlotVA, sizeof(DataImportSlotVA));
  Image.Segments.push_back(std::move(Rdata));

  Segment Idata;
  Idata.Name = ".idata";
  Idata.VA = DataImportSlotVA;
  Idata.Size = 0x10;
  Idata.FileSz = Idata.Size;
  Idata.Flags = SegmentFlags::Readable;
  Idata.Data.resize(Idata.Size);
  Image.Segments.push_back(std::move(Idata));

  addImport(Image, "_commode", DataImportSlotVA);
  EXPECT_TRUE(Image.recordImportStorageSlot(
      DataImportSlotVA, "_commode", 0, ImportStorageEvidence::ImportDirectory));
  return Image;
}

/// `__p__commode: mov rax, [rip + .refptr.__imp__commode]; mov rax, [rax];
/// ret`.
MedFunc makeRefptrReader() {
  MedFunc Func;
  Func.Entry = CallerVA;
  Func.Name = "__p__commode";
  Func.ReturnType = NdType::makeInt(8);
  auto temporary = [](int Id) {
    MedVar Var;
    Var.Kind = MedVar::Temp;
    Var.TheArch = Arch::X64;
    Var.Id = Id;
    Var.SSAVer = 1;
    Var.Size = 8;
    return Var;
  };
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = CallerVA;
  Block.EndAddr = CallerVA + 0xB;
  MedOp Slot;
  Slot.Opcode = NdOp::COPY;
  Slot.Addr = CallerVA;
  Slot.Output = temporary(1);
  Slot.addInput(
      MedVar::makeConst(RefptrVA, 8, ConstantAddressProvenance::Address));
  Block.Ops.push_back(std::move(Slot));
  MedOp Refptr;
  Refptr.Opcode = NdOp::LOAD;
  Refptr.Addr = CallerVA;
  Refptr.Output = temporary(2);
  Refptr.addInput(temporary(1));
  Block.Ops.push_back(std::move(Refptr));
  MedOp Import;
  Import.Opcode = NdOp::LOAD;
  Import.Addr = CallerVA + 7;
  Import.Output = temporary(3);
  Import.addInput(temporary(2));
  Block.Ops.push_back(std::move(Import));
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = CallerVA + 0xA;
  Return.addInput(temporary(3));
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
  // PE binds an import to the DLL that exports it, so a local function with
  // its name is another function: it takes its address as a suffix, and the
  // pointer to the import's stub is the import's.
  BinaryImage Image = makeCodePointerImage();
  addImport(Image, "symbol_collision", IATVA);
  ASSERT_TRUE(Image.recordImportStub(StubVA, 0));

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {makeIndirectCaller(), makeLocalFunction("symbol_collision")}, Context,
      "import-local-name-collision", Arch::X64, importNames(Image), &Image,
      BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  expectValidModule(*Module);
  const llvm::Function *Local =
      Module->getFunction("symbol_collision_" + llvm::utohexstr(LocalVA));
  ASSERT_NE(Local, nullptr);
  EXPECT_FALSE(Local->isDeclaration());
  EXPECT_TRUE(Local->use_empty());
  const llvm::GlobalValue *Imported = Module->getNamedValue("symbol_collision");
  ASSERT_NE(Imported, nullptr);
  EXPECT_TRUE(Imported->isDeclaration());
}

TEST(LLVMImportVeneerBoundary, ACallNamingAnImportReachesTheLocalFunction) {
  // MinGW's own atexit calls _crt_atexit; msvcrt.dll's atexit is another
  // function.  A call to the local one names it by its symbol, which spells
  // the import's name.
  BinaryImage Image = makeCodePointerImage();
  Image.CodePtrRelocSlots.clear();
  addImport(Image, "symbol_collision", IATVA);
  ASSERT_TRUE(Image.recordImportStorageSlot(
      IATVA, "symbol_collision", 0, ImportStorageEvidence::ImportDirectory));

  // A lifted function returns what it leaves in RAX.
  MedFunc Local = makeLocalFunction("symbol_collision");
  Local.ReturnType = NdType::makeInt(8);

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {makeDirectCaller(LocalVA, "symbol_collision"), std::move(Local)},
      Context, "import-local-call", Arch::X64, importNames(Image), &Image,
      BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  expectValidModule(*Module);
  const llvm::Function *Renamed =
      Module->getFunction("symbol_collision_" + llvm::utohexstr(LocalVA));
  ASSERT_NE(Renamed, nullptr);
  const llvm::Function *Caller = Module->getFunction("direct_caller");
  ASSERT_NE(Caller, nullptr);
  std::vector<const llvm::CallInst *> Calls;
  for (const llvm::BasicBlock &Block : *Caller)
    for (const llvm::Instruction &Instruction : Block)
      if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction))
        Calls.push_back(Call);
  ASSERT_EQ(Calls.size(), 1u);
  EXPECT_EQ(Calls.front()->getCalledFunction(), Renamed);
}

TEST(LLVMImportVeneerBoundary, ACallThroughASlotReturnsWhatTheImportDeclares) {
  // calloc returns its allocation; free returns nothing, so what it leaves
  // in RAX is no result of the thunk's.
  for (const auto &[Name, ReturnsValue] :
       {std::pair{"calloc", true}, std::pair{"free", false}}) {
    SCOPED_TRACE(Name);
    BinaryImage Image = makeCodePointerImage();
    Image.CodePtrRelocSlots.clear();
    addImport(Image, Name, IATVA);
    // The PE loader binds each import's slot of the address table.
    ASSERT_TRUE(Image.recordImportStorageSlot(
        IATVA, Name, 0, ImportStorageEvidence::ImportDirectory));
    MedFunc Source = makeSlotThunk(LocalVA, IATVA);
    MedCallInfo CI;
    CI.BlockId = CI.OpIdx = 0;
    CI.IsIndirect = true;
    CI.Args.assign(ReturnsValue ? 2 : 1,
                   MedVar::makeConst(0, 8, ConstantAddressProvenance::Scalar));
    Source.CallInfos.push_back(CI);
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Source}, Context, "slot-thunk", Arch::X64,
                              importNames(Image), &Image, BinaryFormat::COFF);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);
    const llvm::Function *Thunk = Module->getFunction("slot_thunk");
    ASSERT_NE(Thunk, nullptr);
    size_t Calls = 0;
    bool Produces = false;
    for (const llvm::BasicBlock &Block : *Thunk)
      for (const llvm::Instruction &Instruction : Block)
        if (llvm::isa<llvm::CallInst>(Instruction)) {
          ++Calls;
          Produces |= llvm_value_provenance::isSemanticProducer(Instruction);
        }
    EXPECT_EQ(Calls, 1u);
    EXPECT_EQ(Produces, ReturnsValue);
  }
}

TEST(LLVMImportVeneerBoundary, AStubLiftedUnderItsImportNameLeavesTheName) {
  // MinGW's `calloc: jmp [__imp_calloc]` lifted as a function named calloc:
  // the stub is not the import, so it takes its address as a suffix and the
  // import keeps its name for the pointer that refers to the stub.
  BinaryImage Image = makeCodePointerImage();
  addImport(Image, "symbol_collision", IATVA);
  ASSERT_TRUE(Image.recordImportStub(StubVA, 0));

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {makeIndirectCaller(), makeLocalFunction("symbol_collision", StubVA)},
      Context, "import-stub-name", Arch::X64, importNames(Image), &Image,
      BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  expectValidModule(*Module);
  const llvm::Function *Stub =
      Module->getFunction("symbol_collision_" + llvm::utohexstr(StubVA));
  ASSERT_NE(Stub, nullptr);
  EXPECT_FALSE(Stub->isDeclaration());
  const llvm::GlobalValue *Imported = Module->getNamedValue("symbol_collision");
  ASSERT_NE(Imported, nullptr);
  EXPECT_TRUE(Imported->isDeclaration());
}

TEST(LLVMImportVeneerBoundary,
     ADataImportReachedThroughARefptrSlotIsTheImport) {
  // The function returns what the loader bound to the import's slot, whether
  // or not the image's base relocations name the `.refptr` slot a pointer.
  for (const bool Relocated : {false, true}) {
    SCOPED_TRACE(Relocated);
    BinaryImage Image = makeRefptrImage();
    if (Relocated)
      Image.DataPtrRelocSlots.insert(RefptrVA);
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit({makeRefptrReader()}, Context, "refptr",
                                        Arch::X64, importNames(Image), &Image,
                                        BinaryFormat::COFF);
    ASSERT_NE(Module, nullptr);
    expectValidModule(*Module);
    std::string IR;
    llvm::raw_string_ostream OS(IR);
    Module->print(OS, nullptr);
    // The import's slot holds what the loader binds there.
    EXPECT_NE(IR.find("@__nd_codeptr_140004000 = internal constant <{ i64, "
                      "[8 x i8] }> <{ i64 ptrtoint (ptr @_commode to i64)"),
              std::string::npos)
        << IR;
    if (Relocated) {
      // The relocated `.refptr` slot is a pointer to the import's slot, and
      // the function reads through it as it stands.
      EXPECT_NE(IR.find("@__nd_codeptr_140003000 = internal constant <{ i64, "
                        "[8 x i8] }> <{ i64 ptrtoint (ptr "
                        "@__nd_codeptr_140004000 to i64)"),
                std::string::npos)
          << IR;
      EXPECT_NE(IR.find("load volatile i64, ptr @__nd_codeptr_140003000"),
                std::string::npos)
          << IR;
    } else {
      // The slot's bytes are the slot's address, which the function rebases
      // into the import table.
      EXPECT_NE(IR.find("getelementptr i8, ptr @__nd_codeptr_140004000"),
                std::string::npos)
          << IR;
    }
  }
}

} // namespace

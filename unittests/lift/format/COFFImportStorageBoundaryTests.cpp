//===- COFFImportStorageBoundaryTests.cpp - Exact IAT identity -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/object/SectionNames.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace neverd;

constexpr va_t ImageBase = 0x140000000;
constexpr va_t TextVA = ImageBase + 0x1000;
constexpr va_t DataVA = ImageBase + 0x3000;
constexpr va_t IATVA = DataVA + 0x10;
constexpr va_t RuntimeCallableVA = DataVA + 0x18;

BinaryImage makeImage() {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Format = BinaryFormat::COFF;
  Image.Bits = Bitness::Bits64;
  Image.Base = ImageBase;

  Segment Text;
  Text.Name = section_names::coff::Text;
  Text.VA = TextVA;
  Text.Size = 0x40;
  Text.FileSz = Text.Size;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  Image.Segments.push_back(std::move(Text));

  Segment Data;
  Data.Name = section_names::coff::Rdata;
  Data.VA = DataVA;
  Data.Size = 0x30;
  Data.FileSz = Data.Size;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(Data.Size);
  // An unbound PE IAT contains import lookup metadata on disk, not a callable
  // address.  Only the loader's exact-slot identity may give it call meaning.
  constexpr uint64_t HintNameRVA = 0x38bc;
  std::memcpy(Data.Data.data() + (IATVA - DataVA), &HintNameRVA,
              sizeof(HintNameRVA));
  Image.Segments.push_back(std::move(Data));
  return Image;
}

void addImport(BinaryImage &Image, va_t SlotVA, llvm::StringRef Name) {
  Import Imported;
  Imported.Module = "kernel32.dll";
  Imported.Name = Name.str();
  Imported.IATAddr = SlotVA;
  Image.Imports.push_back(std::move(Imported));
}

std::vector<std::pair<va_t, std::string>>
importNames(const BinaryImage &Image) {
  const std::map<va_t, std::string> Names = Image.getImportAddressNames();
  return {Names.begin(), Names.end()};
}

MedFunc makeIndirectCaller(va_t SlotVA) {
  MedFunc Func;
  Func.Entry = TextVA;
  Func.Name = "iat_indirect_caller";
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
  Block.StartAddr = TextVA;
  Block.EndAddr = TextVA + 0x10;

  MedOp Materialize;
  Materialize.Opcode = NdOp::COPY;
  Materialize.Addr = TextVA;
  Materialize.Output = Slot;
  Materialize.addInput(
      MedVar::makeConst(SlotVA, 8, ConstantAddressProvenance::Address));
  Block.Ops.push_back(std::move(Materialize));

  MedOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Addr = TextVA + 4;
  Load.Output = Target;
  Load.addInput(Slot);
  Block.Ops.push_back(std::move(Load));

  MedOp Call;
  Call.Opcode = NdOp::INDIR_CALL;
  Call.Addr = TextVA + 8;
  Call.addInput(Target);
  Block.Ops.push_back(std::move(Call));

  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = TextVA + 12;
  Block.Ops.push_back(std::move(Return));
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

MedFunc makeMemoryIndirectCaller(va_t SlotVA) {
  MedFunc Func;
  Func.Entry = TextVA;
  Func.Name = "iat_memory_indirect_caller";
  Func.ReturnType = NdType::makeVoid();

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = TextVA;
  Block.EndAddr = TextVA + 8;
  MedOp Call;
  Call.Opcode = NdOp::INDIR_CALL;
  Call.Addr = TextVA;
  Call.addInput(
      MedVar::makeConst(SlotVA, 8, ConstantAddressProvenance::Address));
  Block.Ops.push_back(std::move(Call));
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = TextVA + 4;
  Block.Ops.push_back(std::move(Return));
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

bool referencesValue(const llvm::Value *Root, const llvm::Value *Target,
                     std::set<const llvm::Value *> &Seen) {
  if (!Root || !Seen.insert(Root).second)
    return false;
  if (Root == Target)
    return true;
  // MedLLVMEmitter keeps temporary SSA values in promotable allocas until the
  // pipeline's canonical mem2reg step.  Follow their reaching stores here so
  // this boundary test observes the same semantic value flow without needing
  // to run a private pipeline phase.
  if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Root)) {
    const llvm::Value *Pointer = Load->getPointerOperand()->stripPointerCasts();
    if (const auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(Pointer))
      for (const llvm::User *User : Alloca->users())
        if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(User);
            Store && Store->getPointerOperand()->stripPointerCasts() == Alloca &&
            referencesValue(Store->getValueOperand(), Target, Seen))
          return true;
  }
  const auto *User = llvm::dyn_cast<llvm::User>(Root);
  if (!User)
    return false;
  for (const llvm::Use &Operand : User->operands())
    if (referencesValue(Operand.get(), Target, Seen))
      return true;
  return false;
}

bool loadsFromGlobal(const llvm::Value *Root,
                     const llvm::GlobalVariable *Target,
                     std::set<const llvm::Value *> &Seen) {
  if (!Root || !Seen.insert(Root).second)
    return false;
  if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Root)) {
    std::set<const llvm::Value *> PointerSeen;
    if (referencesValue(Load->getPointerOperand(), Target, PointerSeen))
      return true;
  }
  const auto *User = llvm::dyn_cast<llvm::User>(Root);
  if (!User)
    return false;
  for (const llvm::Use &Operand : User->operands())
    if (loadsFromGlobal(Operand.get(), Target, Seen))
      return true;
  return false;
}

llvm::CallInst *onlyCall(llvm::Function &Function) {
  llvm::CallInst *Result = nullptr;
  for (llvm::BasicBlock &Block : Function)
    for (llvm::Instruction &Instruction : Block)
      if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction)) {
        if (Result)
          return nullptr;
        Result = Call;
      }
  return Result;
}

void expectValidModule(const llvm::Module &Module) {
  std::string Verification;
  llvm::raw_string_ostream OS(Verification);
  EXPECT_FALSE(llvm::verifyModule(Module, &OS)) << OS.str();
}

TEST(COFFImportStorageBoundary, ExactIATSlotFeedsThePointerMirrorAndCall) {
  BinaryImage Image = makeImage();
  addImport(Image, IATVA, "RaiseException");
  ASSERT_TRUE(Image.recordImportStorageSlot(
      IATVA, "RaiseException", 0,
      ImportStorageEvidence::ImportDirectory));

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {makeIndirectCaller(IATVA)}, Context, "coff-exact-iat", Arch::X64,
      importNames(Image), &Image, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  expectValidModule(*Module);

  llvm::GlobalVariable *Mirror = Module->getNamedGlobal(
      (kNdCodePtrPrefix + llvm::utohexstr(DataVA)).str());
  ASSERT_NE(Mirror, nullptr);
  ASSERT_TRUE(Mirror->hasInitializer());
  llvm::GlobalValue *Imported = Module->getNamedValue("RaiseException");
  ASSERT_NE(Imported, nullptr);
  std::set<const llvm::Value *> InitializerSeen;
  EXPECT_TRUE(referencesValue(Mirror->getInitializer(), Imported,
                              InitializerSeen));

  llvm::Function *Caller = Module->getFunction("iat_indirect_caller");
  ASSERT_NE(Caller, nullptr);
  llvm::CallInst *Call = onlyCall(*Caller);
  ASSERT_NE(Call, nullptr);
  std::set<const llvm::Value *> CallSeen;
  EXPECT_TRUE(referencesValue(Call->getCalledOperand(), Mirror, CallSeen));
}

TEST(COFFImportStorageBoundary, MemoryIndirectIATCallLoadsTheSlotContents) {
  BinaryImage Image = makeImage();
  addImport(Image, IATVA, "RaiseException");
  ASSERT_TRUE(Image.recordImportStorageSlot(
      IATVA, "RaiseException", 0,
      ImportStorageEvidence::ImportDirectory));

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {makeMemoryIndirectCaller(IATVA)}, Context, "coff-memory-indirect-iat",
      Arch::X64, importNames(Image), &Image, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  expectValidModule(*Module);

  llvm::GlobalVariable *Mirror = Module->getNamedGlobal(
      (kNdCodePtrPrefix + llvm::utohexstr(DataVA)).str());
  ASSERT_NE(Mirror, nullptr);
  llvm::Function *Caller =
      Module->getFunction("iat_memory_indirect_caller");
  ASSERT_NE(Caller, nullptr);
  llvm::CallInst *Call = onlyCall(*Caller);
  ASSERT_NE(Call, nullptr);
  std::set<const llvm::Value *> Seen;
  EXPECT_TRUE(loadsFromGlobal(Call->getCalledOperand(), Mirror, Seen))
      << "memory-indirect call used the slot address instead of its contents";
}

TEST(COFFImportStorageBoundary,
     RuntimeCallableMemoryIndirectCallLoadsTheExactSlot) {
  BinaryImage Image = makeImage();
  ASSERT_TRUE(Image.recordRuntimeCallablePointerSlot(
      RuntimeCallableVA, RuntimeCallablePointerSlotKind::GuardCFDispatch));

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {makeMemoryIndirectCaller(RuntimeCallableVA)}, Context,
      "coff-runtime-callable-slot", Arch::X64, importNames(Image), &Image,
      BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  expectValidModule(*Module);

  llvm::GlobalVariable *Mirror = Module->getNamedGlobal(
      (kNdCodePtrPrefix + llvm::utohexstr(DataVA)).str());
  ASSERT_NE(Mirror, nullptr);
  llvm::Function *Caller =
      Module->getFunction("iat_memory_indirect_caller");
  ASSERT_NE(Caller, nullptr);
  llvm::CallInst *Call = onlyCall(*Caller);
  ASSERT_NE(Call, nullptr);
  std::set<const llvm::Value *> Seen;
  EXPECT_TRUE(loadsFromGlobal(Call->getCalledOperand(), Mirror, Seen))
      << "runtime-owned call used the slot address instead of its contents";
}

TEST(COFFImportStorageBoundary, NeighboringIATCannotNameAnUnregisteredSlot) {
  BinaryImage Image = makeImage();
  addImport(Image, IATVA + 8, "RaiseException");
  ASSERT_TRUE(Image.recordImportStorageSlot(
      IATVA + 8, "RaiseException", 0,
      ImportStorageEvidence::ImportDirectory));

  llvm::LLVMContext Context;
  testing::internal::CaptureStderr();
  auto Module = MedLLVMEmitter().emit(
      {makeIndirectCaller(IATVA)}, Context, "coff-neighbor-iat", Arch::X64,
      importNames(Image), &Image, BinaryFormat::COFF);
  const std::string Diagnostic = testing::internal::GetCapturedStderr();
  EXPECT_EQ(Module, nullptr);
  EXPECT_NE(Diagnostic.find("has no complete code-slot role"),
            std::string::npos)
      << Diagnostic;
}

TEST(COFFImportStorageBoundary,
     MemoryIndirectCallCannotBorrowANeighboringIATIdentity) {
  BinaryImage Image = makeImage();
  addImport(Image, IATVA + 8, "RaiseException");
  ASSERT_TRUE(Image.recordImportStorageSlot(
      IATVA + 8, "RaiseException", 0,
      ImportStorageEvidence::ImportDirectory));

  llvm::LLVMContext Context;
  testing::internal::CaptureStderr();
  auto Module = MedLLVMEmitter().emit(
      {makeMemoryIndirectCaller(IATVA)}, Context,
      "coff-memory-indirect-neighbor-iat", Arch::X64, importNames(Image),
      &Image, BinaryFormat::COFF);
  const std::string Diagnostic = testing::internal::GetCapturedStderr();
  EXPECT_EQ(Module, nullptr);
  EXPECT_NE(Diagnostic.find("has no exact callable slot identity"),
            std::string::npos)
      << Diagnostic;
}

TEST(COFFImportStorageBoundary, ConflictingExactIATIdentityFailsClosed) {
  BinaryImage Image = makeImage();
  addImport(Image, IATVA, "RaiseException");
  ASSERT_TRUE(Image.recordImportStorageSlot(
      IATVA, "RaiseException", 0,
      ImportStorageEvidence::ImportDirectory));
  ASSERT_FALSE(Image.recordImportStorageSlot(
      IATVA, "DifferentImport", 0,
      ImportStorageEvidence::ImportDirectory));

  llvm::LLVMContext Context;
  testing::internal::CaptureStderr();
  auto Module = MedLLVMEmitter().emit(
      {makeIndirectCaller(IATVA)}, Context, "coff-conflicting-iat", Arch::X64,
      importNames(Image), &Image, BinaryFormat::COFF);
  const std::string Diagnostic = testing::internal::GetCapturedStderr();
  EXPECT_EQ(Module, nullptr);
  EXPECT_NE(Diagnostic.find("has conflicting exact identities"),
            std::string::npos)
      << Diagnostic;
}

} // namespace

//===- MedLLVMDirectDataTests.cpp - Finite image memory accesses ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Direct byte accesses must not depend on unrelated relocations, and must
/// retain shared storage when a later access needs the complete pointer run.
///
//===----------------------------------------------------------------------===//

#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/LLVMOriginalImageStorage.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"

using namespace neverd;

namespace {

constexpr va_t CodeVA = 0x1000, DataVA = 0x3000, FlagVA = DataVA + 0x40;

BinaryImage image(Arch A = Arch::X64, BinaryFormat F = BinaryFormat::COFF,
                  bool Writable = true, va_t Target = 0x1888) {
  BinaryImage Image;
  Image.Arch = A;
  Image.Format = F;
  Image.Bits =
      (A == Arch::X86 || A == Arch::ARM) ? Bitness::Bits32 : Bitness::Bits64;
  Segment Code;
  Code.VA = CodeVA;
  Code.Size = Code.FileSz = 0x1000;
  Code.Data.resize(Code.Size);
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Image.Segments.push_back(Code);
  Segment Data;
  Data.VA = DataVA;
  Data.Size = Data.FileSz = 0x80;
  Data.Data.resize(Data.Size);
  for (unsigned I = 0; I != Data.Data.size(); ++I)
    Data.Data[I] = static_cast<uint8_t>(I);
  Data.Flags = SegmentFlags::Readable;
  if (Writable)
    Data.Flags = Data.Flags | SegmentFlags::Writable;
  Image.Segments.push_back(Data);
  for (unsigned Offset : {8u, 0x78u}) {
    for (unsigned I = 0; I != Image.getPointerSize(); ++I)
      Image.Segments.back().Data[Offset + I] =
          static_cast<uint8_t>(Target >> (I * 8));
    Image.CodePtrRelocSlots.insert(DataVA + Offset);
  }
  return Image;
}

MedFunc function(const BinaryImage &, va_t Entry = CodeVA) {
  MedFunc F;
  F.Entry = Entry;
  F.Name = Entry == CodeVA ? "probe" : "target";
  F.CC = CallingConv::Win64;
  F.ReturnType = NdType::makeVoid();
  F.Blocks.resize(1);
  F.Blocks.front().Id = 0;
  F.Blocks.front().StartAddr = Entry;
  F.Blocks.front().EndAddr = Entry + 0x20;
  return F;
}

MedVar address(const BinaryImage &Image, va_t VA) {
  return MedVar::makeConst(VA, Image.getPointerSize(),
                           ConstantAddressProvenance::DataAddress);
}

void load(MedFunc &F, const BinaryImage &Image, va_t VA, unsigned Size) {
  MedOp Op;
  Op.Opcode = NdOp::LOAD;
  Op.Addr = F.Entry + F.Blocks.front().Ops.size();
  Op.Output.Kind = MedVar::Temp;
  Op.Output.Id = static_cast<int>(F.Blocks.front().Ops.size()) + 1;
  Op.Output.Size = Size;
  Op.addInput(address(Image, VA));
  F.Blocks.front().Ops.push_back(Op);
}

void store(MedFunc &F, const BinaryImage &Image, va_t VA, unsigned Size) {
  MedOp Op;
  Op.Opcode = NdOp::STORE;
  Op.Addr = F.Entry + F.Blocks.front().Ops.size();
  Op.addInput(address(Image, VA));
  Op.addInput(
      MedVar::makeConst(0x12345678, Size, ConstantAddressProvenance::Scalar));
  F.Blocks.front().Ops.push_back(Op);
}

void finish(MedFunc &F, const BinaryImage &Image, va_t ReturnedAddress = 0) {
  MedOp Op;
  Op.Opcode = NdOp::RETURN;
  Op.Addr = F.Entry + F.Blocks.front().Ops.size();
  if (ReturnedAddress) {
    F.ReturnType = NdType::makeInt(Image.getPointerSize(), false);
    Op.addInput(address(Image, ReturnedAddress));
  }
  F.Blocks.front().Ops.push_back(Op);
}

std::unique_ptr<llvm::Module> emit(const BinaryImage &Image,
                                   const std::vector<MedFunc> &Functions,
                                   llvm::LLVMContext &Context,
                                   bool Mergeable = false,
                                   const std::vector<char> *Mask = nullptr) {
  auto Module =
      MedLLVMEmitter().emit(Functions, Context, "direct-data", Image.Arch, {},
                            &Image, Image.Format, Mergeable, Mask);
  if (Module)
    EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  return Module;
}

// The original image addresses differ by one, so the generated accesses must
// retain the same storage and byte displacement, including after promotion.
void expectAliased(llvm::Module &Module, unsigned ExpectedAccesses) {
  const llvm::Value *Storage = nullptr;
  int64_t StoreOffset = 0;
  unsigned Accesses = 0;
  for (auto &BB : *Module.getFunction("probe"))
    for (auto &I : BB) {
      llvm::Value *Pointer = nullptr;
      bool IsStore = false;
      if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I);
          Load && Load->isVolatile())
        Pointer = Load->getPointerOperand();
      if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
          Store && Store->isVolatile()) {
        Pointer = Store->getPointerOperand();
        IsStore = true;
      }
      if (!Pointer)
        continue;
      int64_t Offset = 0;
      const llvm::Value *Base = llvm::GetPointerBaseWithConstantOffset(
          Pointer, Offset, Module.getDataLayout());
      ASSERT_TRUE(llvm::isa<llvm::GlobalVariable>(Base));
      if (IsStore) {
        Storage = Base;
        StoreOffset = Offset;
      } else if (Accesses == 1) {
        EXPECT_EQ(Base, Storage);
        EXPECT_EQ(Offset, StoreOffset + 1);
      }
      ++Accesses;
    }
  EXPECT_EQ(Accesses, ExpectedAccesses);
}

TEST(MedLLVMDirectData, UnrelatedCodePointersDoNotBlockFiniteBytes) {
  for (Arch A : {Arch::X64, Arch::X86, Arch::AArch64, Arch::ARM})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (bool Writable : {false, true})
        for (unsigned Width : {1u, 2u, 4u, 8u, 16u}) {
          SCOPED_TRACE(static_cast<int>(A));
          SCOPED_TRACE(static_cast<int>(Format));
          SCOPED_TRACE(Writable);
          SCOPED_TRACE(Width);
          BinaryImage Image = image(A, Format, Writable);
          MedFunc F = function(Image);
          load(F, Image, FlagVA, Width);
          finish(F, Image);
          llvm::LLVMContext Context;
          auto Module = emit(Image, {F}, Context);
          ASSERT_NE(Module, nullptr);
          unsigned Reads = 0;
          for (auto &BB : *Module->getFunction("probe"))
            for (auto &I : BB)
              if (auto *Read = llvm::dyn_cast<llvm::LoadInst>(&I);
                  Read && Read->isVolatile()) {
                int64_t Offset = 0;
                auto *Storage = llvm::dyn_cast<llvm::GlobalVariable>(
                    llvm::GetPointerBaseWithConstantOffset(
                        Read->getPointerOperand(), Offset,
                        Module->getDataLayout()));
                ASSERT_NE(Storage, nullptr);
                EXPECT_EQ(Storage->isConstant(), !Writable);
                const auto *Bytes = llvm::dyn_cast<llvm::ConstantDataArray>(
                    Storage->getInitializer());
                ASSERT_NE(Bytes, nullptr);
                for (unsigned J = 0; J != Width; ++J)
                  EXPECT_EQ(Bytes->getElementAsInteger(Offset + J), 0x40u + J);
                ++Reads;
              }
          EXPECT_EQ(Reads, 1u);
        }
}

TEST(MedLLVMDirectData, DirectWritesAndOverlappingReadsShareStorage) {
  BinaryImage Image = image();
  MedFunc F = function(Image);
  store(F, Image, FlagVA, 4);
  load(F, Image, FlagVA + 1, 2);
  finish(F, Image);
  llvm::LLVMContext Context;
  auto Module = emit(Image, {F}, Context);
  ASSERT_NE(Module, nullptr);
  expectAliased(*Module, 2);
}

TEST(MedLLVMDirectData, SelectedCDeclaresOriginalImageStorage) {
  BinaryImage Image = image();
  MedFunc F = function(Image);
  store(F, Image, FlagVA, 4);
  load(F, Image, FlagVA + 1, 2);
  finish(F, Image);
  llvm::LLVMContext Context;
  auto Module = emit(Image, {F}, Context);
  ASSERT_NE(Module, nullptr);
  CEmitterOptions Options;
  Options.TheArch = Image.Arch;
  Options.Format = Image.Format;
  std::string Text;
  llvm::raw_string_ostream Out(Text);
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options, nullptr, &Image,
                                  Module->getFunction("probe")));
  Out.flush();
  EXPECT_NE(Text.find("extern unsigned char "), std::string::npos) << Text;
  EXPECT_EQ(Text.find(" = {"), std::string::npos) << Text;
}

TEST(MedLLVMDirectData, ARequiredPointerStillRejectsAnUnresolvedTarget) {
  for (auto [VA, Width] :
       {std::pair{DataVA + 8, 8u}, std::pair{DataVA + 9, 1u},
        std::pair{DataVA + 7, 2u}, std::pair{DataVA + 0x74, 8u}}) {
    BinaryImage Image = image();
    MedFunc F = function(Image);
    load(F, Image, VA, Width);
    finish(F, Image);
    llvm::LLVMContext Context;
    EXPECT_EQ(emit(Image, {F}, Context), nullptr);
  }
}

TEST(MedLLVMDirectData, OriginalImageCursorsIgnoreUnrelatedPointerRelocations) {
  for (Arch A : {Arch::X64, Arch::AArch64})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (auto Provenance : {ConstantAddressProvenance::Address,
                              ConstantAddressProvenance::DataAddress}) {
        SCOPED_TRACE(static_cast<int>(A));
        SCOPED_TRACE(static_cast<int>(Format));
        SCOPED_TRACE(static_cast<int>(Provenance));
        BinaryImage Image = image(A, Format);
        MedFunc F = function(Image);
        F.Name = "advance_cursor";
        F.ReturnType = NdType::makeInt(8, false);
        F.Blocks.resize(3);
        for (int I = 0; I != 3; ++I) {
          F.Blocks[I].Id = I;
          F.Blocks[I].StartAddr = CodeVA + I * 0x20;
          F.Blocks[I].EndAddr = CodeVA + (I + 1) * 0x20;
        }
        F.Blocks[0].Succs = {1};
        F.Blocks[1].Preds = {0, 1};
        F.Blocks[1].Succs = {1, 2};
        F.Blocks[2].Preds = {1};
        auto Value = [&](int Id, unsigned Size = 8) {
          MedVar V;
          V.Kind = MedVar::Temp;
          V.TheArch = A;
          V.Id = Id;
          V.SSAVer = 1;
          V.Size = Size;
          return V;
        };
        auto Scalar = [](uint64_t Bits, unsigned Size = 8) {
          return MedVar::makeConst(Bits, Size,
                                   ConstantAddressProvenance::Scalar);
        };
        auto Add = [&](unsigned Block, NdOp Opcode, MedVar Output,
                       std::initializer_list<MedVar> Inputs) {
          MedOp Op;
          Op.Opcode = Opcode;
          Op.Output = Output;
          Op.Addr = F.Blocks[Block].StartAddr + F.Blocks[Block].Ops.size();
          for (const auto &Input : Inputs)
            Op.addInput(Input);
          F.Blocks[Block].Ops.push_back(Op);
        };
        MedVar Count = Value(0);
        Count.Kind = MedVar::Param;
        Count.RegOff = getTargetRegInfo(A).integerParamRegs(Format)[0];
        F.Params.push_back(Count);
        Add(0, NdOp::COPY, Value(1),
            {MedVar::makeConst(FlagVA, 8, Provenance)});
        Add(0, NdOp::BRANCH, {}, {Scalar(F.Blocks[1].StartAddr)});
        F.Blocks[1].Phis.push_back({Value(2), {{0, Value(1)}, {1, Value(3)}}});
        F.Blocks[1].Phis.push_back({Value(4), {{0, Scalar(0)}, {1, Value(5)}}});
        Add(1, NdOp::LOAD, Value(6, 1), {Value(2)});
        Add(1, NdOp::INT_XOR, Value(7, 1), {Value(6, 1), Scalar(0x5a, 1)});
        Add(1, NdOp::STORE, {}, {Value(2), Value(7, 1)});
        Add(1, NdOp::INT_ADD, Value(3), {Value(2), Scalar(1)});
        Add(1, NdOp::INT_ADD, Value(5), {Value(4), Scalar(1)});
        Add(1, NdOp::INT_LESS, Value(8, 1), {Value(5), Count});
        Add(1, NdOp::COND_BR, {}, {Scalar(F.Blocks[1].StartAddr), Value(8, 1)});
        MedVar Result = Value(9);
        Result.Kind = MedVar::Reg;
        Result.RegOff = getTargetRegInfo(A).IntReturnReg;
        Add(2, NdOp::COPY, Result, {Value(3)});
        Add(2, NdOp::RETURN, {}, {Result});
        MedFunc Literal = function(Image, CodeVA + 0x80);
        Literal.Name = "numeric_collision";
        Literal.ReturnType = NdType::makeInt(8, false);
        MedOp Copy;
        Copy.Opcode = NdOp::COPY;
        Copy.Output = Result;
        Copy.addInput(Scalar(FlagVA));
        Literal.Blocks.front().Ops.push_back(Copy);
        MedOp Return;
        Return.Opcode = NdOp::RETURN;
        Return.addInput(Result);
        Literal.Blocks.front().Ops.push_back(Return);
        llvm::LLVMContext Context;
        MedLLVMEmitter Emitter;
        Emitter.setOriginalImageStorageForSource(true);
        auto Module = Emitter.emit({F, Literal}, Context, "original-cursor", A,
                                   {}, &Image, Format);
        ASSERT_NE(Module, nullptr);
        ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
        unsigned OriginalSpans = 0;
        for (const auto &Global : Module->globals())
          OriginalSpans += llvm_original_image::isMarked(Global);
        ASSERT_EQ(OriginalSpans, 1u);
        CEmitterOptions Options;
        Options.TheArch = A;
        Options.Format = Format;
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        ASSERT_TRUE(
            LLVMCEmitter().emit(*Module, Out, Options, nullptr, &Image));
        Out.flush();
        const std::string Prefix = "extern uint8_t ";
        const size_t Start = Source.find(Prefix);
        ASSERT_NE(Start, std::string::npos) << Source;
        const size_t NameStart = Start + Prefix.size();
        const std::string Name =
            Source.substr(NameStart, Source.find('[', NameStart) - NameStart);
        const size_t Size = DataVA + Image.Segments.back().Size - CodeVA;
        Source += "\nuint8_t " + Name + "[" + std::to_string(Size) +
                  "];\n#define image " + Name + "\n#define offset " +
                  std::to_string(FlagVA - CodeVA) + "\n";
        Source += "#define original_address " + std::to_string(FlagVA) + "\n";
        Source += R"c(
int main(void) {
  for (unsigned count = 1; count <= 16; ++count) {
    for (unsigned i = 0; i < sizeof(image); ++i) image[i] = i * 13 + count;
    if (advance_cursor(count) != (uintptr_t)(image + offset + count)) return 1;
    if (numeric_collision() != original_address) return 2;
    for (unsigned i = 0; i < sizeof(image); ++i) {
      uint8_t expected = i * 13 + count;
      if (i >= offset && i < offset + count) expected ^= 0x5a;
      if (image[i] != expected) return 3;
    }
  }
  return 0;
}
)c";
        for (llvm::StringRef Level : {"-O0", "-O2"})
          source_call_execution_test::compileAndRun(
              Source, {Level, "-fsanitize=address,undefined",
                       "-fno-sanitize-recover=all"});
      }
}

TEST(MedLLVMDirectData, TakingAnAddressPromotesEarlierByteAccesses) {
  BinaryImage Image = image(Arch::X64, BinaryFormat::COFF, true, CodeVA);
  MedFunc F = function(Image);
  store(F, Image, FlagVA, 4);
  load(F, Image, FlagVA + 1, 2);
  finish(F, Image, FlagVA);
  llvm::LLVMContext Context;
  auto Module = emit(Image, {F}, Context);
  ASSERT_NE(Module, nullptr);
  expectAliased(*Module, 2);
  // Promotion removes the private byte copy, rather than leaving a second
  // mutable identity beside the returned whole-run pointer.
  EXPECT_EQ(Module->global_size(), 1u);
  auto *Mirror = &*Module->global_begin();
  auto *Return = llvm::cast<llvm::ReturnInst>(
      Module->getFunction("probe")->back().getTerminator());
  const auto *Integer =
      llvm::dyn_cast<llvm::ConstantExpr>(Return->getReturnValue());
  ASSERT_NE(Integer, nullptr);
  ASSERT_EQ(Integer->getOpcode(), llvm::Instruction::PtrToInt);
  int64_t Offset = 0;
  EXPECT_EQ(llvm::GetPointerBaseWithConstantOffset(
                Integer->getOperand(0), Offset, Module->getDataLayout()),
            Mirror);
  EXPECT_EQ(Offset, FlagVA - DataVA);
}

TEST(MedLLVMDirectData, PointerAndScalarAccessOrderKeepsOneIdentity) {
  for (bool PointerFirst : {false, true}) {
    BinaryImage Image = image(Arch::X64, BinaryFormat::COFF, true, CodeVA);
    MedFunc F = function(Image);
    if (PointerFirst)
      load(F, Image, DataVA + 8, 8);
    store(F, Image, FlagVA, 4);
    load(F, Image, FlagVA + 1, 2);
    if (!PointerFirst)
      load(F, Image, DataVA + 8, 8);
    finish(F, Image);
    llvm::LLVMContext Context;
    auto Module = emit(Image, {F}, Context);
    ASSERT_NE(Module, nullptr);
    EXPECT_EQ(Module->global_size(), 1u);
  }
}

TEST(MedLLVMDirectData, UnrelatedDataPointersDoNotExpandDependencies) {
  BinaryImage Image = image();
  Image.CodePtrRelocSlots.erase(DataVA + 8);
  Image.DataPtrRelocSlots.insert(DataVA + 8);
  Segment Other = Image.Segments.back();
  Other.VA = 0x5000;
  Image.Segments.push_back(Other);
  for (unsigned I = 0; I != 8; ++I)
    Image.Segments[1].Data[8 + I] =
        static_cast<uint8_t>(uint64_t{0x5000} >> (8 * I));
  Image.CodePtrRelocSlots.insert(0x5008);
  MedFunc F = function(Image);
  load(F, Image, FlagVA, 4);
  finish(F, Image);
  llvm::LLVMContext Context;
  auto Module = emit(Image, {F}, Context);
  ASSERT_NE(Module, nullptr);
  EXPECT_EQ(Module->global_size(), 1u);
}

TEST(MedLLVMDirectData, PromotionAcrossFunctionsPreservesOverlappingAccesses) {
  for (bool AddressFirst : {false, true}) {
    BinaryImage Image = image(Arch::X64, BinaryFormat::COFF, true, CodeVA);
    MedFunc F = function(Image);
    store(F, Image, FlagVA, 4);
    load(F, Image, FlagVA + 1, 2);
    finish(F, Image);
    MedFunc Other = function(Image, CodeVA + 0x100);
    finish(Other, Image, FlagVA);
    llvm::LLVMContext Context;
    const std::vector<MedFunc> Functions = AddressFirst
                                               ? std::vector<MedFunc>{Other, F}
                                               : std::vector<MedFunc>{F, Other};
    auto Module = emit(Image, Functions, Context);
    ASSERT_NE(Module, nullptr);
    EXPECT_EQ(Module->global_size(), 1u);
    expectAliased(*Module, 2);
  }
}

TEST(MedLLVMDirectData, MaskedBodiesDoNotDemandTheirPointerDependencies) {
  BinaryImage Image = image();
  MedFunc Reader = function(Image);
  load(Reader, Image, FlagVA, 4);
  finish(Reader, Image);
  MedFunc Other = function(Image, CodeVA + 0x100);
  load(Other, Image, DataVA + 8, 8);
  finish(Other, Image);
  llvm::LLVMContext Context;
  std::vector<char> Mask{1, 0};
  auto Module = emit(Image, {Reader, Other}, Context, false, &Mask);
  ASSERT_NE(Module, nullptr);
  EXPECT_TRUE(Module->getFunction("target")->isDeclaration());
  EXPECT_FALSE(Module->getFunction("probe")->isDeclaration());
}

TEST(MedLLVMDirectData, AtomicByteStorageRetainsOriginalAddressAlignment) {
  BinaryImage Image = image();
  MedFunc F = function(Image);
  load(F, Image, FlagVA, 4);
  F.Blocks.front().Ops.back().MemoryOrdering = NdMemoryOrdering::Acquire;
  finish(F, Image);
  llvm::LLVMContext Context;
  auto Module = emit(Image, {F}, Context);
  ASSERT_NE(Module, nullptr);
  unsigned Reads = 0;
  for (auto &BB : *Module->getFunction("probe"))
    for (auto &I : BB)
      if (auto *Read = llvm::dyn_cast<llvm::LoadInst>(&I);
          Read && Read->isAtomic()) {
        int64_t Offset = 0;
        const auto *Storage = llvm::dyn_cast<llvm::GlobalVariable>(
            llvm::GetPointerBaseWithConstantOffset(
                Read->getPointerOperand(), Offset, Module->getDataLayout()));
        ASSERT_NE(Storage, nullptr);
        EXPECT_GE(Storage->getAlign().valueOrOne().value(), 4u);
        EXPECT_EQ(Offset % 4, 0);
        EXPECT_EQ(Read->getAlign().value(), 4u);
        ++Reads;
      }
  EXPECT_EQ(Reads, 1u);
}

TEST(MedLLVMDirectData, ShardsRetainTheirSharedWholeRunContract) {
  BinaryImage Image = image(Arch::X64, BinaryFormat::COFF, true, CodeVA);
  MedFunc F = function(Image);
  load(F, Image, FlagVA, 4);
  finish(F, Image);
  llvm::LLVMContext Context;
  std::vector<char> Mask{1};
  auto Module = emit(Image, {F}, Context, true, &Mask);
  ASSERT_NE(Module, nullptr);
  EXPECT_EQ(Module->global_size(), 1u);
  EXPECT_TRUE(Module->global_begin()->hasLinkOnceODRLinkage());
  EXPECT_TRUE(
      llvm::isa<llvm::StructType>(Module->global_begin()->getValueType()));
}

TEST(MedLLVMDirectData, LargeGapsGrowOverlappingFiniteStorage) {
  for (Arch A : {Arch::X64, Arch::X86, Arch::AArch64, Arch::ARM})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (bool Writable : {false, true})
        for (bool Reverse : {false, true}) {
          SCOPED_TRACE(static_cast<unsigned>(A));
          SCOPED_TRACE(static_cast<unsigned>(Format));
          SCOPED_TRACE(Writable);
          SCOPED_TRACE(Reverse);
          BinaryImage Image = image(A, Format, Writable);
          // The pointer gap exceeds the embedding limit independently of the
          // requested eight-byte object. Keep an unresolved executable target
          // elsewhere in the same segment to expose accidental promotion.
          auto &Data = Image.Segments.back();
          Data.Size = Data.FileSz = 2 * 1024 * 1024;
          Data.Data.resize(Data.Size);
          Image.CodePtrRelocSlots.erase(DataVA + 0x78);
          MedFunc F = function(Image);
          if (Reverse)
            load(F, Image, FlagVA + 1, 2);
          store(F, Image, FlagVA, 8);
          load(F, Image, FlagVA + 1, 2);
          finish(F, Image);
          llvm::LLVMContext Context;
          auto Module = emit(Image, {F}, Context);
          ASSERT_NE(Module, nullptr);
          EXPECT_EQ(Module->global_size(), 1u);
          EXPECT_EQ(
              Module->global_begin()->getValueType()->getArrayNumElements(),
              8u);
          const llvm::Value *Storage = nullptr;
          unsigned Accesses = 0;
          for (auto &BB : *Module->getFunction("probe"))
            for (auto &I : BB) {
              llvm::Value *Pointer = nullptr;
              if (auto *L = llvm::dyn_cast<llvm::LoadInst>(&I);
                  L && L->isVolatile())
                Pointer = L->getPointerOperand();
              if (auto *S = llvm::dyn_cast<llvm::StoreInst>(&I);
                  S && S->isVolatile())
                Pointer = S->getPointerOperand();
              if (!Pointer)
                continue;
              int64_t Offset = 0;
              const auto *Base = llvm::GetPointerBaseWithConstantOffset(
                  Pointer, Offset, Module->getDataLayout());
              if (!Storage)
                Storage = Base;
              EXPECT_EQ(Base, Storage);
              EXPECT_EQ(Offset, llvm::isa<llvm::StoreInst>(I) ? 0 : 1);
              ++Accesses;
            }
          EXPECT_EQ(Accesses, Reverse ? 3u : 2u);
        }
}

TEST(MedLLVMDirectData, LargeGapBridgingAndPromotionKeepOneStorage) {
  for (bool Promote : {false, true}) {
    BinaryImage Image = image(Arch::X64, BinaryFormat::COFF, true, CodeVA);
    auto &Data = Image.Segments.back();
    Data.Size = Data.FileSz = 2 * 1024 * 1024;
    Data.Data.resize(Data.Size);
    Image.CodePtrRelocSlots.erase(DataVA + 0x78);
    MedFunc F = function(Image);
    load(F, Image, FlagVA + 8, 4);
    store(F, Image, FlagVA, 4);
    load(F, Image, FlagVA + 2, 8); // Bridge both earlier byte objects.
    finish(F, Image, Promote ? FlagVA : 0);
    llvm::LLVMContext Context;
    auto Module = emit(Image, {F}, Context);
    ASSERT_NE(Module, nullptr);
    EXPECT_EQ(Module->global_size(), 1u);
    const llvm::Value *Storage = nullptr;
    unsigned Accesses = 0;
    for (auto &BB : *Module->getFunction("probe"))
      for (auto &I : BB) {
        llvm::Value *Pointer = nullptr;
        if (auto *L = llvm::dyn_cast<llvm::LoadInst>(&I); L && L->isVolatile())
          Pointer = L->getPointerOperand();
        if (auto *S = llvm::dyn_cast<llvm::StoreInst>(&I); S && S->isVolatile())
          Pointer = S->getPointerOperand();
        if (!Pointer)
          continue;
        int64_t Offset = 0;
        const auto *Base = llvm::GetPointerBaseWithConstantOffset(
            Pointer, Offset, Module->getDataLayout());
        if (!Storage)
          Storage = Base;
        EXPECT_EQ(Base, Storage);
        const unsigned Displacements[] = {8, 0, 2};
        EXPECT_EQ(Offset,
                  (Promote ? FlagVA - DataVA : 0) + Displacements[Accesses++]);
      }
    EXPECT_EQ(Accesses, 3u);
  }
}

TEST(MedLLVMDirectData, LargeGapBackwardGrowthPreservesAtomicAlignment) {
  for (bool Promote : {false, true}) {
    BinaryImage Image =
        image(Arch::X64, BinaryFormat::COFF, true, Promote ? CodeVA : 0x1888);
    auto &Data = Image.Segments.back();
    Data.Size = Data.FileSz = 2 * 1024 * 1024;
    Data.Data.resize(Data.Size);
    Image.CodePtrRelocSlots.erase(DataVA + 0x78);
    MedFunc F = function(Image);
    load(F, Image, FlagVA, 4);
    F.Blocks.front().Ops.back().MemoryOrdering = NdMemoryOrdering::Acquire;
    store(F, Image, FlagVA - 1, 2);
    finish(F, Image, Promote ? FlagVA : 0);
    llvm::LLVMContext Context;
    auto Module = emit(Image, {F}, Context);
    ASSERT_NE(Module, nullptr);
    EXPECT_EQ(Module->global_size(), 1u);
    unsigned Reads = 0;
    for (auto &BB : *Module->getFunction("probe"))
      for (auto &I : BB)
        if (auto *Read = llvm::dyn_cast<llvm::LoadInst>(&I);
            Read && Read->isAtomic()) {
          int64_t Offset = 0;
          auto *Storage = llvm::dyn_cast<llvm::GlobalVariable>(
              llvm::GetPointerBaseWithConstantOffset(
                  Read->getPointerOperand(), Offset, Module->getDataLayout()));
          ASSERT_NE(Storage, nullptr);
          EXPECT_TRUE(Storage->getName().starts_with("__nd_codeptr_3000"));
          EXPECT_EQ(Offset, FlagVA - DataVA);
          EXPECT_GE(Storage->getAlign().valueOrOne().value(), 4u);
          EXPECT_EQ(Offset % 4, 0);
          ++Reads;
        }
    EXPECT_EQ(Reads, 1u);
  }
}

TEST(MedLLVMDirectData, LargeGapsKeepAdjacentSegmentsIndependent) {
  for (Arch A : {Arch::X64, Arch::X86, Arch::AArch64, Arch::ARM})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (bool Reverse : {false, true}) {
        SCOPED_TRACE(static_cast<unsigned>(A));
        SCOPED_TRACE(static_cast<unsigned>(Format));
        SCOPED_TRACE(Reverse);
        BinaryImage Image = image(A, Format, true);
        // The last finite bytes of one mapping touch the first finite bytes of
        // another. Their storage and permissions remain independent in either
        // access order, even though both relocation-free gaps exceed the limit.
        auto &First = Image.Segments.back();
        First.Size = First.FileSz = 2 * 1024 * 1024;
        First.Data.resize(First.Size, 0x35);
        Image.CodePtrRelocSlots.erase(DataVA + 0x78);
        const va_t Boundary = First.VA + First.Size;
        Segment Second;
        Second.VA = Boundary;
        Second.Size = Second.FileSz = First.Size;
        Second.Data.resize(Second.Size, 0x79);
        Second.Flags = SegmentFlags::Readable;
        Image.CodePtrRelocSlots.insert(Second.VA + Second.Size - 8);
        Image.Segments.push_back(std::move(Second));

        MedFunc F = function(Image);
        const va_t Addresses[] = {Boundary - 4, Boundary};
        load(F, Image, Addresses[Reverse ? 1 : 0], 4);
        load(F, Image, Addresses[Reverse ? 0 : 1], 4);
        finish(F, Image);
        llvm::LLVMContext Context;
        auto Module = emit(Image, {F}, Context);
        ASSERT_NE(Module, nullptr);
        EXPECT_EQ(Module->global_size(), 2u);
        unsigned Reads = 0;
        for (auto &BB : *Module->getFunction("probe"))
          for (auto &I : BB)
            if (auto *Read = llvm::dyn_cast<llvm::LoadInst>(&I);
                Read && Read->isVolatile()) {
              int64_t Offset = 0;
              auto *Storage = llvm::dyn_cast<llvm::GlobalVariable>(
                  llvm::GetPointerBaseWithConstantOffset(
                      Read->getPointerOperand(), Offset,
                      Module->getDataLayout()));
              ASSERT_NE(Storage, nullptr);
              ASSERT_LT(Reads, 2u);
              const bool IsSecond =
                  Addresses[Reverse ? 1 - Reads : Reads] == Boundary;
              EXPECT_EQ(Storage->isConstant(), IsSecond);
              auto *Bytes = llvm::dyn_cast<llvm::ConstantDataArray>(
                  Storage->getInitializer());
              ASSERT_NE(Bytes, nullptr);
              ASSERT_GE(Offset, 0);
              ASSERT_LE(static_cast<uint64_t>(Offset) + 4,
                        Bytes->getNumElements());
              for (unsigned Byte = 0; Byte != 4; ++Byte)
                EXPECT_EQ(Bytes->getElementAsInteger(Offset + Byte),
                          IsSecond ? 0x79u : 0x35u);
              ++Reads;
            }
        EXPECT_EQ(Reads, 2u);
      }
}

} // namespace

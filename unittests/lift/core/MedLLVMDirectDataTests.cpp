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

#include "neverd/Limits.h"
#include "neverd/backend/LLVMOriginalImageStorage.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"

#include <algorithm>

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

enum class MirrorPointerForm {
  DirectPhi,
  Copy,
  ZeroExtend,
  SignExtend,
  LowSlice,
  Narrow,
  Mask,
  Scalar,
  RawPhi,
  AddressCopy,
  AddressPhi,
  MaskedBackedge,
  TwoPointers
};

// The program selects a caller buffer or a static buffer. A loop then resets
// its cursor to static_base + index, so the static initializer is also shared
// by the backedge. It performs observable byte reads and writes on both arms.
MedFunc runtimeMirrorMerge(const BinaryImage &Image, MirrorPointerForm Form,
                           bool Loop, bool Reverse) {
  MedFunc F = function(Image);
  F.CC = Image.Arch == Arch::AArch64 ? CallingConv::ARM_AAPCS
                                     : CallingConv::SysV_AMD64;
  F.ReturnType = NdType::makeInt(8, false);
  F.Blocks.resize(Loop ? 6 : 4);
  for (int I = 0; I != static_cast<int>(F.Blocks.size()); ++I) {
    F.Blocks[I].Id = I;
    F.Blocks[I].StartAddr = CodeVA + I * 0x20;
    F.Blocks[I].EndAddr = CodeVA + (I + 1) * 0x20;
  }
  F.Blocks[0].Succs = {1, 2};
  F.Blocks[1].Preds = {0};
  F.Blocks[1].Succs = {3};
  F.Blocks[2].Preds = {0};
  F.Blocks[2].Succs = {3};
  F.Blocks[3].Preds = {1, 2};
  auto Value = [&](int Id, unsigned Size = 8) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = Image.Arch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = Size;
    return V;
  };
  auto Scalar = [](uint64_t Bits, unsigned Size = 8) {
    return MedVar::makeConst(Bits, Size, ConstantAddressProvenance::Scalar);
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
  MedVar Runtime = Value(0);
  Runtime.Kind = MedVar::Param;
  Runtime.RegOff =
      getTargetRegInfo(Image.Arch).integerParamRegs(Image.Format)[0];
  F.Params.push_back(Runtime);
  MedVar Index = Value(8), Laps = Value(9);
  for (MedVar *Parameter : {&Index, &Laps}) {
    Parameter->Kind = MedVar::Param;
    Parameter->RegOff = getTargetRegInfo(Image.Arch)
                            .integerParamRegs(Image.Format)[F.Params.size()];
    F.Params.push_back(*Parameter);
  }
  MedVar Static = address(Image, DataVA + 0x20);
  if (Form == MirrorPointerForm::RawPhi)
    Static.Provenance = ConstantAddressProvenance::Unknown;
  else if (Form == MirrorPointerForm::AddressPhi)
    Static.Provenance = ConstantAddressProvenance::Address;
  else if (Form != MirrorPointerForm::DirectPhi) {
    if (Form == MirrorPointerForm::Scalar)
      Static.Provenance = ConstantAddressProvenance::Scalar;
    if (Form == MirrorPointerForm::AddressCopy)
      Static.Provenance = ConstantAddressProvenance::Address;
    NdOp Forward = Form == MirrorPointerForm::ZeroExtend   ? NdOp::INT_ZEXT
                   : Form == MirrorPointerForm::SignExtend ? NdOp::INT_SEXT
                   : Form == MirrorPointerForm::LowSlice   ? NdOp::SUBBYTES
                                                           : NdOp::COPY;
    if (Forward == NdOp::SUBBYTES)
      Add(0, Forward, Value(1), {Static, Scalar(0)});
    else
      Add(0, Forward, Value(1, Form == MirrorPointerForm::Narrow ? 4 : 8),
          {Static});
    Static = Value(1);
    if (Form == MirrorPointerForm::Narrow) {
      Add(0, NdOp::INT_ZEXT, Value(2), {Value(1, 4)});
      Static = Value(2);
    } else if (Form == MirrorPointerForm::Mask) {
      Add(0, NdOp::INT_AND, Value(2), {Static, Scalar(0x7fffffff)});
      Static = Value(2);
    }
  }
  Add(0, NdOp::INT_NOTEQUAL, Value(3, 1), {Runtime, Scalar(0)});
  Add(0, NdOp::COND_BR, {}, {Scalar(F.Blocks[2].StartAddr), Value(3, 1)});
  Add(1, NdOp::BRANCH, {}, {Scalar(F.Blocks[3].StartAddr)});
  Add(2, NdOp::BRANCH, {}, {Scalar(F.Blocks[3].StartAddr)});
  F.Blocks[3].Phis.push_back({Value(4), {{1, Static}, {2, Runtime}}});
  if (Loop) {
    F.Blocks[3].Preds.push_back(4);
    F.Blocks[3].Succs = {4, 5};
    F.Blocks[4].Preds = {3};
    F.Blocks[4].Succs = {3};
    F.Blocks[5].Preds = {3};
    F.Blocks[3].Phis.front().Args.emplace_back(4, Value(10));
    F.Blocks[3].Phis.push_back(
        {Value(11), {{1, Scalar(0)}, {2, Scalar(0)}, {4, Value(12)}}});
    if (Form == MirrorPointerForm::MaskedBackedge)
      Add(4, NdOp::INT_AND, Value(10), {Static, Scalar(0x7fffffff)});
    else if (Form == MirrorPointerForm::TwoPointers)
      Add(4, NdOp::INT_ADD, Value(10), {Static, address(Image, DataVA + 0x30)});
    else
      Add(4, NdOp::INT_ADD, Value(10), {Static, Index});
    Add(4, NdOp::BRANCH, {}, {Scalar(F.Blocks[3].StartAddr)});
  }
  if (Reverse)
    for (PhiNode &Phi : F.Blocks[3].Phis)
      std::reverse(Phi.Args.begin(), Phi.Args.end());
  Add(3, NdOp::INT_ADD, Value(5), {Value(4), Scalar(2)});
  Add(3, NdOp::LOAD, Value(6, 1), {Value(5)});
  Add(3, NdOp::INT_ZEXT, Value(7), {Value(6, 1)});
  if (Loop) {
    Add(3, NdOp::INT_XOR, Value(13, 1), {Value(6, 1), Scalar(0x5a, 1)});
    Add(3, NdOp::STORE, {}, {Value(5), Value(13, 1)});
    Add(3, NdOp::INT_ADD, Value(12), {Value(11), Scalar(1)});
    Add(3, NdOp::INT_LESS, Value(14, 1), {Value(12), Laps});
    Add(3, NdOp::COND_BR, {}, {Scalar(F.Blocks[4].StartAddr), Value(14, 1)});
  }
  MedVar Result = Value(15);
  Result.Kind = MedVar::Reg;
  Result.RegOff = getTargetRegInfo(Image.Arch).IntReturnReg;
  Add(Loop ? 5 : 3, NdOp::COPY, Result, {Value(7)});
  Add(Loop ? 5 : 3, NdOp::RETURN, {}, {Result});
  return F;
}

TEST(MedLLVMDirectData, RuntimeMirrorMergePreservesOnlyCompletePointers) {
  for (Arch A : {Arch::AArch64, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool Loop : {false, true})
        for (bool Reverse : {false, true})
          for (MirrorPointerForm Form :
               {MirrorPointerForm::DirectPhi, MirrorPointerForm::Copy,
                MirrorPointerForm::ZeroExtend, MirrorPointerForm::SignExtend,
                MirrorPointerForm::LowSlice, MirrorPointerForm::Narrow,
                MirrorPointerForm::Mask, MirrorPointerForm::Scalar,
                MirrorPointerForm::RawPhi, MirrorPointerForm::AddressCopy,
                MirrorPointerForm::AddressPhi,
                MirrorPointerForm::MaskedBackedge,
                MirrorPointerForm::TwoPointers}) {
            if (!Loop && (Form == MirrorPointerForm::MaskedBackedge ||
                          Form == MirrorPointerForm::TwoPointers))
              continue;
            SCOPED_TRACE(static_cast<int>(A));
            SCOPED_TRACE(static_cast<int>(Format));
            SCOPED_TRACE(Loop);
            SCOPED_TRACE(Reverse);
            SCOPED_TRACE(static_cast<int>(Form));
            BinaryImage Image = image(A, Format);
            MedFunc F = runtimeMirrorMerge(Image, Form, Loop, Reverse);
            MedFunc Target = function(Image, 0x1888);
            finish(Target, Image);
            llvm::LLVMContext Context;
            auto Module = emit(Image, {F, Target}, Context);
            const bool Preserves = Form <= MirrorPointerForm::LowSlice ||
                                   Form == MirrorPointerForm::AddressCopy ||
                                   Form == MirrorPointerForm::AddressPhi;
            EXPECT_EQ(Module != nullptr, Preserves);
          }
}

TEST(MedLLVMDirectData, DeferredMirrorAddressNeedsCompleteMutableStorage) {
  for (Arch A : {Arch::AArch64, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (unsigned Storage : {0u, 1u, 2u})
        for (MirrorPointerForm Form :
             {MirrorPointerForm::AddressCopy, MirrorPointerForm::AddressPhi}) {
          SCOPED_TRACE(static_cast<int>(A));
          SCOPED_TRACE(static_cast<int>(Format));
          SCOPED_TRACE(Storage);
          SCOPED_TRACE(static_cast<int>(Form));
          BinaryImage Image = image(A, Format, Storage != 0);
          if (Storage == 1)
            Image.Segments.back().ReadOnlyAfterRelocations = true;
          if (Storage == 2) {
            Image.Segments.back().Data.resize(0x70);
            Image.Segments.back().FileSz = 0x70;
            Image.CodePtrRelocSlots.erase(DataVA + 0x78);
          }
          MedFunc F = runtimeMirrorMerge(Image, Form, true, false);
          MedFunc Target = function(Image, 0x1888);
          finish(Target, Image);
          llvm::LLVMContext Context;
          EXPECT_EQ(emit(Image, {F, Target}, Context), nullptr);
        }
}

TEST(MedLLVMDirectData, MappedNumericStridesKeepTheirScalarOccurrence) {
  for (Arch A : {Arch::AArch64, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (MirrorPointerForm Form :
           {MirrorPointerForm::Copy, MirrorPointerForm::AddressCopy,
            MirrorPointerForm::AddressPhi, MirrorPointerForm::Scalar,
            MirrorPointerForm::Mask, MirrorPointerForm::TwoPointers}) {
        SCOPED_TRACE(static_cast<int>(A));
        SCOPED_TRACE(static_cast<int>(Format));
        SCOPED_TRACE(static_cast<int>(Form));
        BinaryImage Image = image(A, Format);
        // Numeric shifts, offsets and zero now all coincide with mapped code.
        auto &Code = Image.Segments.front();
        Code.VA = 0;
        Code.Size = Code.FileSz = 0x2000;
        Code.Data.resize(Code.Size);
        MedFunc F = runtimeMirrorMerge(Image, Form, true, false);
        MedFunc Target = function(Image, 0x1888);
        finish(Target, Image);
        llvm::LLVMContext Context;
        const bool Preserves = Form == MirrorPointerForm::Copy ||
                               Form == MirrorPointerForm::AddressCopy ||
                               Form == MirrorPointerForm::AddressPhi;
        EXPECT_EQ(emit(Image, {F, Target}, Context) != nullptr, Preserves);
      }
}

TEST(MedLLVMDirectData, NullablePreviousPointerKeepsNullInLowMappedImage) {
  for (Arch A : {Arch::AArch64, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool Reverse : {false, true}) {
        SCOPED_TRACE(static_cast<int>(A));
        SCOPED_TRACE(static_cast<int>(Format));
        SCOPED_TRACE(Reverse);
        BinaryImage Image = image(A, Format);
        Image.Segments.front().VA = 0;
        Image.Segments.front().Size = Image.Segments.front().FileSz = 0x2000;
        Image.Segments.front().Data.resize(0x2000);
        MedFunc F = runtimeMirrorMerge(Image, MirrorPointerForm::AddressCopy,
                                       true, Reverse);
        MedVar Current = F.Blocks[3].Phis.front().Output;
        MedVar Previous = Current, Present = Current, Selected = Current;
        Previous.Id = 16;
        Present.Id = 17;
        Present.Size = 1;
        Selected.Id = 18;
        const MedVar Zero =
            MedVar::makeConst(0, 8, ConstantAddressProvenance::Scalar);
        PhiNode Phi{Previous, {{1, Zero}, {2, Zero}, {4, Current}}};
        if (Reverse)
          std::reverse(Phi.Args.begin(), Phi.Args.end());
        F.Blocks[3].Phis.push_back(Phi);
        MedOp Compare;
        Compare.Opcode = NdOp::INT_NOTEQUAL;
        Compare.Output = Present;
        Compare.addInput(Previous);
        Compare.addInput(Zero);
        MedOp Choose;
        Choose.Opcode = NdOp::SELECT;
        Choose.Output = Selected;
        Choose.addInput(Present);
        Choose.addInput(Previous);
        Choose.addInput(Current);
        F.Blocks[3].Ops.front().Inputs[0] = Selected;
        F.Blocks[3].Ops.insert(F.Blocks[3].Ops.begin(), {Compare, Choose});
        MedFunc Target = function(Image, 0x1888);
        finish(Target, Image);
        llvm::LLVMContext Context;
        EXPECT_NE(emit(Image, {F, Target}, Context), nullptr);
      }
}

TEST(MedLLVMDirectData, MutableMirrorPointerSurvivesRepeatedCalls) {
#if !defined(__aarch64__) && !defined(__arm64__)
  GTEST_SKIP() << "The independent assembly oracle requires an AArch64 host";
#else
  BinaryImage Image = image(Arch::AArch64, BinaryFormat::MachO);
  Image.Segments.front().VA = 0;
  Image.Segments.front().Size = Image.Segments.front().FileSz = 0x2000;
  Image.Segments.front().Data.resize(0x2000);
  MedFunc F =
      runtimeMirrorMerge(Image, MirrorPointerForm::AddressCopy, true, false);
  auto Value = [&](int Id, unsigned Size = 8) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = Image.Arch;
    V.Id = Id;
    V.SSAVer = 1;
    V.Size = Size;
    return V;
  };
  auto Scalar = [](uint64_t Bits, unsigned Size = 8) {
    return MedVar::makeConst(Bits, Size, ConstantAddressProvenance::Scalar);
  };
  auto ArchitecturalAddress = [](va_t VA) {
    return MedVar::makeConst(VA, 8, ConstantAddressProvenance::Address);
  };
  auto Op = [](NdOp Opcode, MedVar Output,
               std::initializer_list<MedVar> Inputs) {
    MedOp Result;
    Result.Opcode = Opcode;
    Result.Output = Output;
    for (const MedVar &Input : Inputs)
      Result.addInput(Input);
    return Result;
  };
  const MedVar Base = ArchitecturalAddress(DataVA + 0x20);
  const MedVar Cell = address(Image, DataVA + 0x10);
  const MedVar Delta = address(Image, DataVA + 0x60);
  // A persisted pointer outranks the caller's fallback. The first invocation
  // can initialize the cell; every subsequent one consumes the stored value.
  for (MedOp &Operation : F.Blocks[0].Ops)
    if (Operation.Opcode == NdOp::INT_NOTEQUAL)
      Operation.Inputs[0] = Value(22);
  F.Blocks[3].Phis.front().Args[1].second = Value(22);
  F.Blocks[0].Ops.insert(
      F.Blocks[0].Ops.begin(),
      {Op(NdOp::LOAD, Value(20), {Cell}),
       Op(NdOp::INT_NOTEQUAL, Value(21, 1), {Value(20), Scalar(0)}),
       Op(NdOp::SELECT, Value(22), {Value(21, 1), Value(20), F.Params[0]})});
  F.Blocks[3].Ops.front().Inputs[0] = Value(31);
  F.Blocks[3].Ops.insert(
      F.Blocks[3].Ops.begin(),
      {Op(NdOp::INT_EQUAL, Value(30, 1),
          {Value(4), ArchitecturalAddress(DataVA + 0x30)}),
       Op(NdOp::SELECT, Value(31), {Value(30, 1), Base, Value(4)})});
  auto &Exit = F.Blocks[5].Ops;
  const MedVar Result = Exit.front().Output;
  Exit = {Op(NdOp::INT_ADD, Value(32), {Value(31), Scalar(1)}),
          Op(NdOp::STORE, {}, {Cell, Value(32)}),
          Op(NdOp::SUBBYTES, Value(33, 4), {Value(32), Scalar(0)}),
          Op(NdOp::SUBBYTES, Value(34, 4), {Base, Scalar(0)}),
          Op(NdOp::INT_SUB, Value(35, 4), {Value(33, 4), Value(34, 4)}),
          Op(NdOp::STORE, {}, {Delta, Value(35, 4)}),
          Op(NdOp::CALL, Value(36), {Scalar(CodeVA + 0x300)}),
          Op(NdOp::COPY, Result, {Value(36)}),
          Op(NdOp::RETURN, {}, {Result})};
  MedCallInfo Call;
  Call.BlockId = 5;
  Call.OpIdx = 6;
  Call.TargetAddr = CodeVA + 0x300;
  Call.TargetName = "observe_pointer";
  Call.ArgumentsFromPrototype = true;
  Call.Args = {Value(32), Base, Value(35, 4)};
  F.CallInfos.push_back(Call);

  std::vector<MedFunc> Functions{F, function(Image, 0x1888)};
  finish(Functions.back(), Image);
  for (auto [Name, VA] : {std::pair{"static_base", DataVA + 0x20},
                          std::pair{"cursor_cell", DataVA + 0x10},
                          std::pair{"delta_cell", DataVA + 0x60}}) {
    MedFunc Accessor = function(Image, CodeVA + 0x100 + Functions.size() * 16);
    Accessor.Name = Name;
    Accessor.CC = CallingConv::ARM_AAPCS;
    Accessor.ReturnType = NdType::makeInt(8, false);
    Accessor.Blocks.front().Ops = {Op(NdOp::COPY, Result, {address(Image, VA)}),
                                   Op(NdOp::RETURN, {}, {Result})};
    Functions.push_back(std::move(Accessor));
  }
  llvm::LLVMContext Context;
  auto Module = emit(Image, Functions, Context);
  ASSERT_NE(Module, nullptr);
  Module->setTargetTriple(llvm::Triple(llvm::sys::getDefaultTargetTriple()));
  llvm::SmallString<128> IRPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-mutable-pointer",
                                                  "ll", IRPath));
  llvm::FileRemover RemoveIR(IRPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream IR(IRPath, EC);
    ASSERT_FALSE(EC);
    Module->print(IR, nullptr);
  }
  const std::string Harness = R"c(
#include <stdint.h>
#include <string.h>
extern uint64_t probe(uint64_t, uint64_t, uint64_t);
extern uint64_t static_base(void), cursor_cell(void), delta_cell(void);
static uint64_t wanted_pointer, wanted_base;
static uint32_t wanted_delta;
static unsigned calls, bad_call;
uint64_t observe_pointer(uint64_t pointer, uint64_t base, uint32_t delta, ...) {
  ++calls;
  bad_call |= pointer != wanted_pointer || base != wanted_base ||
              delta != wanted_delta;
  return pointer;
}
static uint64_t original(uint8_t **cell, uint8_t *runtime, uint8_t *base,
                         uint32_t *delta, uint64_t index, uint64_t laps) {
  uintptr_t result;
  __asm__ volatile(
      "ldr x8, [%x[cell]]\n"
      "cmp x8, #0\n"
      "csel x8, x8, %x[runtime], ne\n"
      "cmp x8, #0\n"
      "csel x8, x8, %x[base], ne\n"
      "mov x9, #0\n"
      "1:\n"
      "add x12, %x[base], #16\n"
      "cmp x8, x12\n"
      "csel x8, %x[base], x8, eq\n"
      "ldrb w10, [x8, #2]\n"
      "mov w11, #0x5a\n"
      "eor w11, w10, w11\n"
      "strb w11, [x8, #2]\n"
      "add x9, x9, #1\n"
      "cmp x9, %x[laps]\n"
      "b.hs 2f\n"
      "add x8, %x[base], %x[index]\n"
      "b 1b\n"
      "2:\n"
      "add %x[result], x8, #1\n"
      "str %x[result], [%x[cell]]\n"
      "sub w12, %w[result], %w[base]\n"
      "str w12, [%x[delta]]\n"
      : [result] "=&r"(result)
      : [cell] "r"(cell), [runtime] "r"(runtime), [base] "r"(base),
        [delta] "r"(delta), [index] "r"(index), [laps] "r"(laps)
      : "x8", "x9", "x10", "x11", "x12", "cc", "memory");
  return observe_pointer(result, (uintptr_t)base, *delta);
}
static uint8_t *initial(unsigned kind, uint8_t *base, uint8_t *runtime) {
  return kind < 2 ? 0 : kind == 2 ? base + 8 : kind == 3 ? base + 16
                    : runtime + 8;
}
static uint8_t *counterpart(uint8_t *p, uint8_t *expected_static,
                            uint8_t *expected_runtime, uint8_t *base,
                            uint8_t *runtime) {
  for (unsigned i = 0; i < 32; ++i) {
    if (p == expected_static + i) return base + i;
    if (p == expected_runtime + i) return runtime + i;
  }
  return 0;
}
int main(void) {
  uint8_t *rebuilt_static = (uint8_t *)(uintptr_t)static_base();
  uint8_t **rebuilt_cell = (uint8_t **)(uintptr_t)cursor_cell();
  uint32_t *rebuilt_delta = (uint32_t *)(uintptr_t)delta_cell();
  for (unsigned seed = 0; seed < 4; ++seed)
    for (unsigned kind = 0; kind < 6; ++kind)
      for (unsigned index = 0; index < 16; ++index)
        for (unsigned laps = 1; laps <= 4; ++laps) {
          uint8_t original_static[32], expected_static[32];
          uint8_t original_runtime[32], rebuilt_runtime[32], expected_runtime[32];
          for (unsigned i = 0; i < 32; ++i) {
            expected_static[i] = original_static[i] = rebuilt_static[i] =
                (uint8_t)(seed * 17 + i * 7);
            expected_runtime[i] = original_runtime[i] = rebuilt_runtime[i] =
                (uint8_t)(seed * 31 + i * 13);
          }
          uint8_t *original_cell = initial(kind, original_static, original_runtime);
          uint8_t *expected_cell = initial(kind, expected_static, expected_runtime);
          *rebuilt_cell = initial(kind, rebuilt_static, rebuilt_runtime);
          uint8_t *op = kind == 1 ? original_runtime : kind == 5 ? original_static : 0;
          uint8_t *rp = kind == 1 ? rebuilt_runtime : kind == 5 ? rebuilt_static : 0;
          uint8_t *ep = kind == 1 ? expected_runtime : kind == 5 ? expected_static : 0;
          for (unsigned round = 0; round < 5; ++round) {
            uint8_t *p = expected_cell ? expected_cell : ep ? ep : expected_static;
            for (unsigned iteration = 0; iteration < laps; ++iteration) {
              if (p == expected_static + 16) p = expected_static;
              p[2] ^= 0x5a;
              if (iteration + 1 < laps) p = expected_static + index;
            }
            expected_cell = p + 1;
            uint8_t *onext = counterpart(expected_cell, expected_static,
                expected_runtime, original_static, original_runtime);
            uint8_t *rnext = counterpart(expected_cell, expected_static,
                expected_runtime, rebuilt_static, rebuilt_runtime);
            if (!onext || !rnext) return 1;
            uint32_t original_delta = 0;
            wanted_pointer = (uintptr_t)onext;
            wanted_base = (uintptr_t)original_static;
            wanted_delta = (uint32_t)wanted_pointer - (uint32_t)wanted_base;
            if (original(&original_cell, op, original_static, &original_delta,
                         index, laps) != wanted_pointer) return 2;
            if (original_cell != onext || original_delta != wanted_delta) return 3;
            wanted_pointer = (uintptr_t)rnext;
            wanted_base = (uintptr_t)rebuilt_static;
            wanted_delta = (uint32_t)wanted_pointer - (uint32_t)wanted_base;
            if (probe((uintptr_t)rp, index, laps) != wanted_pointer) return 4;
            if (*rebuilt_cell != rnext || *rebuilt_delta != wanted_delta) return 5;
            if (memcmp(original_static, expected_static, 32) ||
                memcmp(rebuilt_static, expected_static, 32)) return 6;
            if (memcmp(original_runtime, expected_runtime, 32) ||
                memcmp(rebuilt_runtime, expected_runtime, 32)) return 7;
            if (bad_call) return 8;
          }
        }
  return calls != 4 * 6 * 16 * 4 * 5 * 2;
}
)c";
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.TheArch = Image.Arch;
  Options.Format = Image.Format;
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options, nullptr, &Image));
  Out.flush();
  for (llvm::StringRef Level : {"-O0", "-O2"}) {
    source_call_execution_test::compileAndRun(
        Harness, {IRPath, Level, "-fsanitize=address,undefined",
                  "-fno-sanitize-recover=all"});
    source_call_execution_test::compileAndRun(
        Source + Harness,
        {Level, "-fsanitize=address,undefined", "-fno-sanitize-recover=all"});
  }
#endif
}

TEST(MedLLVMDirectData, RuntimeMirrorLoopMatchesNativeMemoryEffects) {
#if !defined(__aarch64__) && !defined(__arm64__)
  GTEST_SKIP() << "The independent assembly oracle requires an AArch64 host";
#else
  BinaryImage Image = image(Arch::AArch64, BinaryFormat::MachO);
  MedFunc F = runtimeMirrorMerge(Image, MirrorPointerForm::Copy, true, false);
  MedFunc Target = function(Image, 0x1888);
  finish(Target, Image);
  MedFunc Base = function(Image, CodeVA + 0x100);
  Base.Name = "static_base";
  Base.CC = CallingConv::ARM_AAPCS;
  Base.ReturnType = NdType::makeInt(8, false);
  MedVar Result;
  Result.Kind = MedVar::Reg;
  Result.TheArch = Image.Arch;
  Result.Id = 0;
  Result.SSAVer = 1;
  Result.Size = 8;
  Result.RegOff = getTargetRegInfo(Image.Arch).IntReturnReg;
  MedOp Copy;
  Copy.Opcode = NdOp::COPY;
  Copy.Output = Result;
  Copy.addInput(address(Image, DataVA + 0x20));
  Base.Blocks.front().Ops.push_back(Copy);
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(Result);
  Base.Blocks.front().Ops.push_back(Return);
  llvm::LLVMContext Context;
  auto Module = emit(Image, {F, Target, Base}, Context);
  ASSERT_TRUE(Module != nullptr);
  Module->setTargetTriple(llvm::Triple(llvm::sys::getDefaultTargetTriple()));
  llvm::SmallString<128> IRPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-mirror-loop", "ll", IRPath));
  llvm::FileRemover RemoveIR(IRPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream IR(IRPath, EC);
    ASSERT_FALSE(EC);
    Module->print(IR, nullptr);
  }
  const std::string Harness = R"c(
#include <stdint.h>
#include <string.h>
extern uint64_t probe(uint64_t, uint64_t, uint64_t);
extern uint64_t static_base(void);
static uint64_t original(uint8_t *runtime, uint8_t *base,
                         uint64_t index, uint64_t laps) {
  uint32_t result;
  __asm__ volatile(
      "cmp %x[runtime], #0\n"
      "csel x8, %x[runtime], %x[base], ne\n"
      "mov x9, #0\n"
      "1:\n"
      "ldrb w10, [x8, #2]\n"
      "mov w11, #0x5a\n"
      "eor w11, w10, w11\n"
      "strb w11, [x8, #2]\n"
      "add x9, x9, #1\n"
      "cmp x9, %x[laps]\n"
      "b.hs 2f\n"
      "add x8, %x[base], %x[index]\n"
      "b 1b\n"
      "2:\n"
      "mov %w[result], w10\n"
      : [result] "=&r"(result)
      : [runtime] "r"(runtime), [base] "r"(base),
        [index] "r"(index), [laps] "r"(laps)
      : "x8", "x9", "x10", "x11", "cc", "memory");
  return result;
}
int main(void) {
  uint8_t *rebuilt_static = (uint8_t *)(uintptr_t)static_base();
  for (unsigned seed = 0; seed < 16; ++seed)
    for (unsigned kind = 0; kind < 3; ++kind)
      for (unsigned index = 0; index < 16; ++index)
        for (unsigned laps = 1; laps <= 6; ++laps) {
          uint8_t original_static[32], expected_static[32];
          uint8_t original_runtime[32], rebuilt_runtime[32], expected_runtime[32];
          for (unsigned i = 0; i < 32; ++i) {
            expected_static[i] = original_static[i] = rebuilt_static[i] =
                (uint8_t)(seed * 17 + i * 7);
            expected_runtime[i] = original_runtime[i] = rebuilt_runtime[i] =
                (uint8_t)(seed * 31 + i * 13);
          }
          uint8_t *op = kind == 0 ? 0 : kind == 1 ? original_runtime
                                                   : original_static + 8;
          uint8_t *rp = kind == 0 ? 0 : kind == 1 ? rebuilt_runtime
                                                   : rebuilt_static + 8;
          uint8_t *ep = kind == 0 ? expected_static : kind == 1 ? expected_runtime
                                                                : expected_static + 8;
          uint64_t expected = 0;
          for (unsigned iteration = 0; iteration < laps; ++iteration) {
            expected = ep[2];
            ep[2] ^= 0x5a;
            ep = expected_static + index;
          }
          if (original(op, original_static, index, laps) != expected) return 1;
          if (probe((uintptr_t)rp, index, laps) != expected) return 2;
          if (memcmp(original_static, expected_static, 32) ||
              memcmp(rebuilt_static, expected_static, 32)) return 3;
          if (memcmp(original_runtime, expected_runtime, 32) ||
              memcmp(rebuilt_runtime, expected_runtime, 32)) return 4;
        }
  return 0;
}
)c";
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.TheArch = Image.Arch;
  Options.Format = Image.Format;
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options, nullptr, &Image));
  Out.flush();
  for (llvm::StringRef Level : {"-O0", "-O2"}) {
    source_call_execution_test::compileAndRun(
        Harness, {IRPath, Level, "-fsanitize=address,undefined",
                  "-fno-sanitize-recover=all"});
    source_call_execution_test::compileAndRun(
        Source + Harness,
        {Level, "-fsanitize=address,undefined", "-fno-sanitize-recover=all"});
  }
#endif
}

namespace {

class LinkedDataDebug : public NullDebugContext {
public:
  std::pair<va_t, va_t> Region{FlagVA, FlagVA + 32};
  std::optional<std::pair<va_t, va_t>>
  readOnlyDataRegion(va_t Address) const override {
    return Address >= Region.first && Address < Region.second
               ? std::optional(Region)
               : std::nullopt;
  }
};

BinaryImage mixedDataInCode(Arch A, BinaryFormat Format) {
  BinaryImage Image = image(A, Format, false);
  auto &Data = Image.Segments.back();
  Data.Size = Data.FileSz = limits::kMaxSingleGlobalEmbedLen + 0x1000;
  Data.Data.resize(Data.Size);
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Image.DataInCode.insert(FlagVA);
  Image.DataPtrRelocSlots.insert(FlagVA + 8);
  for (unsigned I = 0; I < Image.getPointerSize(); ++I)
    Data.Data[FlagVA + 8 - DataVA + I] =
        static_cast<uint8_t>((FlagVA + 24) >> (I * 8));
  return Image;
}

std::unique_ptr<llvm::Module> emitLinkedData(const BinaryImage &Image,
                                             llvm::LLVMContext &Context,
                                             const DebugContext *Debug,
                                             bool Reverse = false) {
  MedFunc Base = function(Image), Interior = function(Image, CodeVA + 0x100);
  auto ReturnAddress = [&](MedFunc &Function, va_t Address) {
    MedOp Copy;
    Copy.Opcode = NdOp::COPY;
    Copy.Addr = Function.Entry;
    Copy.Output.Kind = MedVar::Reg;
    Copy.Output.TheArch = Image.Arch;
    Copy.Output.RegOff = getTargetRegInfo(Image.Arch).IntReturnReg;
    Copy.Output.Size = Image.getPointerSize();
    Copy.Output.Id = 1;
    Copy.Output.SSAVer = 1;
    Copy.addInput(address(Image, Address));
    Function.Blocks.front().Ops.push_back(Copy);
    finish(Function, Image, Address);
    Function.Blocks.front().Ops.back().Inputs[0] = Copy.Output;
  };
  ReturnAddress(Base, FlagVA);
  ReturnAddress(Interior, FlagVA + 4);
  std::vector<MedFunc> Functions{Base, Interior};
  if (Reverse)
    std::reverse(Functions.begin(), Functions.end());
  MedLLVMEmitter Emitter;
  Emitter.setDebugContext(Debug);
  return Emitter.emit(Functions, Context, "linked-data", Image.Arch, {}, &Image,
                      Image.Format);
}

} // namespace

TEST(MedLLVMDirectData, LinkedDataRegionMirrorsMixedRecordsAndInteriorAliases) {
  for (Arch A : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (bool Reverse : {false, true}) {
        for (bool Relocated : {false, true}) {
          SCOPED_TRACE(static_cast<unsigned>(A));
          SCOPED_TRACE(static_cast<unsigned>(Format));
          SCOPED_TRACE(Reverse);
          SCOPED_TRACE(Relocated);
          BinaryImage Image = mixedDataInCode(A, Format);
          if (!Relocated)
            Image.DataPtrRelocSlots.clear();
          LinkedDataDebug Debug;
          llvm::LLVMContext Context;
          auto Module = emitLinkedData(Image, Context, &Debug, Reverse);
          ASSERT_NE(Module, nullptr);
          EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
          auto *Storage = Module->getNamedGlobal("__nd_codeptr_3040");
          ASSERT_NE(Storage, nullptr);
          EXPECT_EQ(
              Module->getDataLayout().getTypeAllocSize(Storage->getValueType()),
              32u);
          for (const auto &[Name, Expected] :
               {std::pair{"probe", 0}, std::pair{"target", 4}}) {
            auto *Function = Module->getFunction(Name);
            // Eliminate the emitter's register allocas before checking the
            // returned storage identity. This transforms IR; it executes no
            // generated instructions.
            llvm::SmallVector<llvm::AllocaInst *> Allocas;
            for (auto &I : Function->getEntryBlock())
              if (auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(&I);
                  Alloca && llvm::isAllocaPromotable(Alloca))
                Allocas.push_back(Alloca);
            llvm::DominatorTree Dominators(*Function);
            llvm::PromoteMemToReg(Allocas, Dominators);
            auto *Return =
                llvm::cast<llvm::ReturnInst>(Function->back().getTerminator());
            auto *Cast =
                llvm::dyn_cast<llvm::ConstantExpr>(Return->getReturnValue());
            ASSERT_NE(Cast, nullptr);
            ASSERT_EQ(Cast->getOpcode(), llvm::Instruction::PtrToInt);
            int64_t Offset = 0;
            EXPECT_EQ(llvm::GetPointerBaseWithConstantOffset(
                          Cast->getOperand(0), Offset, Module->getDataLayout()),
                      Storage);
            EXPECT_EQ(Offset, Expected);
          }
          // The relocated member refers to the same storage at offset 24,
          // rather than retaining a numeric address in the original code
          // segment.
          auto *Initializer =
              llvm::dyn_cast<llvm::ConstantStruct>(Storage->getInitializer());
          ASSERT_NE(Initializer, nullptr);
          if (Relocated) {
            ASSERT_EQ(Initializer->getNumOperands(), 3u);
            auto *Pointer =
                llvm::dyn_cast<llvm::ConstantExpr>(Initializer->getOperand(1));
            ASSERT_NE(Pointer, nullptr);
            ASSERT_EQ(Pointer->getOpcode(), llvm::Instruction::PtrToInt);
            int64_t Offset = 0;
            EXPECT_EQ(
                llvm::GetPointerBaseWithConstantOffset(
                    Pointer->getOperand(0), Offset, Module->getDataLayout()),
                Storage);
            EXPECT_EQ(Offset, 24);
          } else {
            ASSERT_EQ(Initializer->getNumOperands(), 1u);
            const auto *Bytes = llvm::dyn_cast<llvm::ConstantDataSequential>(
                Initializer->getOperand(0));
            ASSERT_NE(Bytes, nullptr);
            for (unsigned I = 0; I != 32; ++I)
              EXPECT_EQ(Bytes->getElementAsInteger(I),
                        Image.Segments.back().Data[FlagVA - DataVA + I]);
          }
        }
      }
}

TEST(MedLLVMDirectData, LinkedDataRegionRequiresCompletePointerStorage) {
  for (Arch A : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    const unsigned HalfPointer = getTargetRegInfo(A).PointerSize / 2;
    for (va_t Slot : {FlagVA - HalfPointer, FlagVA + 8 + HalfPointer,
                      FlagVA + 32 - HalfPointer}) {
      BinaryImage Image = mixedDataInCode(A, BinaryFormat::COFF);
      Image.DataPtrRelocSlots.insert(Slot);
      LinkedDataDebug Debug;
      llvm::LLVMContext Context;
      EXPECT_EQ(emitLinkedData(Image, Context, &Debug), nullptr);
    }
  }
}

TEST(MedLLVMDirectData, LinkedDataRegionRetainsExtentAndTargetRefusals) {
  for (unsigned Case = 0; Case != 4; ++Case) {
    SCOPED_TRACE(Case);
    BinaryImage Image = mixedDataInCode(Arch::X64, BinaryFormat::COFF);
    LinkedDataDebug Debug;
    if (Case == 1)
      Debug.Region.second =
          Debug.Region.first + limits::kMaxSingleGlobalEmbedLen + 1;
    if (Case == 2)
      Debug.Region.second = DataVA + Image.Segments.back().Data.size() + 1;
    if (Case == 3) {
      Image.CodePtrRelocSlots.insert(FlagVA + 16);
      for (unsigned I = 0; I != 8; ++I)
        Image.Segments.back().Data[FlagVA + 16 - DataVA + I] =
            static_cast<uint8_t>(uint64_t(0x1888) >> (I * 8));
    }
    llvm::LLVMContext Context;
    EXPECT_EQ(emitLinkedData(Image, Context, Case == 0 ? nullptr : &Debug),
              nullptr);
  }
}

TEST(MedLLVMDirectData, OriginalImageCursorsIgnoreUnrelatedPointerRelocations) {
  for (Arch A : {Arch::X64, Arch::AArch64})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (unsigned SegmentPrefix : {0u, 0x100u})
        for (auto Provenance : {ConstantAddressProvenance::Address,
                                ConstantAddressProvenance::DataAddress,
                                ConstantAddressProvenance::AddressFragment}) {
          if (Provenance == ConstantAddressProvenance::AddressFragment &&
              A != Arch::AArch64)
            continue;
          if (SegmentPrefix &&
              Provenance != ConstantAddressProvenance::AddressFragment)
            continue;
          SCOPED_TRACE(SegmentPrefix);
          SCOPED_TRACE(static_cast<int>(A));
          SCOPED_TRACE(static_cast<int>(Format));
          SCOPED_TRACE(static_cast<int>(Provenance));
          BinaryImage Image = image(A, Format);
          Image.Segments.back().VA += SegmentPrefix;
          const va_t BaseVA =
              Provenance == ConstantAddressProvenance::AddressFragment ? DataVA
                                                                       : FlagVA;
          const va_t CursorVA = BaseVA + SegmentPrefix;
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
          Add(0, NdOp::COPY, Value(11),
              {MedVar::makeConst(BaseVA, 8, Provenance)});
          Add(0, NdOp::INT_ADD, Value(1), {Value(11), Scalar(SegmentPrefix)});
          Add(0, NdOp::INT_ADD, Value(10), {Value(1), Count});
          Add(0, NdOp::BRANCH, {}, {Scalar(F.Blocks[1].StartAddr)});
          F.Blocks[1].Phis.push_back(
              {Value(2), {{0, Value(1)}, {1, Value(3)}}});
          F.Blocks[1].Phis.push_back(
              {Value(4), {{0, Scalar(0)}, {1, Value(5)}}});
          Add(1, NdOp::LOAD, Value(6, 1), {Value(2)});
          Add(1, NdOp::INT_XOR, Value(7, 1), {Value(6, 1), Scalar(0x5a, 1)});
          Add(1, NdOp::STORE, {}, {Value(2), Value(7, 1)});
          Add(1, NdOp::INT_ADD, Value(3), {Value(2), Scalar(1)});
          Add(1, NdOp::INT_ADD, Value(5), {Value(4), Scalar(1)});
          Add(1, NdOp::INT_NOTEQUAL, Value(8, 1), {Value(3), Value(10)});
          Add(1, NdOp::COND_BR, {},
              {Scalar(F.Blocks[1].StartAddr), Value(8, 1)});
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
          Copy.addInput(Scalar(CursorVA));
          Literal.Blocks.front().Ops.push_back(Copy);
          MedOp Return;
          Return.Opcode = NdOp::RETURN;
          Return.addInput(Result);
          Literal.Blocks.front().Ops.push_back(Return);
          MedFunc PageBits = function(Image, CodeVA + 0xc0);
          PageBits.Name = "image_page_bits";
          PageBits.ReturnType = NdType::makeInt(8, false);
          Copy.Opcode = NdOp::INT_AND;
          Copy.Inputs[0] = MedVar::makeConst(BaseVA, 8, Provenance);
          Copy.addInput(Scalar(0xfff));
          PageBits.Blocks.front().Ops.push_back(Copy);
          PageBits.Blocks.front().Ops.push_back(Return);
          llvm::LLVMContext Context;
          MedLLVMEmitter Emitter;
          Emitter.setOriginalImageStorageForSource(true);
          auto Module = Emitter.emit({F, Literal, PageBits}, Context,
                                     "original-cursor", A, {}, &Image, Format);
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
          const size_t Size =
              Image.Segments.back().VA + Image.Segments.back().Size - CodeVA;
          Source += std::string(A == Arch::AArch64 ? "\n_Alignas(4096) uint8_t "
                                                   : "\nuint8_t ") +
                    Name + "[" + std::to_string(Size) + "];\n#define image " +
                    Name + "\n#define offset " +
                    std::to_string(CursorVA - CodeVA) + "\n";
          Source +=
              "#define original_address " + std::to_string(CursorVA) + "\n";
          Source += "#define original_page " + std::to_string(BaseVA) + "\n";
          Source += "#define check_page_bits " +
                    std::to_string(A == Arch::AArch64) + "\n";
          Source += R"c(
int main(void) {
  for (unsigned count = 1; count <= 16; ++count) {
    for (unsigned i = 0; i < sizeof(image); ++i) image[i] = i * 13 + count;
    if (advance_cursor(count) != (uintptr_t)(image + offset + count)) return 1;
    if (numeric_collision() != original_address) return 2;
    if (check_page_bits && image_page_bits() != (original_page & 0xfff)) return 4;
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

TEST(MedLLVMDirectData, ScalarMasksKeepNumericRolesInPointerOffsets) {
  for (Arch A : {Arch::X64, Arch::AArch64})
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (unsigned Variant = 0; Variant != 4; ++Variant)
        for (auto Provenance : {ConstantAddressProvenance::Scalar,
                                ConstantAddressProvenance::DataAddress,
                                ConstantAddressProvenance::Unknown}) {
          SCOPED_TRACE(static_cast<int>(A));
          SCOPED_TRACE(static_cast<int>(Format));
          SCOPED_TRACE(Variant);
          SCOPED_TRACE(static_cast<int>(Provenance));
          const bool Copied = Variant & 1;
          const bool Selection = Variant & 2;
          BinaryImage Image = image(A, Format, false);
          Image.CodePtrRelocSlots.clear();
          MedFunc F = function(Image);
          F.Name = "numeric_mask_offset";
          F.ReturnType = NdType::makeInt(8, false);
          auto Value = [&](int Id, unsigned Size = 8) {
            MedVar V;
            V.Kind = MedVar::Temp;
            V.TheArch = A;
            V.Id = Id;
            V.SSAVer = 1;
            V.Size = Size;
            return V;
          };
          const auto Registers = getTargetRegInfo(A).integerParamRegs(Format);
          for (unsigned I = 0; I != 3; ++I) {
            MedVar Parameter = Value(I);
            Parameter.Kind = MedVar::Param;
            Parameter.RegOff = Registers[I];
            F.Params.push_back(Parameter);
          }
          auto Add = [&](NdOp Opcode, MedVar Output,
                         std::initializer_list<MedVar> Inputs) {
            MedOp Op;
            Op.Opcode = Opcode;
            Op.Output = Output;
            Op.Addr = CodeVA + 4 * F.Blocks.front().Ops.size();
            for (const auto &Input : Inputs)
              Op.addInput(Input);
            F.Blocks.front().Ops.push_back(Op);
          };
          // The mask's bits coincide with a materializable read-only object.
          // That coincidence grants no address role to a scalar occurrence.
          MedVar Mask = MedVar::makeConst(DataVA, 8, Provenance);
          if (Copied) {
            Add(NdOp::COPY, Value(10), {Mask});
            Mask = Value(10);
          }
          if (Selection) {
            Add(NdOp::SELECT, Value(11, 4), {F.Params[2], Mask, F.Params[1]});
            Add(NdOp::INT_ZEXT, Value(13), {Value(11, 4)});
          } else {
            Add(NdOp::INT_AND, Value(11), {F.Params[1], Mask});
            Add(NdOp::INT_AND, Value(12),
                {F.Params[2],
                 MedVar::makeConst(~uint64_t(DataVA), 8,
                                   ConstantAddressProvenance::Scalar)});
            Add(NdOp::INT_OR, Value(13), {Value(11), Value(12)});
          }
          Add(NdOp::INT_ADD, Value(14), {F.Params[0], Value(13)});
          Add(NdOp::LOAD, Value(15, 1), {Value(14)});
          MedVar Result = Value(16);
          Result.Kind = MedVar::Reg;
          Result.RegOff = getTargetRegInfo(A).IntReturnReg;
          Add(NdOp::INT_ZEXT, Result, {Value(15, 1)});
          Add(NdOp::RETURN, {}, {Result});
          llvm::LLVMContext Context;
          auto Module = emit(Image, {F}, Context);
          if (Provenance != ConstantAddressProvenance::Scalar) {
            EXPECT_EQ(Module, nullptr);
            continue;
          }
          ASSERT_NE(Module, nullptr);
          CEmitterOptions Options;
          Options.TheArch = A;
          Options.Format = Format;
          std::string Source;
          llvm::raw_string_ostream Out(Source);
          ASSERT_TRUE(
              LLVMCEmitter().emit(*Module, Out, Options, nullptr, &Image));
          Out.flush();
          Source +=
              "\n#define select_offset " + std::to_string(Selection) + "\n";
          Source += R"c(
int main(void) {
  static uint8_t bytes[65536];
  for (unsigned i = 0; i != sizeof(bytes); ++i)
    bytes[i] = (uint8_t)((i * 37) ^ (i >> 8));
  for (unsigned x = 0; x < 65536; x += 17)
    for (unsigned y = 0; y < 65536; y += 4093) {
      unsigned offset = select_offset ? (y ? 0x3000 : x)
                                      : (x & 0x3000) | (y & ~0x3000u);
      if ((uint8_t)numeric_mask_offset((uintptr_t)bytes, x, y) != bytes[offset])
        return 1;
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

TEST(MedLLVMDirectData, OriginalImageDoesNotCompleteOtherAddressFragments) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
    for (unsigned Case = 0; Case != 8; ++Case) {
      SCOPED_TRACE(static_cast<int>(Format));
      SCOPED_TRACE(Case);
      BinaryImage Image = image(Case == 0 ? Arch::X64 : Arch::AArch64, Format);
      Image.IsRelocatable = Case == 1;
      if (Case == 2)
        Image.Segments.back().Flags = SegmentFlags::Writable;
      if (Case == 3)
        ++Image.Segments.front().VA;
      const unsigned Size = Case == 4 ? 4 : 8;
      const uint64_t VA = Case == 5   ? DataVA + 1
                          : Case == 6 ? DataVA + 0x1000
                                      : DataVA;
      MedFunc F = function(Image);
      MedOp Store;
      Store.Opcode = NdOp::STORE;
      Store.addInput(address(Image, FlagVA));
      Store.addInput(MedVar::makeConst(
          VA, Size, ConstantAddressProvenance::AddressFragment));
      F.Blocks.front().Ops.push_back(Store);
      finish(F, Image);
      llvm::LLVMContext Context;
      MedLLVMEmitter Emitter;
      Emitter.setOriginalImageStorageForSource(Case != 7);
      EXPECT_EQ(Emitter.emit({F}, Context, "incomplete-page", Image.Arch, {},
                             &Image, Format),
                nullptr);
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

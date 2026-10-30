//===- PipelineSourceStackAccessesTests.cpp - Stack projection -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Source and machine conversions retain distinct stack-access contracts,
/// including when return recovery reconverts a caller.
///
//===----------------------------------------------------------------------===//

#include "PipelineSourceStackAccesses.h"
#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/lift/ARM/ARMRegs.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace neverd;

MedFunc savedCallerRegister(Arch Architecture, bool ReadsRestore) {
  const auto &TRI = getTargetRegInfo(Architecture);
  auto Reg = [&](int Id, uint64_t Offset, int Version) {
    MedVar V;
    V.Kind = MedVar::Reg;
    V.TheArch = Architecture;
    V.Id = Id;
    V.RegOff = Offset;
    V.SSAVer = Version;
    V.Size = TRI.PointerSize;
    return V;
  };
  auto Op = [](NdOp Code, MedVar Output, std::initializer_list<MedVar> Inputs) {
    MedOp O;
    O.Opcode = Code;
    O.Output = Output;
    for (const auto &V : Inputs)
      O.addInput(V);
    return O;
  };
  const auto SP = Reg(1, TRI.StackPointer, 0);
  const auto Frame = Reg(1, TRI.StackPointer, 1);
  const auto Offset = Architecture == Arch::ARM ? armreg::R8 : x86reg::RBX;
  const auto EntryValue = Reg(2, Offset, 0);
  const auto Restored = Reg(2, Offset, 1);
  auto ReturnAddress = Reg(3, 0, 1);
  ReturnAddress.Kind = MedVar::Temp;
  MedVar None;
  None.Id = -1;
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = 0x1000;
  Block.Ops = {
      Op(NdOp::COPY, SP, {SP}),
      Op(NdOp::COPY, EntryValue, {EntryValue}),
      Op(NdOp::INT_SUB, Frame,
         {SP, MedVar::makeConst(TRI.PointerSize, TRI.PointerSize)}),
      Op(NdOp::STORE, None, {Frame, EntryValue}),
      Op(NdOp::LOAD, Restored, {Frame}),
      Op(NdOp::LOAD, ReturnAddress, {SP}),
      Op(NdOp::RETURN, None,
         {ReadsRestore ? Restored : MedVar::makeConst(0, TRI.PointerSize)})};
  MedFunc F;
  F.Entry = Block.StartAddr;
  F.CC = Architecture == Arch::ARM ? CallingConv::ARM_AAPCS
                                   : CallingConv::SysV_AMD64;
  F.FrameSize = TRI.PointerSize;
  F.Blocks.push_back(std::move(Block));
  return F;
}

TEST(PipelineSourceStackAccesses, MachineRequestsKeepCallerStateOperations) {
  struct Case {
    bool Lift;
    bool Patch;
    bool Analysis;
    bool Projection;
    bool Source;
  };
  for (const auto Architecture : {Arch::ARM, Arch::X64})
    for (const Case C : {Case{false, false, false, false, true},
                         Case{true, false, false, false, false},
                         Case{true, false, false, true, true},
                         Case{true, true, false, true, false},
                         Case{false, false, true, true, false}})
      for (const bool ReadsRestore : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << int(Architecture) << ':' << C.Lift << C.Patch
                     << C.Analysis << C.Projection << ':' << ReadsRestore);
        BinaryImage Image;
        Image.Arch = Architecture;
        Image.Format = BinaryFormat::ELF;
        PipelineOptions Options;
        Options.LiftMode = C.Lift;
        Options.PatchMode = C.Patch;
        Options.MedAnalysisOnly = C.Analysis;
        Options.SourceProjection = C.Projection;
        auto F = savedCallerRegister(Architecture, ReadsRestore);
        MedOp Atomic;
        Atomic.Opcode = NdOp::ATOMIC_ADD;
        Atomic.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
        Atomic.Output = F.Blocks[0].Ops[5].Output;
        Atomic.Output.Id = 9;
        Atomic.Output.Size = 1;
        Atomic.addInput(F.Blocks[0].Ops[5].Inputs[0]);
        Atomic.addInput(MedVar::makeConst(0, 1));
        F.Blocks[0].Ops.insert(F.Blocks[0].Ops.end() - 1, Atomic);
        // These ARM/x64 fixtures have no x86 registration-state provenance.
        LowFunc Source;
        Source.Entry = F.Entry;
        prepareMedStackAccesses(F, Source, Image, medStackProjection(Options));
        EXPECT_EQ(F.Blocks[0].Ops[3].PrologueSave, C.Source && !ReadsRestore);
        EXPECT_EQ(F.Blocks[0].Ops[5].ReturnAddressRead,
                  C.Source && Architecture == Arch::X64);
        EXPECT_EQ(F.Blocks[0].Ops[6].SourceReturnSlotOffset.has_value(),
                  C.Source && Architecture == Arch::X64);
        EXPECT_EQ(F.Blocks[0].Ops.size(), 8u);
        EXPECT_EQ(F.Blocks[0].Ops[3].Opcode, NdOp::STORE);
        EXPECT_EQ(F.Blocks[0].Ops[4].Opcode, NdOp::LOAD);
      }
}

TEST(PipelineSourceStackAccesses, ReturnRangesDoNotDependOnFunctionScheduling) {
  const unsigned Previous = workerThreadOverride().load();
  const auto Reset = llvm::scope_exit([&] { setWorkerThreadCount(Previous); });
  for (unsigned Threads : {1, 4})
    for (bool Reverse : {false, true}) {
      setWorkerThreadCount(Threads);
      BinaryImage Image;
      Image.Arch = Arch::X64;
      Image.Format = BinaryFormat::ELF;
      PipelineResult Result;
      const auto &TRI = getTargetRegInfo(Image.Arch);
      for (unsigned I = 0; I != 64; ++I) {
        LowFunc Low;
        Low.Entry = 0x1000 + I * 16;
        Low.Name = "bounded";
        Low.Blocks.resize(1);
        auto &Block = Low.Blocks.front();
        Block.Id = 0;
        Block.StartAddr = Low.Entry;
        Block.EndAddr = Low.Entry + 2;
        LowOp Copy;
        Copy.Opcode = NdOp::COPY;
        Copy.Addr = Low.Entry;
        Copy.Output = NdVar::reg(TRI.IntReturnReg, TRI.PointerSize);
        Copy.addInput(NdVar::scalar(I * 3, TRI.PointerSize));
        LowOp Return;
        Return.Opcode = NdOp::RETURN;
        Return.Addr = Low.Entry + 1;
        Return.addInput(Copy.Output);
        Block.Ops = {Copy, Return};
        Result.MedFuncs.push_back(
            LowToMedConverter().convert(Low, Image.Arch, Image.Format));
        Result.LowFuncs.push_back(std::move(Low));
      }
      if (Reverse) {
        std::reverse(Result.LowFuncs.begin(), Result.LowFuncs.end());
        std::reverse(Result.MedFuncs.begin(), Result.MedFuncs.end());
      }
      const auto Ranges = sourceCallReturnRanges(Image, Result);
      ASSERT_EQ(Ranges.size(), 64u);
      for (unsigned I = 0; I != 64; ++I) {
        const auto Found = Ranges.find(0x1000 + I * 16);
        ASSERT_NE(Found, Ranges.end());
        EXPECT_EQ(Found->second.Min, I * 3);
        EXPECT_EQ(Found->second.Max, I * 3);
      }
      auto &Changed = Result.LowFuncs.front();
      Changed.Blocks.front().Ops.front().Inputs[0] =
          NdVar::reg(TRI.StackPointer, TRI.PointerSize);
      Result.MedFuncs.front() =
          LowToMedConverter().convert(Changed, Image.Arch, Image.Format);
      const auto Fresh = sourceCallReturnRanges(Image, Result);
      EXPECT_EQ(Fresh.size(), 63u);
      EXPECT_FALSE(Fresh.count(Changed.Entry));
    }
}

TEST(PipelineSourceStackAccesses, BoundedCalleeKeepsDynamicFrameRestores) {
  const unsigned Previous = workerThreadOverride().load();
  const auto Reset = llvm::scope_exit([&] { setWorkerThreadCount(Previous); });
  for (unsigned Threads : {1, 4})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (bool Bounded : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << Threads << ':' << int(Format) << ':' << Bounded);
        setWorkerThreadCount(Threads);
        BinaryImage Image;
        Image.Arch = Arch::X64;
        Image.Bits = Bitness::Bits64;
        Image.Format = Format;
        Image.Entry = 0x1000;
        Segment Text;
        Text.Name = ".text";
        Text.VA = Image.Entry;
        Text.Size = Text.FileSz = 0x2006;
        Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
        Text.Data.assign(Text.Size, 0xcc);
        std::vector<uint8_t> Caller = {0x55, 0x53, // push rbp; push rbx
                                       0x48, 0x83, 0xec, 0x28, // sub rsp, 40
                                       0x48, 0x8d, 0x6c, 0x24,
                                       0x20}; // lea rbp, [rsp+32]
        // Keep an observable fixed-frame value across the dynamic allocation.
        // For count -1 the second CALL overwrites exactly this eight-byte slot.
        const uint8_t SavedArgument[] = {
            0x48, 0x89, uint8_t(Format == BinaryFormat::COFF ? 0x4c : 0x7c),
            0x24, 0x18}; // mov [rsp+24], rcx/rdi
        Caller.insert(Caller.end(), std::begin(SavedArgument),
                      std::end(SavedArgument));
        const auto Call = [&](va_t Target) {
          const uint32_t Offset = Target - (Image.Entry + Caller.size() + 5);
          Caller.push_back(0xe8);
          for (unsigned I = 0; I != 4; ++I)
            Caller.push_back(Offset >> (I * 8));
        };
        Call(0x2000);
        const uint8_t Allocation[] = {
            0x48, 0x63, 0xc0,       // movsxd rax, eax
            0x48, 0x6b, 0xc0, 0x28, // imul rax, rax, 40
            0x48, 0x83, 0xc0, 0x0f, // add rax, 15
            0x48, 0x83, 0xe0, 0xf0, // and rax, -16
            0x48, 0x29, 0xc4};      // sub rsp, rax
        Caller.insert(Caller.end(), std::begin(Allocation),
                      std::end(Allocation));
        Call(0x3000);
        const uint8_t Restore[] = {0x48, 0x8b, 0x45, 0xf8, // mov rax, [rbp-8]
                                   0x48, 0x8d, 0x65, 0x08, // lea rsp, [rbp+8]
                                   0x5b, 0x5d, 0xc3}; // pop rbx; pop rbp; ret
        Caller.insert(Caller.end(), std::begin(Restore), std::end(Restore));
        std::copy(Caller.begin(), Caller.end(), Text.Data.begin());
        const uint8_t Memory = Format == BinaryFormat::COFF ? 0x01 : 0x07;
        const std::vector<uint8_t> Callee =
            Bounded ? std::vector<uint8_t>{0x0f, 0xb7, Memory, 0xc3}
                    : std::vector<uint8_t>{0x8b, Memory, 0xc3};
        std::copy(Callee.begin(), Callee.end(), Text.Data.begin() + 0x1000);
        // A memory effect keeps the second call from being replaced by a
        // register-only leaf projection before the stack proof runs.
        const uint8_t Sink[] = {0xc6, Memory, 0x01, 0x31, 0xc0, 0xc3};
        std::copy(std::begin(Sink), std::end(Sink), Text.Data.begin() + 0x2000);
        Image.Segments.push_back(std::move(Text));
        Image.Symbols = {Symbol::makeFunc(0x1000, Caller.size()),
                         Symbol::makeFunc(0x2000, Callee.size()),
                         Symbol::makeFunc(0x3000, sizeof(Sink))};
        PipelineOptions Options;
        Options.LiftMode = Options.SourceProjection = Options.NoOpt = true;
        Options.OnlyFunctionEntries = {0x1000, 0x2000, 0x3000};
        llvm::LLVMContext Context;
        const auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_EQ(Result.Success, Bounded) << Result.Error;
        if (Bounded) {
          ASSERT_NE(Result.LlvmModule, nullptr);
          EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule));
        } else {
          EXPECT_NE(Result.Error.find("physical CALL return slot"),
                    std::string::npos)
              << Result.Error;
        }
      }
}

TEST(PipelineSourceStackAccesses, IncomingArgumentsRetainPhysicalCallWrites) {
  for (const Arch Architecture : {Arch::X86, Arch::X64})
    for (const BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (const bool LLVMSource : {false, true})
        for (const bool FPRead : {false, true})
          for (unsigned Placement = 0; Placement != 3; ++Placement) {
            // 0: a represented local slot; 1: the CALL overwrites an incoming
            // argument; 2: the incoming argument is adjacent to the CALL slot.
            SCOPED_TRACE(::testing::Message()
                         << int(Architecture) << ':' << int(Format) << ':'
                         << LLVMSource << ':' << FPRead << ':' << Placement);
            const bool Wide = Architecture == Arch::X64;
            const uint8_t Width = Wide ? 8 : 4;
            const uint8_t Frame = Wide ? 40 : 16;
            const uint8_t Raise = Wide ? 96 : 32;
            BinaryImage Image;
            Image.Arch = Architecture;
            Image.Bits = Wide ? Bitness::Bits64 : Bitness::Bits32;
            Image.Format = Format;
            Image.Entry = 0x1000;
            std::vector<uint8_t> Caller;
            const auto Adjust = [&](bool Add, uint8_t Bytes) {
              if (Wide)
                Caller.push_back(0x48);
              Caller.insert(Caller.end(),
                            {0x83, uint8_t(Add ? 0xc4 : 0xec), Bytes});
            };
            Adjust(false, Frame);
            if (Placement)
              Adjust(true, Raise);
            const uint32_t Relative =
                0x1100 - (Image.Entry + Caller.size() + 5);
            Caller.push_back(0xe8);
            for (unsigned I = 0; I != 4; ++I)
              Caller.push_back(Relative >> (I * 8));
            if (FPRead)
              Caller.insert(Caller.end(), {0xf3, 0x0f, 0x2c}); // cvttss2si eax
            else {
              if (Wide)
                Caller.push_back(0x48);
              Caller.push_back(0x8b); // mov eax/rax
            }
            Caller.insert(Caller.end(),
                          {0x44, 0x24,
                           uint8_t(Placement == 2 ? 0 : -Width)}); // [sp-slot]
            if (Placement)
              Adjust(false, Raise);
            Adjust(true, Frame);
            Caller.push_back(0xc3);

            Segment Text;
            Text.Name = ".text";
            Text.VA = Image.Entry;
            Text.Size = Text.FileSz = 0x10a;
            Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
            Text.Data.assign(Text.Size, 0xcc);
            std::copy(Caller.begin(), Caller.end(), Text.Data.begin());
            // A visible store retains the CALL. The callee changes no stack
            // argument; only the hardware's return-PC push can explain the
            // difference between placements 1 and 2.
            const std::vector<uint8_t> Callee = {
                0xc6,
                0x05,
                uint8_t(Wide ? 0xf9 : 0x00),
                uint8_t(Wide ? 0x00 : 0x12),
                0x00,
                0x00,
                0x01,
                0x31,
                0xc0,
                0xc3}; // mov byte [0x1200],1; xor eax,eax; ret
            std::copy(Callee.begin(), Callee.end(), Text.Data.begin() + 0x100);
            Image.Segments.push_back(std::move(Text));
            Segment Data;
            Data.Name = ".data";
            Data.VA = 0x1200;
            Data.Size = Data.FileSz = 1;
            Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
            Data.Data = {0};
            Image.Segments.push_back(std::move(Data));
            Image.Symbols = {Symbol::makeFunc(0x1000, Caller.size()),
                             Symbol::makeFunc(0x1100, Callee.size())};
            PipelineOptions Options;
            Options.LiftMode = Options.SourceProjection = LLVMSource;
            Options.NoOpt = true;
            Options.OnlyFunctionEntries = {0x1000, 0x1100};
            llvm::LLVMContext Context;
            const auto Result = Pipeline().run(Image, Context, Options);
            ASSERT_EQ(Result.Success, Placement != 1) << Result.Error;
            if (Placement == 1) {
              EXPECT_NE(Result.Error.find("physical CALL return slot"),
                        std::string::npos)
                  << Result.Error;
            } else if (LLVMSource) {
              ASSERT_NE(Result.LlvmModule, nullptr);
              EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule));
            } else {
              EXPECT_EQ(Result.HighFuncs.size(), 2u);
            }
          }
}

} // namespace

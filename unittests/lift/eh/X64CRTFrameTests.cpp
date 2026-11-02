//===- X64CRTFrameTests.cpp - Win64 CRT borrowed output cells ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks concrete CRT output extents independently of saved-frame identities.

#include "../../../lib/ir/low/X86/FrameEffects.h"
#include "PipelineSEHRuntimeMemory.h"
#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/X86/X87Constant.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/SEHFrameProof.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

using namespace neverd;

namespace {

constexpr va_t Entry = 0x140001000, Unwind = Entry + 0x200;
constexpr va_t ControlIAT = Entry + 0x1000;

struct ControlFixture {
  BinaryImage Image;
  va_t RuntimeCall = InvalidVA;
  va_t UnwindCall = InvalidVA;

  ControlFixture(int32_t OutputOffset, uint32_t Mask, bool OpaqueCall = false,
                 llvm::ArrayRef<uint8_t> FloatingBytes = {}) {
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::COFF;
    Image.Base = Entry - 0x1000;
    Image.Entry = Entry;
    std::vector<uint8_t> Code{0x48, 0x83, 0xec, 0x78}; // sub rsp,120
    const auto Word = [&](uint32_t Value) {
      for (unsigned I = 0; I != 4; ++I)
        Code.push_back(Value >> (I * 8));
    };
    const auto Call = [&](va_t Target) {
      Code.push_back(0xe8);
      Word(Target - (Entry + Code.size() + 4));
    };
    if (OpaqueCall)
      Call(Unwind);
    Code.insert(Code.end(), {0x48, 0x89, 0x64, 0x24, 0x68, // saved SP
                             0x48, 0x8d, 0x8c, 0x24});     // output
    Word(OutputOffset);
    Code.insert(Code.end(), {0x31, 0xd2, 0x41, 0xb8}); // new=0; r8d=mask
    Word(Mask);
    RuntimeCall = Entry + Code.size();
    Code.insert(Code.end(), {0xff, 0x15});
    Word(ControlIAT - (Entry + Code.size() + 4));
    Code.insert(Code.end(), FloatingBytes.begin(), FloatingBytes.end());
    Code.insert(Code.end(), {0x48, 0x8b, 0x4c, 0x24, 0x68});
    UnwindCall = Entry + Code.size();
    Call(Unwind);
    Code.insert(Code.end(), {0x48, 0x83, 0xc4, 0x78, 0xc3});
    Segment Text;
    Text.VA = Entry;
    Text.Size = Text.FileSz = Code.size();
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data = std::move(Code);
    Image.Symbols.push_back(Symbol::makeFunc(Entry, Text.Size));
    Image.Segments.push_back(std::move(Text));
    Segment IAT;
    IAT.VA = ControlIAT;
    IAT.Size = IAT.FileSz = 8;
    IAT.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    IAT.Data.assign(8, 0);
    Image.Segments.push_back(std::move(IAT));
    for (const auto &Segment : Image.Segments) {
      Section Section;
      Section.VA = Segment.VA;
      Section.Size = Segment.Size;
      Section.FileSz = Segment.FileSz;
      Section.Flags = Segment.Flags;
      Image.Sections.push_back(std::move(Section));
    }
    Image.Imports.push_back(
        {"api-ms-win-crt-runtime-l1-1-0.dll", "_controlfp_s", 0, ControlIAT});
    EXPECT_TRUE(Image.recordImportStorageSlot(
        ControlIAT, "_controlfp_s", 0, ImportStorageEvidence::ImportDirectory));
  }

  LowFunc lift() const {
    Decoder Dec;
    EXPECT_TRUE(Dec.init(Arch::X64));
    return CFGBuilder().build(Image, Dec, Entry, "control_caller");
  }

  LowSEHFrameProof prove(const LowFunc &Caller) const {
    const auto Memory = collectLowSEHRuntimeMemory(Image, {Caller});
    size_t Work = 100000;
    return proveLowSEHFrames(Caller, getTargetRegInfo(Arch::X64), Work, nullptr,
                             &Memory);
  }
};

} // namespace

TEST(X64CRTFrame, ControlFPInvalidatesOnlyItsBoundedOutput) {
  // The saved pointer occupies [104,112). The output may touch either end,
  // but a one-byte overlap must withdraw the complete pointer identity.
  for (int32_t Offset : {32, 96, 100, 101, 104, 108, 111, 112, 116, 117, 120,
                         124, 0, 28, 31, -4}) {
    SCOPED_TRACE(Offset);
    ControlFixture F(Offset, 0x8001f);
    auto Caller = F.lift();
    ASSERT_TRUE(Caller.hasCompleteLiftCoverage());
    auto Proof = F.prove(Caller);
    const bool Fits = Offset >= 32 && Offset <= 116;
    const bool Overlaps = Offset < 112 && Offset + 4 > 104;
    if (!Fits || Overlaps) {
      EXPECT_TRUE(Proof.Calls.empty());
      continue;
    }
    ASSERT_EQ(Proof.Calls.size(), 1u);
    EXPECT_EQ(Proof.Calls.begin()->first.first, F.UnwindCall);
    EXPECT_EQ(Proof.Calls.begin()->second, -120);
    EXPECT_TRUE(Proof.UsesRuntimeMemory);
  }
}

TEST(X64CRTFrame, FloatingInstructionsKeepTheIntegerFrameProof) {
  const std::vector<uint8_t> Floating{0x9b,                   // wait
                                      0xf2, 0x0f, 0x59, 0xc1, // mulsd xmm0,xmm1
                                      0xf2, 0x0f, 0x59, 0x44,
                                      0x24, 0x60}; // mulsd xmm0,[rsp+96]
  ControlFixture F(96, 0, false, Floating);
  const auto Original = F.lift();
  ASSERT_TRUE(Original.hasCompleteLiftCoverage());
  for (const auto &Block : Original.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::INTRINSIC)
        ASSERT_TRUE(isX86FramePreservingIntrinsic(Op, Arch::X64))
            << intrinsicName(static_cast<Intrinsic>(Op.Inputs[0].Offset))
            << " at " << Op.Addr;
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Caller = Original;
    if (Mutation == 7) {
      Caller.ExceptionMetadata.emplace();
      Caller.ExceptionMetadata->SEH.emplace();
    }
    bool Changed = false;
    for (auto &Block : Caller.Blocks)
      for (auto &Op : Block.Ops) {
        if (Changed || Op.Opcode != NdOp::INTRINSIC || !Op.NumInputs ||
            Op.Inputs[0] !=
                NdVar::cst(static_cast<unsigned>(Intrinsic::X86ReadMXCSR), 2))
          continue;
        if (Mutation == 1)
          Op.Output = NdVar::reg(x86reg::RSP, 4);
        if (Mutation == 2)
          Op.Output = NdVar::reg(x86reg::R8, 4);
        if (Mutation == 3)
          Op.Output = NdVar::reg(x86reg::R16, 4);
        if (Mutation == 4)
          Op.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
        if (Mutation == 5)
          Op.Inputs[0].Size = 4;
        if (Mutation == 6)
          Op.NumInputs = 0;
        Changed = true;
      }
    ASSERT_TRUE(Changed);
    const auto Proof = F.prove(Caller);
    if (Mutation)
      EXPECT_TRUE(Proof.Calls.empty());
    else {
      ASSERT_EQ(Proof.Calls.size(), 1u);
      EXPECT_EQ(Proof.Calls.begin()->first.first, F.UnwindCall);
    }
  }
}

TEST(X64CRTFrame, X87ConstantsAndControlKeepTheIntegerFrameProof) {
  for (uint8_t Selector = 0; Selector != 7; ++Selector)
    for (bool Overlap : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << unsigned(Selector) << ':' << Overlap);
      const uint8_t Slot = Overlap ? 104 : 96;
      const std::vector<uint8_t> Floating{
          0xd9, uint8_t(0xe8 + Selector), // fld1 through fldz
          0xdd, 0xd8,                     // fstp st(0)
          0xd9, 0x7c,
          0x24, Slot, // fnstcw [rsp+Slot]
          0xd9, 0x6c,
          0x24, Slot}; // fldcw [rsp+Slot]
      ControlFixture F(96, 0, false, Floating);
      const auto Caller = F.lift();
      ASSERT_TRUE(Caller.hasCompleteLiftCoverage());
      const auto Proof = F.prove(Caller);
      // The explicit FNSTCW store still invalidates an overlapping saved SP.
      // The floating instructions never certify its memory destination.
      if (Overlap)
        EXPECT_TRUE(Proof.Calls.empty());
      else {
        ASSERT_EQ(Proof.Calls.size(), 1u);
        EXPECT_EQ(Proof.Calls.begin()->first.first, F.UnwindCall);
        EXPECT_EQ(Proof.Calls.begin()->second, -120);
      }
    }
}

TEST(X64CRTFrame, X87FrameEffectsRejectIntegerAndMisalignedOutputs) {
  const std::vector<uint8_t> Floating{0xd9, 0xeb, 0xdd, 0xd8}; // fldpi; fstp
  ControlFixture F(96, 0, false, Floating);
  const auto Caller = F.lift();
  ASSERT_TRUE(Caller.hasCompleteLiftCoverage());
  unsigned Checked = 0;
  for (const auto &Block : Caller.Blocks)
    for (const auto &Original : Block.Ops) {
      if (Original.Opcode != NdOp::INTRINSIC || !Original.NumInputs ||
          !Original.Inputs[0].isConst())
        continue;
      const auto Id = static_cast<Intrinsic>(Original.Inputs[0].Offset);
      if (Id != Intrinsic::X87ReadControl && Id != Intrinsic::X87LoadConstant)
        continue;
      ++Checked;
      for (Arch Architecture : {Arch::X86, Arch::X64}) {
        EXPECT_TRUE(isX86FramePreservingIntrinsic(Original, Architecture));
        for (uint64_t Register :
             {x86reg::RSP, x86reg::RBP, x86reg::R8, x86reg::R16,
              x86reg::ST0 + 1, x86reg::ST7 + x86reg::FPURegStride}) {
          auto Changed = Original;
          Changed.Output = NdVar::reg(Register, Original.Output.Size);
          // The numerical shape alone permits these writable values. The
          // frame policy must separately reject integer and non-slot writes.
          ASSERT_TRUE(x87ConstantControlShapeIsValid(
              Id, x87ConstantControlLowShape(Changed, Architecture)));
          EXPECT_FALSE(isX86FramePreservingIntrinsic(Changed, Architecture));
        }
      }
    }
  EXPECT_EQ(Checked, 2u);
}

TEST(X64CRTFrame, ControlFPRequiresAnAuthenticatedReturningInvocation) {
  for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
    SCOPED_TRACE(Mutation);
    const auto Mask = Mutation == 5 ? 0x30000u : 0u;
    ControlFixture F(96, Mask, Mutation == 7);
    if (Mutation == 1)
      F.Image.Imports[0].Module = "application.dll";
    if (Mutation == 2)
      F.Image.Imports[0].IsDelayImport = true;
    if (Mutation == 3)
      F.Image.ImportStorageSlots.clear();
    if (Mutation == 4)
      F.Image.Imports[0].Name = "same_abi_only";
    if (Mutation == 8)
      F.Image.Imports[0].Module = "api-ms-win-crt-stdio-l1-1-0.dll";
    if (Mutation == 9)
      F.Image.Sections.back().FileSz = 7;
    if (Mutation == 10)
      F.Image.Imports.push_back({"other.dll", "_controlfp_s", 0, ControlIAT});
    auto Caller = F.lift();
    ASSERT_TRUE(Caller.hasCompleteLiftCoverage());
    for (auto &Block : Caller.Blocks)
      for (auto &Op : Block.Ops) {
        if (Mutation == 6 && Op.Output == NdVar::reg(x86reg::R8, 4) &&
            Op.Opcode == NdOp::COPY)
          Op.Inputs[0] = NdVar::reg(x86reg::R9, 4); // Unknown mask
        if (Mutation == 11 && Op.Addr == F.RuntimeCall &&
            Op.Opcode == NdOp::INDIR_CALL)
          Op.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      }
    auto Proof = F.prove(Caller);
    if (Mutation)
      EXPECT_TRUE(Proof.Calls.empty());
    else {
      ASSERT_EQ(Proof.Calls.size(), 1u);
      EXPECT_EQ(Proof.Calls.begin()->first.first, F.UnwindCall);
    }
  }
}

TEST(X64CRTFrame, ControlFPProviderIdentityDoesNotBorrowPE32Contracts) {
  struct Case {
    const char *Module;
    bool Accepted;
  };
  const Case Cases[] = {{"UCRTBASE.DLL", true},
                        {"ucrtbased.dll", true},
                        {"api-ms-win-crt-runtime-l1-1-0.dll", true},
                        {"api-ms-win-crt-math-l1-1-0.dll", true},
                        {"msvcrt.dll", false},
                        {"msvcrtd.dll", false},
                        {"msvcr100.dll", false},
                        {"api-ms-win-crt-stdio-l1-1-0.dll", false}};
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Module);
    ControlFixture F(96, 0);
    F.Image.Imports[0].Module = C.Module;
    const auto Caller = F.lift();
    ASSERT_TRUE(Caller.hasCompleteLiftCoverage());
    const auto Proof = F.prove(Caller);
    if (!C.Accepted) {
      EXPECT_TRUE(Proof.Calls.empty());
      continue;
    }
    ASSERT_EQ(Proof.Calls.size(), 1u);
    EXPECT_EQ(Proof.Calls.begin()->first.first, F.UnwindCall);
    EXPECT_TRUE(Proof.UsesRuntimeMemory);
  }
}

//===- RegistrationSEHPrivateCallTests.cpp - Live capture callees ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/low/X86/RegistrationSEHPrivateCall.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"

using namespace neverd;
using namespace neverd::registration_state;

namespace {
struct PrivateCallImage {
  BinaryImage Image;
  LowFunc Function;
  static constexpr va_t Entry = 0x401000;
  static constexpr va_t Callee = 0x401100;
  static constexpr va_t Child = 0x401200;
  static constexpr va_t Table = 0x402000;
  static constexpr va_t Data = 0x403000;

  PrivateCallImage() {
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Image.Base = 0x400000;
    Image.Entry = Entry;
    addSegment(Entry, 0x300, SegmentFlags::Readable | SegmentFlags::Executable);
    addSegment(Table, 0x20, SegmentFlags::Readable);
    addSegment(Data, 0x20, SegmentFlags::Readable | SegmentFlags::Writable);
    Image.Symbols = {Symbol::makeFunc(Entry, 0x20),
                     Symbol::makeFunc(Callee, 0x100),
                     Symbol::makeFunc(Child, 0x100)};
    install(Entry, {0xe8, 0xfb, 0, 0, 0});
    install(Callee, {0xb8, 7, 0, 0, 0, 0xc3});
    install(Child, {0xb8, 7, 0, 0, 0, 0xc3});
    Function.Entry = Entry;
    auto &EH = Function.ExceptionMetadata.emplace();
    EH.CodeRange = {Entry, Entry + 0x20};
    EH.Encoding = ExceptionEncoding::X86ScopeTableEH3;
    EH.Personality = ExceptionPersonality::ExceptHandler3;
    auto &Chain = EH.Registration.emplace();
    Chain.ScopeTableVA = Table;
    Chain.Scopes = {{-1, 0, Entry + 0x10, true}};
    auto &Block = Function.Blocks.emplace_back();
    Block.Id = 0;
    Block.StartAddr = Entry;
    Block.EndAddr = Entry + 5;
    auto &Call = Block.Ops.emplace_back();
    Call.Addr = Entry;
    Call.Seq = 0;
    Call.Opcode = NdOp::CALL;
    Call.Output = NdVar::reg(x86reg::RAX, 4);
    Call.addInput(NdVar::cst(Callee, 4));
    Block.InstructionBoundaries = {{Entry, 5, 0, 1}};
    auto &Source = Block.InstructionBoundaries.front();
    Source.Control = LowInstructionControl::Call;
    Source.ControlFlags = LowInstructionControlFlag::Call;
    Source.Immediate = Callee;
  }

  void addSegment(va_t Address, size_t Size, SegmentFlags Flags) {
    Segment Segment;
    Segment.VA = Address;
    Segment.Size = Segment.FileSz = Size;
    Segment.Flags = Flags;
    Segment.Data.assign(Size, 0xcc);
    Section Section;
    Section.VA = Address;
    Section.Size = Section.FileSz = Size;
    Section.Flags = Flags;
    Image.Sections.push_back(Section);
    Image.Segments.push_back(std::move(Segment));
  }

  void install(va_t Address, llvm::ArrayRef<uint8_t> Bytes) {
    auto *Out = const_cast<uint8_t *>(Image.readVA(Address, Bytes.size()));
    ASSERT_TRUE(Out);
    llvm::copy(Bytes, Out);
  }

  bool prove(size_t &Work) {
    SEHPrivateCallProof Proof(Image, Function);
    return Proof.preservesFrame(Function.Blocks[0].Ops[0], Work);
  }
};
} // namespace

TEST(RegistrationSEHPrivateCall,
     PreservesAFrameAcrossLeafAndPrivateGraphCalls) {
  for (bool Graph : {false, true}) {
    PrivateCallImage F;
    if (Graph)
      F.install(F.Callee, {0xe8, 0xfb, 0, 0, 0, 0xc3});
    size_t Work = 0;
    SEHPrivateCallProof Proof(F.Image, F.Function);
    EXPECT_TRUE(Proof.preservesFrame(F.Function.Blocks[0].Ops[0], Work));
    EXPECT_GT(Work, 0u);
    const auto Before = Work;
    EXPECT_TRUE(Proof.preservesFrame(F.Function.Blocks[0].Ops[0], Work));
    EXPECT_EQ(Work, Before + 1);
  }
}

TEST(RegistrationSEHPrivateCall, RejectsFrameObserversAndUnboundedEffects) {
  const std::pair<const char *, std::vector<uint8_t>> Mutations[] = {
      {"caller stack", {0x8b, 0x44, 0x24, 4, 0xc3}},
      {"caller EBP", {0x8b, 0x45, 0xa0, 0xc3}},
      {"borrowed object", {0xc7, 0x01, 7, 0, 0, 0, 0xc3}},
      {"callee cleanup", {0xc2, 4, 0}},
      {"nonvolatile clobber", {0x31, 0xdb, 0xc3}},
      {"unbalanced stack", {0x6a, 7, 0xc3}},
      {"opaque call", {0xff, 0xd0, 0xc3}},
      {"recursive call", {0xe8, 0xfb, 0xff, 0xff, 0xff, 0xc3}},
      {"registration chain", {0x64, 0xa1, 0, 0, 0, 0, 0xc3}},
      {"scope table write", {0xc7, 0x05, 0, 0x20, 0x40, 0, 7, 0, 0, 0, 0xc3}},
      {"code write", {0xc7, 0x05, 0, 0x12, 0x40, 0, 7, 0, 0, 0, 0xc3}},
      {"caller PC readback",
       {0x8b, 4, 0x24, 0xa3, 0, 0x30, 0x40, 0, 0xa1, 0, 0x30, 0x40, 0, 0xc3}},
  };
  for (const auto &[Name, Bytes] : Mutations) {
    SCOPED_TRACE(Name);
    for (bool Graph : {false, true}) {
      SCOPED_TRACE(Graph);
      PrivateCallImage F;
      F.Image.Segments[1].Flags =
          F.Image.Segments[1].Flags | SegmentFlags::Writable;
      F.Image.Sections[1].Flags =
          F.Image.Sections[1].Flags | SegmentFlags::Writable;
      if (Graph)
        F.install(F.Callee, {0xe8, 0xfb, 0, 0, 0, 0xc3});
      F.install(Graph ? F.Child : F.Callee, Bytes);
      size_t Work = 0;
      EXPECT_FALSE(F.prove(Work));
      EXPECT_LE(Work, limits::kMaxRegistrationEHStateWork);
    }
  }
}

TEST(RegistrationSEHPrivateCall, BindsTheCurrentSourceBytesAndCallSlice) {
  for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
    SCOPED_TRACE(Mutation);
    PrivateCallImage F;
    auto &Op = F.Function.Blocks[0].Ops[0];
    auto &Source = F.Function.Blocks[0].InstructionBoundaries[0];
    size_t Work = 0;
    switch (Mutation) {
    case 0:
      F.install(F.Entry, {0x90});
      break;
    case 1:
      Op.Inputs[0].Offset = F.Child;
      break;
    case 2:
      Source.OpCount = 0;
      break;
    case 3:
      Source.Immediate = F.Child;
      break;
    case 4:
      Source.ControlFlags |= LowInstructionControlFlag::NoReturn;
      break;
    case 5:
      Source.ControlFlags |= LowInstructionControlFlag::Conditional;
      break;
    case 6:
      Source.ControlFlags |= LowInstructionControlFlag::InstructionGuard;
      break;
    case 7:
      Op.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 8:
      F.Image.Segments[0].Flags =
          F.Image.Segments[0].Flags | SegmentFlags::Writable;
      F.Image.Sections[0].Flags =
          F.Image.Sections[0].Flags | SegmentFlags::Writable;
      break;
    case 9:
      Work = limits::kMaxRegistrationEHStateWork;
      break;
    case 10:
      F.Image.IsRelocatable = true;
      break;
    }
    EXPECT_FALSE(F.prove(Work));
  }
  PrivateCallImage F;
  size_t Work = 0;
  SEHPrivateCallProof Proof(F.Image, F.Function);
  const auto Detached = F.Function.Blocks[0].Ops[0];
  EXPECT_FALSE(Proof.preservesFrame(Detached, Work));
}

TEST(RegistrationSEHPrivateCall,
     PreservingCapturesDoesNotWaiveCallerPCReadback) {
  PrivateCallImage F;
  F.install(F.Callee, {0x8b, 4, 0x24, 0xa3, 0, 0x30, 0x40, 0, 0xc3});
  size_t Work = 0;
  ASSERT_TRUE(F.prove(Work));
  auto &States = F.Function.RegistrationStates.emplace();
  States.ImageReadsComplete = true;
  EXPECT_TRUE(hasCallerCleanupRegistrationABI(F.Function, F.Image));
  States.ImageReads.push_back({F.Data, F.Data + 4});
  EXPECT_FALSE(hasCallerCleanupRegistrationABI(F.Function, F.Image));
}

TEST(RegistrationSEHPrivateCall, RechecksChangedCalleeBytesAndImportStorage) {
  PrivateCallImage F;
  size_t Work = 0;
  ASSERT_TRUE(F.prove(Work));
  F.install(F.Callee, {0xc7, 0x05, 0, 0x30, 0x40, 0, 7, 0, 0, 0, 0xc3});
  Work = 0;
  ASSERT_TRUE(F.prove(Work));
  auto &Import = F.Image.Imports.emplace_back();
  Import.IATAddr = F.Data;
  Import.Name = "_longjmpex";
  Import.Module = "vcruntime140.dll";
  Work = 0;
  EXPECT_FALSE(F.prove(Work));
}

TEST(RegistrationSEHPrivateCall, EH4CalleeGraphsPreserveTheImageCookie) {
  for (bool Graph : {false, true})
    for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
      SCOPED_TRACE(Graph);
      SCOPED_TRACE(Mutation);
      PrivateCallImage F;
      auto &EH = *F.Function.ExceptionMetadata;
      EH.Personality = ExceptionPersonality::ExceptHandler4;
      EH.Encoding = ExceptionEncoding::X86ScopeTableEH4;
      EH.Registration->SeededTryLevel = -2;
      EH.Registration->Scopes.front().EnclosingLevel = -2;
      F.Image.DynInfo.SecurityCookieRVA = F.Data + 4 - F.Image.Base;
      if (Graph)
        F.install(F.Callee, {0xe8, 0xfb, 0, 0, 0, 0xc3});
      if (Mutation >= 1 && Mutation <= 5) {
        const va_t Address = Mutation == 1   ? F.Data
                             : Mutation == 2 ? F.Data + 4
                             : Mutation == 3 ? F.Data + 3
                             : Mutation == 4 ? F.Data + 7
                                             : F.Table;
        F.install(Graph ? F.Child : F.Callee,
                  {0xc7, 0x05, uint8_t(Address), uint8_t(Address >> 8),
                   uint8_t(Address >> 16), uint8_t(Address >> 24), 7, 0, 0, 0,
                   0xb8, 7, 0, 0, 0, 0xc3});
        if (Mutation == 5) {
          F.Image.Segments[1].Flags =
              F.Image.Segments[1].Flags | SegmentFlags::Writable;
          F.Image.Sections[1].Flags =
              F.Image.Sections[1].Flags | SegmentFlags::Writable;
        }
      }
      if (Mutation == 6)
        F.Image.DynInfo.SecurityCookieRVA = 0;
      if (Mutation == 7)
        F.Image.DynInfo.SecurityCookieRVA = UINT32_MAX - F.Image.Base;
      if (Mutation == 8)
        F.Image.Base = uint64_t(UINT32_MAX) + 1;
      size_t Work = 0;
      EXPECT_EQ(F.prove(Work), Mutation < 2);
    }
}
